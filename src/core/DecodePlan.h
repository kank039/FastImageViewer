#pragma once
// Exact memory math for decoding one image. Everything the scheduler admits is
// sized here *before* any allocation happens, so the budget can never be
// overshot by an estimate that turned out wrong.
#include <cstdint>

namespace fiv::core {

constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kGiB = 1024ull * kMiB;
constexpr uint32_t kBytesPerPixel = 4;  // BGRA8

struct Size {
    uint32_t w = 0;
    uint32_t h = 0;
    bool empty() const { return w == 0 || h == 0; }
    bool operator==(const Size& o) const { return w == o.w && h == o.h; }
    bool operator!=(const Size& o) const { return !(*this == o); }
};

uint32_t FullMipLevels(Size s);
uint64_t MipChainBytes(Size s, uint32_t levels);  // BGRA8, tightly packed
Size MipLevelSize(Size s, uint32_t level);

struct PlanRequest {
    Size source;                    // oriented (display) size of the original image
    Size target;                    // box to fit inside; empty = full resolution
    uint32_t maxTextureDim = 16384; // GPU limit
    uint64_t maxGpuBytes = UINT64_MAX;
    bool upscale = false;           // allow output larger than source (never needed for display)
    bool mipmaps = true;
};

struct DecodePlan {
    Size size;              // final oriented texture size (level 0)
    uint32_t mipLevels = 0;
    uint64_t gpuBytes = 0;  // texture memory including the mip chain
    bool valid() const { return !size.empty() && mipLevels > 0; }
    bool operator==(const DecodePlan& o) const { return size == o.size && mipLevels == o.mipLevels; }
    bool operator!=(const DecodePlan& o) const { return !(*this == o); }
};

// Largest size <= target (aspect preserved), <= GPU limits and <= maxGpuBytes.
DecodePlan PlanDecode(const PlanRequest& r);

// Power-of-two reduction the decoder itself can do (JPEG DCT scaling, up to 1/8),
// chosen so the decoder output still covers `want` in both axes. Returns 0..3.
uint32_t PrescaleShift(Size stored, Size want);
Size PrescaledSize(Size stored, uint32_t shift);

struct TransientRequest {
    uint64_t fileBytes = 0;
    bool readIntoMemory = true;  // whole file read into RAM before decoding
    Size stored;                 // size as stored in the file (before EXIF rotation)
    bool canPrescale = false;
    bool reorients = false;      // EXIF orientation != 1 needs a second buffer
    bool progressive = false;    // progressive JPEG: decoder buffers all DCT coefficients
    DecodePlan plan;
};

// Peak RAM a decode job may touch: input file buffer, decoder output, the
// orientation scratch buffer, the final mip chain and the driver's upload copy.
uint64_t TransientBytes(const TransientRequest& r);

// Files above this are streamed from disk instead of being read into RAM.
constexpr uint64_t kMaxInMemoryFile = 64 * kMiB;

}  // namespace fiv::core
