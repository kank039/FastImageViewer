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

#include <unordered_map>

#include "../core/DecodePlan.h"
#include "../core/Exif.h"
#include "Formats.h"
#include "Win.h"

namespace fiv {

struct FileEntry {
    std::wstring path;
    std::wstring name;
    std::wstring rel;  // path relative to the open folder ("sub\\name.jpg"; just the name at the top level)
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

    // RAW/XMP files with the same name that travel with this one (full paths).
    std::vector<std::wstring> companions;
    core::PhotoInfo photo;   // EXIF summary from the probe
    int64_t takenSec = -1;   // capture time in seconds (from photo.dateTaken), -1 unknown
    float sharpness = -1;    // focus score of the decoded image, -1 unknown

    std::string error;       // last probe/decode error for the UI
    uint64_t changedAt = 0;  // tick of the last change notification (still being written?)
    uint64_t retryAt = 0;    // transient failure: do not retry before this tick

    core::Size Oriented() const;
    bool CloudOnly() const;  // OneDrive/online-only placeholder: reading it downloads it to disk
};

struct Scanned;

class FolderModel {
public:
    // Full scan. Entries whose path, size and mtime are unchanged keep their id
    // and probe data, so a rescan does not invalidate the cache.
    bool Load(const std::wstring& folder, const FormatRegistry& formats);
    // Include subfolders (up to kMaxDepth levels, kMaxFiles files), skipping
    // hidden/system folders and the excluded ones (move destinations).
    // Takes effect on the next Load.
    void SetRecursive(bool on, std::vector<std::wstring> excluded);
    bool Recursive() const { return recursive_; }
    bool Truncated() const { return truncated_; }  // stopped at kMaxFiles
    static constexpr int kMaxDepth = 8;
    static constexpr size_t kMaxFiles = 50000;
    void Clear();

    const std::wstring& Folder() const { return folder_; }
    std::vector<FileEntry>& Files() { return files_; }
    const std::vector<FileEntry>& Files() const { return files_; }
    int Count() const { return int(files_.size()); }
    int IndexOfPath(const std::wstring& path) const;
    FileEntry* FindById(uint64_t id);

    // Re-stats one name (relative to the folder) from a change notification.
    // Returns true if the list changed. `needsReload` is set when the change
    // cannot be applied incrementally (a whole subfolder appeared or vanished).
    bool Refresh(const std::wstring& name, const FormatRegistry& formats, uint64_t now, bool* needsReload = nullptr);
    bool Excluded(const std::wstring& dirRel) const;
    bool RemovePath(const std::wstring& path);

    // RAW/XMP pairing (takes effect on the next Load).
    void SetPairing(bool pair) { pairRaw_ = pair; }

private:
    std::vector<FileEntry> Build(const std::vector<Scanned>& scanned, const FormatRegistry& formats,
                                 std::unordered_map<std::wstring, FileEntry>& old, uint64_t now) const;
    std::wstring folder_;
    std::vector<FileEntry> files_;
    bool pairRaw_ = true;
    bool recursive_ = false;
    bool truncated_ = false;
    std::vector<std::wstring> excluded_;  // lower-case full paths
};

// Watches one directory with ReadDirectoryChangesW and posts `msg` to `hwnd`
// once changes have been quiet for a short moment.
class DirWatcher {
public:
    ~DirWatcher() { Stop(); }
    void Start(const std::wstring& folder, HWND hwnd, UINT msg, bool subtree = false);
    void Stop();
    // Changed names since the last call; overflow means "rescan everything".
    std::vector<std::wstring> Take(bool& overflow);

private:
    void Run(std::wstring folder, HWND hwnd, UINT msg, bool subtree);
    std::thread thread_;
    HANDLE stop_ = nullptr;
    std::mutex m_;
    std::set<std::wstring> names_;
    bool overflow_ = false;
};

}  // namespace fiv
