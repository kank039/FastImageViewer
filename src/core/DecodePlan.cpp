#include "DecodePlan.h"

#include <algorithm>
#include <cmath>

namespace fiv::core {

uint32_t FullMipLevels(Size s) {
    if (s.empty()) return 0;
    uint32_t m = std::max(s.w, s.h);
    uint32_t levels = 1;
    while (m > 1) {
        m >>= 1;
        ++levels;
    }
    return levels;
}

Size MipLevelSize(Size s, uint32_t level) {
    return {std::max(1u, s.w >> level), std::max(1u, s.h >> level)};
}

uint64_t MipChainBytes(Size s, uint32_t levels) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < levels; ++i) {
        Size l = MipLevelSize(s, i);
        total += uint64_t(l.w) * l.h * kBytesPerPixel;
    }
    return total;
}

static DecodePlan MakePlan(Size src, double scale, bool mipmaps) {
    DecodePlan p;
    p.size.w = std::max(1u, static_cast<uint32_t>(std::lround(src.w * scale)));
    p.size.h = std::max(1u, static_cast<uint32_t>(std::lround(src.h * scale)));
    p.mipLevels = mipmaps ? FullMipLevels(p.size) : 1;
    p.gpuBytes = MipChainBytes(p.size, p.mipLevels);
    return p;
}

DecodePlan PlanDecode(const PlanRequest& r) {
    if (r.source.empty() || r.maxTextureDim == 0) return {};

    double scale = 1.0;
    if (!r.target.empty()) {
        scale = std::min(double(r.target.w) / r.source.w, double(r.target.h) / r.source.h);
        if (!r.upscale) scale = std::min(scale, 1.0);
    }
    scale = std::min(scale, double(r.maxTextureDim) / r.source.w);
    scale = std::min(scale, double(r.maxTextureDim) / r.source.h);

    DecodePlan p = MakePlan(r.source, scale, r.mipmaps);
    // Shrink until it fits the byte cap. Bytes scale with area, so sqrt gets
    // close in one step; the loop absorbs rounding.
    for (int i = 0; i < 64 && p.gpuBytes > r.maxGpuBytes; ++i) {
        if (p.size.w == 1 && p.size.h == 1) return {};
        double f = std::sqrt(double(r.maxGpuBytes) / double(p.gpuBytes)) * 0.995;
        scale *= std::min(f, 0.995);
        p = MakePlan(r.source, scale, r.mipmaps);
    }
    if (p.gpuBytes > r.maxGpuBytes) return {};
    // A clamp to the texture limit can round one axis a pixel over.
    p.size.w = std::min(p.size.w, r.maxTextureDim);
    p.size.h = std::min(p.size.h, r.maxTextureDim);
    p.mipLevels = r.mipmaps ? FullMipLevels(p.size) : 1;
    p.gpuBytes = MipChainBytes(p.size, p.mipLevels);
    return p;
}

DecodePlan PlanRegion(Size source, Rect visible, float margin, uint32_t maxSide) {
    if (source.empty() || visible.empty() || maxSide == 0) return {};
    const auto grow = [&](uint32_t start, uint32_t len, uint32_t limit, uint32_t& outStart, uint32_t& outLen) {
        const double extra = double(len) * double(margin);
        double a = std::max(0.0, double(start) - extra), b = std::min(double(limit), double(start) + len + extra);
        if (b - a > maxSide) {  // too big: keep the middle
            const double mid = (double(start) + double(start) + len) * 0.5;
            a = std::clamp(mid - maxSide * 0.5, 0.0, double(limit) - std::min<double>(limit, maxSide));
            b = std::min(double(limit), a + maxSide);
        }
        outStart = uint32_t(a);
        outLen = uint32_t(b) > outStart ? uint32_t(b) - outStart : 0;
    };
    if (visible.x >= source.w || visible.y >= source.h) return {};
    visible.w = std::min(visible.w, source.w - visible.x);
    visible.h = std::min(visible.h, source.h - visible.y);
    DecodePlan p;
    grow(visible.x, visible.w, source.w, p.region.x, p.region.w);
    grow(visible.y, visible.h, source.h, p.region.y, p.region.h);
    if (p.region.empty()) return {};
    p.size = {p.region.w, p.region.h};
    p.mipLevels = 1;
    p.gpuBytes = uint64_t(p.size.w) * p.size.h * kBytesPerPixel;
    return p;
}

Rect StoredRect(Rect r, Size s, int o) {
    const uint32_t W = s.w, H = s.h;
    const uint32_t x0 = r.x, x1 = r.x + r.w, y0 = r.y, y1 = r.y + r.h;
    switch (o) {
        case 2: return {W - x1, y0, r.w, r.h};
        case 3: return {W - x1, H - y1, r.w, r.h};
        case 4: return {x0, H - y1, r.w, r.h};
        case 5: return {y0, x0, r.h, r.w};
        case 6: return {y0, H - x1, r.h, r.w};
        case 7: return {W - y1, H - x1, r.h, r.w};
        case 8: return {W - y1, x0, r.h, r.w};
        default: return r;
    }
}

Size PrescaledSize(Size stored, uint32_t shift) {
    const uint32_t d = 1u << shift;
    return {(stored.w + d - 1) / d, (stored.h + d - 1) / d};
}

uint32_t PrescaleShift(Size stored, Size want) {
    for (uint32_t shift = 3; shift > 0; --shift) {
        Size s = PrescaledSize(stored, shift);
        if (s.w >= want.w && s.h >= want.h) return shift;
    }
    return 0;
}

uint64_t TransientBytes(const TransientRequest& r) {
    const uint64_t level0 = uint64_t(r.plan.size.w) * r.plan.size.h * kBytesPerPixel;
    uint64_t total = 0;
    if (r.readIntoMemory) total += r.fileBytes;
    // Decoder output before final scaling (only when the codec can prescale).
    if (r.canPrescale) {
        Size want = r.reorients ? Size{r.plan.size.h, r.plan.size.w} : r.plan.size;
        uint32_t shift = PrescaleShift(r.stored, want);
        if (shift > 0) {
            Size ps = PrescaledSize(r.stored, shift);
            total += uint64_t(ps.w) * ps.h * kBytesPerPixel;
        }
    }
    if (r.reorients) total += level0;  // pre-rotation scratch
    total += r.plan.gpuBytes;          // CPU mip chain handed to the GPU
    total += r.plan.gpuBytes;          // driver upload copy (short-lived)
    // Codec working memory, proportional to the stored image: about 2 bytes per
    // pixel in general, up to 6 for progressive JPEG (a 16-bit coefficient per
    // sample for every block, 4:4:4 worst case), which DCT scaling does not reduce.
    total += 4 * kMiB + uint64_t(r.stored.w) * r.stored.h * (r.progressive ? 6 : 2);
    return total;
}

}  // namespace fiv::core
