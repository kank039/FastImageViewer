#pragma once
// Image probing and decoding through the Windows Imaging Component.
#include <d3d11.h>

#include <atomic>
#include <memory>
#include <set>
#include <string>

#include "../core/DecodePlan.h"
#include "../core/Exif.h"
#include "../core/Histogram.h"
#include "Jobs.h"
#include "Win.h"

namespace fiv {

struct ProbeResult {
    HRESULT hr = E_FAIL;
    core::Size stored;       // pixel size as stored in the file
    int orientation = 1;     // EXIF orientation to apply (1 = none)
    bool canPrescale = false;
    bool progressive = false;
    core::PhotoInfo photo;
    ComPtr<ID3D11ShaderResourceView> thumb;  // embedded preview, if requested and present
    core::Size thumbSize;
};

// Reads only the header (plus the embedded thumbnail when `thumbDevice` is set).
// headExif: the format keeps EXIF at the start of the file (JPEG, TIFF-based RAW),
// so it is parsed from the first 256 KB; otherwise WIC's metadata reader is used.
ProbeResult ProbeImage(WorkerContext& ctx, ID3D11Device* thumbDevice, const std::wstring& path,
                       bool honorExifOrientation, bool headExif);

struct DecodeRequest {
    std::wstring path;
    uint64_t fileBytes = 0;
    bool readIntoMemory = true;
    core::Size stored;
    int orientation = 1;
    bool canPrescale = false;
    core::DecodePlan plan;
    bool wantSharpness = false;  // compute the focus score (screen-sized decodes)
    // Live priority (rank), updated by the UI thread when the user navigates.
    // Rank <= 1 is the image on screen: normal I/O and CPU priority.
    std::shared_ptr<std::atomic<int>> priority = std::make_shared<std::atomic<int>>(0);
    int Priority() const { return priority->load(std::memory_order_relaxed); }
};

struct DecodeResult {
    HRESULT hr = E_FAIL;
    bool retry = false;  // transient failure (memory, file changed): try again later
    ComPtr<ID3D11ShaderResourceView> srv;
    std::shared_ptr<const core::Histogram> histogram;  // from a small mip level (4 KB)
    float sharpness = -1;  // core::Sharpness, if requested
    double ms = 0;
};

// Decodes straight to plan.size, builds the mip chain and creates an immutable
// texture. Allocations never exceed core::TransientBytes() for the same inputs.
DecodeResult DecodeImage(WorkerContext& ctx, ID3D11Device* device, IoGate& gate, const DecodeRequest& req,
                         const CancelFlag& cancel);

// Lower-case extensions (".jpg") that installed WIC decoders claim.
std::set<std::wstring> InstalledDecoderExtensions();

}  // namespace fiv
