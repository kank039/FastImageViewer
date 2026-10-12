#pragma once
// RAW+JPEG pairing: which files are shown, and which travel with them
// (companions) when the shown file is deleted, moved or restored.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fiv::core {

enum class FileKind : uint8_t {
    Ignore,   // not an image we handle
    Primary,  // displayable image (JPEG, HEIC, PNG, ...)
    Raw,      // camera RAW
    Sidecar,  // .xmp metadata sidecar
};

struct GroupInput {
    std::wstring stem;  // file name without extension, already case-folded by the caller
    FileKind kind = FileKind::Ignore;
};

struct GroupOptions {
    bool pairRaw = true;      // RAW/XMP files with the same name travel with the shown image
    bool rawVisible = false;  // RAW format enabled: RAW files without a partner are shown on their own
};

struct FileGroup {
    size_t primary = 0;                // index into the input
    std::vector<size_t> companions;    // indices into the input
};

// Groups by stem. With pairing, the first primary of a stem is shown and every
// RAW/XMP of that stem becomes its companion; a stem with only RAW files shows
// the RAW (if RAW is enabled). Without pairing, every primary (and every RAW,
// if enabled) is shown on its own and sidecars are ignored.
// Result is ordered by primary index.
std::vector<FileGroup> GroupFiles(const std::vector<GroupInput>& in, const GroupOptions& opt);

}  // namespace fiv::core
