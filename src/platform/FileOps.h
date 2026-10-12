#pragma once
// File operations through the Windows shell (IFileOperation), so moves and
// deletes behave exactly like Explorer: Recycle Bin, progress for slow
// cross-drive moves, permanent-delete warnings. A progress sink records where
// every file ended up, which is what makes undo exact.
// Main (STA) thread only.
#include <string>
#include <vector>

#include "Win.h"

namespace fiv {

struct OpFile {
    std::wstring from;     // original full path
    std::wstring to;       // move: new full path; recycle: Recycle Bin item parsing name ("" if not known)
    HRESULT hr = E_FAIL;   // per-file outcome
    bool untracked = false;  // recycled through the legacy API: in the bin, but not restorable by us
};

struct FileOpResult {
    std::vector<OpFile> files;
    bool aborted = false;  // user cancelled a confirmation
    bool AllOk() const {
        for (const auto& f : files)
            if (FAILED(f.hr)) return false;
        return !files.empty() && !aborted;
    }
};

// Moves into destDir (created if missing). With renameOnCollision an existing
// name becomes "name (2).ext"; otherwise colliding files are skipped
// (hr = HRESULT_FROM_WIN32(ERROR_FILE_EXISTS)). Never overwrites silently.
FileOpResult MoveFiles(HWND owner, const std::vector<std::wstring>& paths, const std::wstring& destDir,
                       bool renameOnCollision);

// Sends to the Recycle Bin. Windows asks first if a file would be deleted permanently.
FileOpResult RecycleFiles(HWND owner, const std::vector<std::wstring>& paths);

// Undo helpers. Both refuse to overwrite an existing file at the original path.
HRESULT RestoreRecycled(HWND owner, const std::wstring& recycleItem, const std::wstring& originalPath);
HRESULT MoveBack(HWND owner, const std::wstring& currentPath, const std::wstring& originalPath);

bool PathExists(const std::wstring& path);

}  // namespace fiv
