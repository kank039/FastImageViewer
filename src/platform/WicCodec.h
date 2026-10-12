#pragma once
// Image probing and decoding through the Windows Imaging Component.
#include <d3d11.h>

#include <set>
#include <string>

#include "../core/DecodePlan.h"
#include "Jobs.h"
#include "Win.h"

namespace fiv {

struct ProbeResult {
    HRESULT hr = E_FAIL;
    core::Size stored;       // pixel size as stored in the file
    int orientation = 1;     // EXIF orientation to apply (1 = none)
    bool canPrescale = false;
    ComPtr<ID3D11ShaderResourceView> thumb;  // embedded preview, if requested and present
    core::Size thumbSize;
};

// Reads only the header (plus the embedded thumbnail when `thumbDevice` is set).
ProbeResult ProbeImage(WorkerContext& ctx, ID3D11Device* thumbDevice, const std::wstring& path,
                       bool honorExifOrientation);

struct DecodeRequest {
    std::wstring path;
    uint64_t fileBytes = 0;
    bool readIntoMemory = true;
    core::Size stored;
    int orientation = 1;
    bool canPrescale = false;
    core::DecodePlan plan;
    int priority = 0;
    bool background = false;
};

struct DecodeResult {
    HRESULT hr = E_FAIL;
    bool retry = false;  // transient failure (memory, file changed): try again later
    ComPtr<ID3D11ShaderResourceView> srv;
    double ms = 0;
};

// Decodes straight to plan.size, builds the mip chain and creates an immutable
// texture. Allocations never exceed core::TransientBytes() for the same inputs.
DecodeResult DecodeImage(WorkerContext& ctx, ID3D11Device* device, IoGate& gate, const DecodeRequest& req,
                         const CancelFlag& cancel);

// Lower-case extensions (".jpg") that installed WIC decoders claim.
std::set<std::wstring> InstalledDecoderExtensions();

}  // namespace fiv
