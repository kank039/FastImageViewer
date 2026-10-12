#pragma once
// The list of images in the open folder, kept in Explorer order and updated
// incrementally from directory-change notifications (never a cache flush).
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../core/DecodePlan.h"
#include "Formats.h"
#include "Win.h"

namespace fiv {

struct FileEntry {
    std::wstring path;
    std::wstring name;
    std::wstring ext;  // lower case
    uint64_t size = 0;
    uint64_t mtime = 0;
    uint32_t attrs = 0;
    uint64_t id = 0;  // new id whenever size/mtime change, so stale cache entries can never be shown

    // Header probe
    bool probed = false;
    HRESULT probeHr = S_OK;
    core::Size stored;
    int orientation = 1;
    bool canPrescale = false;
    bool progressive = false;  // progressive JPEG (larger decoder working memory)

    std::string error;       // last probe/decode error for the UI
    uint64_t changedAt = 0;  // tick of the last change notification (still being written?)
    uint64_t retryAt = 0;    // transient failure: do not retry before this tick

    core::Size Oriented() const;
    bool CloudOnly() const;  // OneDrive/online-only placeholder: reading it downloads it to disk
};

class FolderModel {
public:
    // Full scan. Entries whose path, size and mtime are unchanged keep their id
    // and probe data, so a rescan does not invalidate the cache.
    bool Load(const std::wstring& folder, const FormatRegistry& formats);
    void Clear();

    const std::wstring& Folder() const { return folder_; }
    std::vector<FileEntry>& Files() { return files_; }
    const std::vector<FileEntry>& Files() const { return files_; }
    int Count() const { return int(files_.size()); }
    int IndexOfPath(const std::wstring& path) const;
    FileEntry* FindById(uint64_t id);

    // Re-stats one name from a change notification. Returns true if the list changed.
    bool Refresh(const std::wstring& name, const FormatRegistry& formats, uint64_t now);
    bool RemovePath(const std::wstring& path);

private:
    std::wstring folder_;
    std::vector<FileEntry> files_;
};

// Watches one directory with ReadDirectoryChangesW and posts `msg` to `hwnd`
// once changes have been quiet for a short moment.
class DirWatcher {
public:
    ~DirWatcher() { Stop(); }
    void Start(const std::wstring& folder, HWND hwnd, UINT msg);
    void Stop();
    // Changed names since the last call; overflow means "rescan everything".
    std::vector<std::wstring> Take(bool& overflow);

private:
    void Run(std::wstring folder, HWND hwnd, UINT msg);
    std::thread thread_;
    HANDLE stop_ = nullptr;
    std::mutex m_;
    std::set<std::wstring> names_;
    bool overflow_ = false;
};

}  // namespace fiv
