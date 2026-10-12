#include "FileOps.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <shellapi.h>

#include <algorithm>
#include <atomic>

#include "../core/FileGroups.h"
#include "System.h"

namespace fiv {

namespace {

std::wstring ItemName(IShellItem* item, SIGDN kind) {
    PWSTR s = nullptr;
    std::wstring out;
    if (item && SUCCEEDED(item->GetDisplayName(kind, &s)) && s) out = s;
    CoTaskMemFree(s);
    return out;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring LeafName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Records, per source file, the outcome and where it ended up. Files are
// matched by shell identity (not by string), so 8.3 or differently cased
// paths still match.
class Sink final : public IFileOperationProgressSink {
public:
    Sink(std::vector<OpFile>& files, const std::vector<ComPtr<IShellItem>>& items) : files_(files), items_(items) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IFileOperationProgressSink)) {
            *ppv = static_cast<IFileOperationProgressSink*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }  // stack object: owner controls lifetime

    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem* item, IShellItem*, LPCWSTR, HRESULT hr,
                                           IShellItem* created) override {
        Record(item, hr, created, SIGDN_FILESYSPATH);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem* item, HRESULT hr, IShellItem* created) override {
        // `created` is the item in the Recycle Bin; null when deleted permanently.
        Record(item, hr, created, SIGDN_DESKTOPABSOLUTEPARSING);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

private:
    void Record(IShellItem* item, HRESULT hr, IShellItem* created, SIGDN kind) {
        const std::wstring path = ItemName(item, SIGDN_FILESYSPATH);
        for (size_t i = 0; i < files_.size(); ++i) {
            int order = 1;
            const bool same = (items_[i] && item &&
                               item->Compare(items_[i].Get(), SICHINT_CANONICAL | SICHINT_TEST_FILESYSPATH_IF_NOT_EQUAL,
                                             &order) == S_OK && order == 0) ||
                              SamePath(files_[i].from, path);
            if (!same) continue;
            files_[i].hr = hr;
            if (SUCCEEDED(hr) && created) files_[i].to = ItemName(created, kind);
            return;
        }
    }
    std::vector<OpFile>& files_;
    const std::vector<ComPtr<IShellItem>>& items_;
    std::atomic<ULONG> refs_{1};
};

ComPtr<IFileOperation> NewOperation(HWND owner, DWORD flags) {
    ComPtr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) return nullptr;
    op->SetOwnerWindow(owner);
    op->SetOperationFlags(flags);
    return op;
}

// Runs the queued operation, then decides each file by what is actually on
// disk: a file counts as done only if it left its original place (the shell
// reports "skipped by the user" as a success code).
void Perform(IFileOperation* op, FileOpResult& r, int queued, const std::vector<ComPtr<IShellItem>>& items,
             const std::vector<std::wstring>& expected, bool recycle) {
    if (queued == 0) return;
    Sink sink(r.files, items);
    DWORD cookie = 0;
    const bool advised = SUCCEEDED(op->Advise(&sink, &cookie));
    const HRESULT hr = op->PerformOperations();
    BOOL aborted = FALSE;
    op->GetAnyOperationsAborted(&aborted);
    if (advised) op->Unadvise(cookie);
    r.aborted = aborted != FALSE || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == COPYENGINE_E_USER_CANCELLED;
    for (size_t i = 0; i < r.files.size(); ++i) {
        OpFile& f = r.files[i];
        const bool queuedFile = f.hr == E_PENDING || SUCCEEDED(f.hr);
        if (!queuedFile) continue;  // refused before the operation started
        if (PathExists(f.from)) {
            f.hr = r.aborted || SUCCEEDED(f.hr) ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : (FAILED(hr) ? hr : E_FAIL);
            f.to.clear();
            continue;
        }
        f.hr = S_OK;
        if (!recycle && i < expected.size() && (f.to.empty() || !PathExists(f.to)) && PathExists(expected[i]))
            f.to = expected[i];
        if (recycle && f.to.empty()) f.untracked = true;  // gone, but the sink did not say where
    }
}

HRESULT MoveItemTo(HWND owner, IShellItem* item, const std::wstring& originalPath) {
    const std::wstring folder = ParentFolder(originalPath);
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    ComPtr<IShellItem> dest;
    HRESULT hr = SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&dest));
    if (FAILED(hr)) return hr;
    ComPtr<IFileOperation> op = NewOperation(owner, FOF_NOCONFIRMMKDIR);
    if (!op) return E_FAIL;
    const std::wstring leaf = LeafName(originalPath);
    hr = op->MoveItem(item, dest.Get(), leaf.c_str(), nullptr);
    if (SUCCEEDED(hr)) hr = op->PerformOperations();
    if (FAILED(hr)) return hr;
    return PathExists(originalPath) ? S_OK : E_FAIL;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& leaf) {
    return dir + (dir.empty() || dir.back() == L'\\' ? L"" : L"\\") + leaf;
}

// Fallbacks for systems where the shell's IFileOperation is unavailable.
HRESULT LegacyMove(const std::wstring& from, const std::wstring& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)
               ? S_OK
               : HRESULT_FROM_WIN32(GetLastError());
}

bool Unsupported(HRESULT hr) { return hr == E_NOTIMPL || hr == REGDB_E_CLASSNOTREG || hr == E_NOINTERFACE; }

}  // namespace

bool PathExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

FileOpResult MoveFiles(HWND owner, const std::vector<std::wstring>& paths, const std::wstring& destDir,
                       bool renameOnCollision) {
    FileOpResult r;
    for (const auto& p : paths) r.files.push_back({p, L"", E_FAIL, false});
    if (paths.empty()) return r;
    const int mk = SHCreateDirectoryExW(nullptr, destDir.c_str(), nullptr);
    if (mk != ERROR_SUCCESS && mk != ERROR_ALREADY_EXISTS && mk != ERROR_FILE_EXISTS) {
        for (auto& f : r.files) f.hr = HRESULT_FROM_WIN32(mk);
        return r;
    }

    // One set of names for the whole group, so pairs stay paired: either the
    // original names, or the same "(n)" suffix on every file.
    std::vector<std::wstring> leaves, names, targets;
    for (const auto& p : paths) leaves.push_back(LeafName(p));
    auto anyExists = [&](const std::vector<std::wstring>& candidate) {
        for (const auto& n : candidate)
            if (PathExists(JoinPath(destDir, n))) return true;
        return false;
    };
    names = leaves;
    if (anyExists(names)) {
        if (!renameOnCollision) {
            for (auto& f : r.files) f.hr = HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
            return r;
        }
        for (int n = 2; n < 10000 && anyExists(names = core::SuffixedNames(leaves, n)); ++n) {
        }
        if (anyExists(names)) {
            for (auto& f : r.files) f.hr = HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
            return r;
        }
    }
    for (const auto& n : names) targets.push_back(JoinPath(destDir, n));

    ComPtr<IShellItem> dest;
    HRESULT hr = SHCreateItemFromParsingName(destDir.c_str(), nullptr, IID_PPV_ARGS(&dest));
    // No FOF_NOCONFIRMATION: if a name appears in the meantime, Windows asks instead of replacing.
    ComPtr<IFileOperation> op = SUCCEEDED(hr) ? NewOperation(owner, FOF_NOCONFIRMMKDIR) : nullptr;
    std::vector<ComPtr<IShellItem>> items(paths.size());
    int queued = 0;
    for (size_t i = 0; op && i < paths.size(); ++i) {
        OpFile& f = r.files[i];
        if (SUCCEEDED(f.hr = SHCreateItemFromParsingName(paths[i].c_str(), nullptr, IID_PPV_ARGS(&items[i]))) &&
            SUCCEEDED(f.hr = op->MoveItem(items[i].Get(), dest.Get(), names[i].c_str(), nullptr))) {
            f.hr = E_PENDING;
            ++queued;
        }
    }
    if (op) Perform(op.Get(), r, queued, items, targets, false);

    // The shell could not do it at all: plain Win32 moves (never replacing).
    const bool fallback = !op || std::all_of(r.files.begin(), r.files.end(), [](const OpFile& f) { return Unsupported(f.hr); });
    if (fallback) {
        for (size_t i = 0; i < r.files.size(); ++i) {
            OpFile& f = r.files[i];
            f.hr = LegacyMove(f.from, targets[i]);
            if (SUCCEEDED(f.hr)) f.to = targets[i];
        }
    }
    return r;
}

FileOpResult RecycleFiles(HWND owner, const std::vector<std::wstring>& paths) {
    FileOpResult r;
    // FOF_WANTNUKEWARNING: if a file cannot go to the Recycle Bin (network share,
    // bin disabled or full) Windows asks before deleting it permanently.
    ComPtr<IFileOperation> op =
        NewOperation(owner, FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT);
    std::vector<ComPtr<IShellItem>> items(paths.size());
    int queued = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        OpFile f;
        f.from = paths[i];
        f.hr = op ? S_OK : E_FAIL;
        if (op && SUCCEEDED(f.hr = SHCreateItemFromParsingName(paths[i].c_str(), nullptr, IID_PPV_ARGS(&items[i]))) &&
            SUCCEEDED(f.hr = op->DeleteItem(items[i].Get(), nullptr))) {
            f.hr = E_PENDING;
            ++queued;
        }
        r.files.push_back(std::move(f));
    }
    if (op) Perform(op.Get(), r, queued, items, {}, true);

    // The shell could not do it at all: legacy API (still the Recycle Bin, but
    // without knowing the bin item, so these deletes cannot be undone by us).
    const bool fallback = !op || std::all_of(r.files.begin(), r.files.end(), [](const OpFile& f) { return Unsupported(f.hr); });
    if (fallback) {
        for (auto& f : r.files) {
            std::wstring from = f.from;
            from.push_back(L'\0');  // double-null terminated list
            SHFILEOPSTRUCTW legacy{};
            legacy.hwnd = owner;
            legacy.wFunc = FO_DELETE;
            legacy.pFrom = from.c_str();
            legacy.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_NOERRORUI | FOF_SILENT;
            const int rc = SHFileOperationW(&legacy);
            f.hr = rc == 0 && !legacy.fAnyOperationsAborted && !PathExists(f.from) ? S_OK : E_FAIL;
            f.untracked = SUCCEEDED(f.hr);
            if (legacy.fAnyOperationsAborted) r.aborted = true;
        }
    }
    return r;
}

HRESULT RestoreRecycled(HWND owner, const std::wstring& recycleItem, const std::wstring& originalPath) {
    if (PathExists(originalPath)) return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    if (recycleItem.empty()) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);  // was deleted permanently
    ComPtr<IShellItem> item;
    HRESULT hr = SHCreateItemFromParsingName(recycleItem.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);  // Recycle Bin was emptied

    // Preferred: the Recycle Bin's own "Restore" command (keeps its bookkeeping tidy).
    ComPtr<IContextMenu> menu;
    if (SUCCEEDED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&menu)))) {
        HMENU hm = CreatePopupMenu();
        menu->QueryContextMenu(hm, 0, 1, 0x7FFF, CMF_NORMAL);
        CMINVOKECOMMANDINFOEX ici{};
        ici.cbSize = sizeof ici;
        ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_FLAG_NO_UI;
        ici.hwnd = owner;
        ici.lpVerb = "undelete";
        ici.lpVerbW = L"undelete";
        ici.nShow = SW_SHOWNORMAL;
        hr = menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ici));
        DestroyMenu(hm);
        for (int i = 0; SUCCEEDED(hr) && i < 20 && !PathExists(originalPath); ++i) Sleep(25);
        if (PathExists(originalPath)) return S_OK;
    }
    // Fallback: move the item out of the bin to where it came from.
    return MoveItemTo(owner, item.Get(), originalPath);
}

HRESULT MoveBack(HWND owner, const std::wstring& currentPath, const std::wstring& originalPath) {
    if (PathExists(originalPath)) return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    ComPtr<IShellItem> item;
    const HRESULT hr = SHCreateItemFromParsingName(currentPath.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr)) return hr;
    const HRESULT moved = MoveItemTo(owner, item.Get(), originalPath);
    if (Unsupported(moved)) {
        SHCreateDirectoryExW(nullptr, ParentFolder(originalPath).c_str(), nullptr);
        return LegacyMove(currentPath, originalPath);
    }
    return moved;
}

}  // namespace fiv
