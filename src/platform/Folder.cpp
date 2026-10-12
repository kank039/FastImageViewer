#include "Folder.h"

#include <shlwapi.h>

#include <algorithm>
#include <unordered_map>

#include "../core/Pixels.h"

#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif

namespace fiv {

namespace {

std::atomic<uint64_t> g_nextId{1};

uint64_t FileTimeToU64(const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

std::wstring ExtOf(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? std::wstring() : ToLower(name.substr(dot));
}

bool Listable(DWORD attrs) {
    return !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
}

bool NameLess(const FileEntry& a, const FileEntry& b) { return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0; }

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring Join(const std::wstring& folder, const std::wstring& name) {
    if (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/')) return folder + name;
    return folder + L'\\' + name;
}

void ResetProbe(FileEntry& e) {
    e.probed = false;
    e.probeHr = S_OK;
    e.stored = {};
    e.orientation = 1;
    e.canPrescale = false;
    e.error.clear();
    e.retryAt = 0;
}

}  // namespace

core::Size FileEntry::Oriented() const { return core::OrientedSize(stored, orientation); }

bool FileEntry::CloudOnly() const {
    return (attrs & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE)) != 0;
}

void FolderModel::Clear() {
    folder_.clear();
    files_.clear();
}

bool FolderModel::Load(const std::wstring& folder, const FormatRegistry& formats) {
    std::unordered_map<std::wstring, FileEntry> old;
    if (SamePath(folder, folder_))
        for (auto& e : files_) old.emplace(ToLower(e.path), std::move(e));
    folder_ = folder;
    files_.clear();

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(Join(folder, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (!Listable(fd.dwFileAttributes)) continue;
        FileEntry e;
        e.name = fd.cFileName;
        e.ext = ExtOf(e.name);
        if (!formats.IsEnabled(e.ext)) continue;
        e.path = Join(folder, e.name);
        e.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        e.mtime = FileTimeToU64(fd.ftLastWriteTime);
        e.attrs = fd.dwFileAttributes;
        auto it = old.find(ToLower(e.path));
        if (it != old.end() && it->second.size == e.size && it->second.mtime == e.mtime) {
            FileEntry kept = std::move(it->second);
            kept.attrs = e.attrs;
            e = std::move(kept);
        } else {
            e.id = g_nextId++;
        }
        files_.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(files_.begin(), files_.end(), NameLess);
    return true;
}

int FolderModel::IndexOfPath(const std::wstring& path) const {
    for (size_t i = 0; i < files_.size(); ++i)
        if (SamePath(files_[i].path, path)) return int(i);
    return -1;
}

FileEntry* FolderModel::FindById(uint64_t id) {
    for (auto& e : files_)
        if (e.id == id) return &e;
    return nullptr;
}

bool FolderModel::RemovePath(const std::wstring& path) {
    const int i = IndexOfPath(path);
    if (i < 0) return false;
    files_.erase(files_.begin() + i);
    return true;
}

bool FolderModel::Refresh(const std::wstring& name, const FormatRegistry& formats, uint64_t now) {
    const std::wstring path = Join(folder_, name);
    WIN32_FILE_ATTRIBUTE_DATA a{};
    const bool exists = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) != FALSE;
    const std::wstring ext = ExtOf(name);
    if (!exists || !Listable(a.dwFileAttributes) || !formats.IsEnabled(ext)) return RemovePath(path);

    const uint64_t size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    const uint64_t mtime = FileTimeToU64(a.ftLastWriteTime);
    const int i = IndexOfPath(path);
    if (i >= 0) {
        FileEntry& e = files_[size_t(i)];
        const bool attrsChanged = e.attrs != a.dwFileAttributes;
        e.attrs = a.dwFileAttributes;
        if (e.size == size && e.mtime == mtime) return attrsChanged;
        e.size = size;
        e.mtime = mtime;
        e.id = g_nextId++;
        e.changedAt = now;
        ResetProbe(e);
        return true;
    }
    FileEntry e;
    e.name = name;
    e.ext = ext;
    e.path = path;
    e.size = size;
    e.mtime = mtime;
    e.attrs = a.dwFileAttributes;
    e.id = g_nextId++;
    e.changedAt = now;
    files_.insert(std::upper_bound(files_.begin(), files_.end(), e, NameLess), std::move(e));
    return true;
}

// ---- DirWatcher -------------------------------------------------------------

void DirWatcher::Start(const std::wstring& folder, HWND hwnd, UINT msg) {
    Stop();
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        std::lock_guard lk(m_);
        names_.clear();
        overflow_ = false;
    }
    thread_ = std::thread([this, folder, hwnd, msg] { Run(folder, hwnd, msg); });
}

void DirWatcher::Stop() {
    if (stop_) SetEvent(stop_);
    if (thread_.joinable()) thread_.join();
    if (stop_) CloseHandle(stop_);
    stop_ = nullptr;
}

std::vector<std::wstring> DirWatcher::Take(bool& overflow) {
    std::lock_guard lk(m_);
    overflow = overflow_;
    overflow_ = false;
    std::vector<std::wstring> out(names_.begin(), names_.end());
    names_.clear();
    return out;
}

void DirWatcher::Run(std::wstring folder, HWND hwnd, UINT msg) {
    HANDLE dir = CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE) return;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<DWORD> buf(16 * 1024);  // 64 KB, DWORD aligned (network shares cap at 64 KB)
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                         FILE_NOTIFY_CHANGE_ATTRIBUTES;
    auto issue = [&] {
        ResetEvent(ov.hEvent);
        return ReadDirectoryChangesW(dir, buf.data(), DWORD(buf.size() * sizeof(DWORD)), FALSE, filter, nullptr,
                                     &ov, nullptr) != FALSE;
    };

    constexpr uint64_t kQuietMs = 300;
    uint64_t deadline = 0;  // 0: nothing pending
    if (issue()) {
        HANDLE handles[2] = {stop_, ov.hEvent};
        for (;;) {
            DWORD timeout = INFINITE;
            if (deadline) {
                const uint64_t now = NowMs();
                timeout = now >= deadline ? 0 : DWORD(deadline - now);
            }
            const DWORD r = WaitForMultipleObjects(2, handles, FALSE, timeout);
            if (r == WAIT_OBJECT_0) break;
            if (r == WAIT_OBJECT_0 + 1) {
                DWORD bytes = 0;
                if (!GetOverlappedResult(dir, &ov, &bytes, FALSE)) break;
                {
                    std::lock_guard lk(m_);
                    if (bytes == 0) {
                        overflow_ = true;
                    } else {
                        auto* p = reinterpret_cast<const uint8_t*>(buf.data());
                        for (;;) {
                            auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
                            names_.insert(std::wstring(info->FileName, info->FileNameLength / sizeof(wchar_t)));
                            if (!info->NextEntryOffset) break;
                            p += info->NextEntryOffset;
                        }
                    }
                }
                if (!deadline) deadline = NowMs() + kQuietMs;
                if (!issue()) break;
            } else if (r == WAIT_TIMEOUT) {
                deadline = 0;
                PostMessageW(hwnd, msg, 0, 0);
            } else {
                break;
            }
        }
        CancelIoEx(dir, &ov);
        DWORD ignored = 0;
        GetOverlappedResult(dir, &ov, &ignored, TRUE);
    }
    CloseHandle(ov.hEvent);
    CloseHandle(dir);
}

}  // namespace fiv
