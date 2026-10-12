#include "Animation.h"

#include <algorithm>

namespace fiv::core {

Disposal ToDisposal(int v) {
    switch (v) {
        case 0: return Disposal::None;
        case 2: return Disposal::Background;
        case 3: return Disposal::Previous;
        default: return Disposal::Keep;
    }
}

namespace {

// The part of r inside the canvas, and where it starts inside the frame.
bool Clip(Size cs, FrameRect r, FrameRect& out, int& fx, int& fy) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(int(cs.w), r.x + r.w), y1 = std::min(int(cs.h), r.y + r.h);
    if (r.w <= 0 || r.h <= 0 || x1 <= x0 || y1 <= y0) return false;
    out = {x0, y0, x1 - x0, y1 - y0};
    fx = x0 - r.x;
    fy = y0 - r.y;
    return true;
}

}  // namespace

void DrawFrame(uint32_t* canvas, Size cs, const uint32_t* frame, FrameRect r) {
    FrameRect c;
    int fx = 0, fy = 0;
    if (!Clip(cs, r, c, fx, fy)) return;
    for (int y = 0; y < c.h; ++y) {
        const uint32_t* src = frame + size_t(fy + y) * size_t(r.w) + size_t(fx);
        uint32_t* dst = canvas + size_t(c.y + y) * cs.w + size_t(c.x);
        for (int x = 0; x < c.w; ++x)
            if (src[x] >> 24) dst[x] = src[x];
    }
}

void ClearRect(uint32_t* canvas, Size cs, FrameRect r) {
    FrameRect c;
    int fx = 0, fy = 0;
    if (!Clip(cs, r, c, fx, fy)) return;
    for (int y = 0; y < c.h; ++y) std::fill_n(canvas + size_t(c.y + y) * cs.w + size_t(c.x), size_t(c.w), 0u);
}

uint32_t FrameDelayMs(uint32_t cs) { return cs < 2 ? 100 : cs * 10; }

}  // namespace fiv::core
