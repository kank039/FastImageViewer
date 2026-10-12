#include "Folder.h"

#include <shlwapi.h>

#include <algorithm>
#include <unordered_map>

#include "../core/FileGroups.h"
#include "../core/Pixels.h"
#include "System.h"

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

// Explorer order; subfolder files after the top level ones, folder by folder.
bool NameLess(const FileEntry& a, const FileEntry& b) {
    const bool subA = a.rel.size() != a.name.size(), subB = b.rel.size() != b.name.size();
    if (subA != subB) return subB;
    return StrCmpLogicalW(a.rel.c_str(), b.rel.c_str()) < 0;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring Join(const std::wstring& folder, const std::wstring& name) {
    if (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/')) return folder + name;
    return folder + L'\\' + name;
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

struct Scanned {
    std::wstring dir;              // relative folder ("" or "sub\\deeper")
    std::wstring name, ext, stem;  // stem lower-cased, includes dir so pairs never span folders
    uint64_t size = 0, mtime = 0;
    uint32_t attrs = 0;
};

namespace {

std::wstring StemOf(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

std::wstring ParentRel(const std::wstring& rel) {
    const size_t slash = rel.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : rel.substr(0, slash);
}

void Scan(const std::wstring& pattern, const std::wstring& dir, std::vector<Scanned>& out,
          std::vector<std::wstring>* subdirs = nullptr) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (subdirs && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
            subdirs->push_back(dir.empty() ? std::wstring(fd.cFileName) : dir + L'\\' + fd.cFileName);
            continue;
        }
        if (!Listable(fd.dwFileAttributes)) continue;
        Scanned s;
        s.dir = dir;
        s.name = fd.cFileName;
        s.ext = ExtOf(s.name);
        s.stem = ToLower(dir.empty() ? StemOf(s.name) : dir + L'\\' + StemOf(s.name));
        s.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        s.mtime = FileTimeToU64(fd.ftLastWriteTime);
        s.attrs = fd.dwFileAttributes;
        out.push_back(std::move(s));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

}  // namespace

std::vector<FileEntry> FolderModel::Build(const std::vector<Scanned>& scanned, const FormatRegistry& formats,
                                          std::unordered_map<std::wstring, FileEntry>& old, uint64_t now) const {
    std::vector<core::GroupInput> in;
    in.reserve(scanned.size());
    for (const auto& s : scanned) in.push_back({s.stem, formats.Classify(s.ext)});
    std::vector<FileEntry> out;
    for (const auto& g : core::GroupFiles(in, {pairRaw_, formats.RawEnabled()})) {
        const Scanned& s = scanned[g.primary];
        FileEntry e;
        e.name = s.name;
        e.ext = s.ext;
        e.rel = s.dir.empty() ? s.name : s.dir + L'\\' + s.name;
        e.path = Join(folder_, e.rel);
        e.size = s.size;
        e.mtime = s.mtime;
        e.attrs = s.attrs;
        auto it = old.find(ToLower(e.path));
        if (it != old.end() && it->second.size == e.size && it->second.mtime == e.mtime) {
            FileEntry kept = std::move(it->second);
            kept.attrs = e.attrs;
            e = std::move(kept);
        } else {
            e.id = g_nextId++;
            e.changedAt = now;
        }
        e.companions.clear();
        for (size_t c : g.companions)
            e.companions.push_back(Join(folder_, scanned[c].dir.empty() ? scanned[c].name : scanned[c].dir + L'\\' + scanned[c].name));
        out.push_back(std::move(e));
    }
    return out;
}

bool FolderModel::Load(const std::wstring& folder, const FormatRegistry& formats) {
    std::unordered_map<std::wstring, FileEntry> old;
    if (SamePath(folder, folder_))
        for (auto& e : files_) old.emplace(ToLower(e.path), std::move(e));
    folder_ = folder;
    files_.clear();
    truncated_ = false;
    if (!IsDirectory(folder)) return false;
    std::vector<Scanned> scanned;
    if (!recursive_) {
        Scan(Join(folder, L"*"), L"", scanned);
    } else {
        // Breadth first, so a huge tree still shows its top levels.
        std::vector<std::wstring> level{L""};
        for (int depth = 0; depth <= kMaxDepth && !level.empty() && !truncated_; ++depth) {
            std::vector<std::wstring> next;
            for (const auto& dir : level) {
                if (!dir.empty() && Excluded(dir)) continue;
                Scan(Join(folder, dir.empty() ? L"*" : dir + L"\\*"), dir, scanned, depth < kMaxDepth ? &next : nullptr);
                if (scanned.size() >= kMaxFiles) {
                    truncated_ = true;
                    break;
                }
            }
            level.swap(next);
        }
    }
    files_ = Build(scanned, formats, old, 0);
    std::sort(files_.begin(), files_.end(), NameLess);
    return true;
}

void FolderModel::SetRecursive(bool on, std::vector<std::wstring> excluded) {
    recursive_ = on;
    excluded_.clear();
    for (auto& e : excluded) excluded_.push_back(ToLower(e));
}

bool FolderModel::Excluded(const std::wstring& dirRel) const {
    if (dirRel.empty()) return false;
    const std::wstring full = ToLower(Join(folder_, dirRel));
    for (const auto& e : excluded_)
        if (full == e || (full.size() > e.size() && full.compare(0, e.size(), e) == 0 && full[e.size()] == L'\\'))
            return true;
    return false;
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

bool FolderModel::Refresh(const std::wstring& name, const FormatRegistry& formats, uint64_t now, bool* needsReload) {
    const std::wstring dir = ParentRel(name);
    if (!dir.empty() && (!recursive_ || Excluded(dir))) return false;
    if (recursive_) {
        // A folder appeared, vanished or was renamed: rescan instead of guessing.
        const DWORD attrs = GetFileAttributesW(Join(folder_, name).c_str());
        bool knownDir = false;
        const std::wstring prefix = ToLower(name) + L'\\';
        for (const auto& e : files_)
            if (ToLower(e.rel).compare(0, prefix.size(), prefix) == 0) {
                knownDir = true;
                break;
            }
        if ((attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) || knownDir) {
            if (needsReload) *needsReload = true;
            return false;
        }
    }
    // Everything sharing this name's stem is regrouped together, so a RAW
    // appearing or a JPEG disappearing updates the pairing correctly.
    const std::wstring stem = StemOf(name), stemLower = ToLower(stem);
    auto signature = [&] {
        std::wstring sig;
        for (const auto& e : files_) {
            if (ToLower(StemOf(e.rel)) != stemLower) continue;
            sig += e.path + L'|' + std::to_wstring(e.id) + L'|' + std::to_wstring(e.attrs) + L'|';
            for (const auto& c : e.companions) sig += c + L';';
            sig += L'\n';
        }
        return sig;
    };
    const std::wstring before = signature();

    std::unordered_map<std::wstring, FileEntry> old;
    for (auto it = files_.begin(); it != files_.end();) {
        if (ToLower(StemOf(it->rel)) == stemLower) {
            old.emplace(ToLower(it->path), std::move(*it));
            it = files_.erase(it);
        } else {
            ++it;
        }
    }
    std::vector<Scanned> scanned;
    Scan(Join(folder_, stem + L".*"), dir, scanned);
    if (stem == name) Scan(Join(folder_, name), dir, scanned);  // extensionless name
    scanned.erase(std::remove_if(scanned.begin(), scanned.end(), [&](const Scanned& s) { return s.stem != stemLower; }),
                  scanned.end());
    for (auto& e : Build(scanned, formats, old, now))
        files_.insert(std::upper_bound(files_.begin(), files_.end(), e, NameLess), std::move(e));
    return signature() != before;
}

// ---- DirWatcher -------------------------------------------------------------

void DirWatcher::Start(const std::wstring& folder, HWND hwnd, UINT msg, bool subtree) {
    Stop();
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        std::lock_guard lk(m_);
        names_.clear();
        overflow_ = false;
    }
    thread_ = std::thread([this, folder, hwnd, msg, subtree] { Run(folder, hwnd, msg, subtree); });
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

void DirWatcher::Run(std::wstring folder, HWND hwnd, UINT msg, bool subtree) {
    HANDLE dir = CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE) return;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<DWORD> buf(16 * 1024);  // 64 KB, DWORD aligned (network shares cap at 64 KB)
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                         FILE_NOTIFY_CHANGE_ATTRIBUTES | (subtree ? FILE_NOTIFY_CHANGE_DIR_NAME : 0);
    auto issue = [&] {
        ResetEvent(ov.hEvent);
        return ReadDirectoryChangesW(dir, buf.data(), DWORD(buf.size() * sizeof(DWORD)), subtree, filter, nullptr,
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
