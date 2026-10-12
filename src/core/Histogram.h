#pragma once
// Tonal histogram of a decoded image, computed from a small mip level that is
// already in memory (no extra decoding or disk reads).
#include <array>
#include <cstdint>

#include "DecodePlan.h"

namespace fiv::core {

struct Histogram {
    std::array<uint32_t, 256> r{}, g{}, b{}, luma{};
    uint32_t total = 0;          // pixels counted (fully transparent ones are skipped)
    uint32_t clippedHigh = 0;    // pixels with any channel at 255
    uint32_t clippedLow = 0;     // pixels with every channel <= 1
    uint32_t MaxBin() const;     // tallest bin over all channels (for scaling a plot)
    float HighPercent() const { return total ? 100.0f * float(clippedHigh) / float(total) : 0.0f; }
    float LowPercent() const { return total ? 100.0f * float(clippedLow) / float(total) : 0.0f; }
};

// BGRA8, tightly packed.
void ComputeHistogram(const uint8_t* bgra, Size size, Histogram& out);

// Index of the first mip level whose larger side is <= maxSide (the last level if none).
uint32_t HistogramLevel(Size level0, uint32_t levels, uint32_t maxSide = 512);

}  // namespace fiv::core
