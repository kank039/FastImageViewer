#include "Histogram.h"

#include <algorithm>

namespace fiv::core {

uint32_t Histogram::MaxBin() const {
    uint32_t m = 0;
    for (int i = 0; i < 256; ++i) m = std::max({m, r[i], g[i], b[i], luma[i]});
    return m;
}

void ComputeHistogram(const uint8_t* p, Size s, Histogram& h) {
    h = Histogram{};
    const size_t n = size_t(s.w) * s.h;
    for (size_t i = 0; i < n; ++i, p += 4) {
        if (p[3] == 0) continue;
        const uint8_t B = p[0], G = p[1], R = p[2];
        ++h.r[R];
        ++h.g[G];
        ++h.b[B];
        ++h.luma[(54u * R + 183u * G + 19u * B) >> 8];  // Rec. 709 weights
        ++h.total;
        const uint8_t mx = std::max({R, G, B});
        if (R == 255 || G == 255 || B == 255) ++h.clippedHigh;
        if (mx <= 1) ++h.clippedLow;
    }
}

uint32_t HistogramLevel(Size level0, uint32_t levels, uint32_t maxSide) {
    for (uint32_t l = 0; l < levels; ++l) {
        const Size m = MipLevelSize(level0, l);
        if (std::max(m.w, m.h) <= maxSide) return l;
    }
    return levels ? levels - 1 : 0;
}

}  // namespace fiv::core
