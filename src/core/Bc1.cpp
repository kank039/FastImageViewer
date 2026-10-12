#include "Bc1.h"

#include <algorithm>

namespace fiv::core {

Size BlockPadded(Size s) { return {(s.w + 3) / 4 * 4, (s.h + 3) / 4 * 4}; }

uint64_t Bc1LevelBytes(Size l) { return uint64_t((l.w + 3) / 4) * ((l.h + 3) / 4) * 8; }

uint64_t Bc1ChainBytes(Size padded, uint32_t levels) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < levels; ++i) total += Bc1LevelBytes(MipLevelSize(padded, i));
    return total;
}

DecodePlan PlanCompact(const DecodePlan& bgra) {
    if (!bgra.valid() || !bgra.region.empty()) return {};
    DecodePlan p;
    p.size = bgra.size;
    p.compressed = true;
    const Size pad = BlockPadded(p.size);
    p.mipLevels = FullMipLevels(pad);
    p.gpuBytes = Bc1ChainBytes(pad, p.mipLevels);
    return p;
}

namespace {

uint16_t To565(const int c[3]) {  // c in RGB order
    return uint16_t(((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 63 + 127) / 255) << 5 | ((c[2] * 31 + 127) / 255));
}

void From565(uint16_t v, int c[3]) {
    const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    c[0] = (r << 3) | (r >> 2);
    c[1] = (g << 2) | (g >> 4);
    c[2] = (b << 3) | (b >> 2);
}

void Palette(uint16_t c0, uint16_t c1, int pal[4][3]) {
    From565(c0, pal[0]);
    From565(c1, pal[1]);
    for (int k = 0; k < 3; ++k) {
        pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
        pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
    }
}

}  // namespace

void EncodeBc1(const uint8_t* bgra, Size s, uint8_t* out) {
    const uint32_t bw = (s.w + 3) / 4, bh = (s.h + 3) / 4;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            int px[16][3];
            for (uint32_t i = 0; i < 16; ++i) {
                const uint32_t x = std::min(bx * 4 + (i & 3), s.w - 1), y = std::min(by * 4 + (i >> 2), s.h - 1);
                const uint8_t* p = bgra + (size_t(y) * s.w + x) * 4;
                px[i][0] = p[2];
                px[i][1] = p[1];
                px[i][2] = p[0];
            }
            int lo = 0, hi = 0, loL = 1 << 30, hiL = -1;
            for (int i = 0; i < 16; ++i) {
                const int l = px[i][0] * 299 + px[i][1] * 587 + px[i][2] * 114;
                if (l < loL) loL = l, lo = i;
                if (l > hiL) hiL = l, hi = i;
            }
            uint16_t c0 = To565(px[hi]), c1 = To565(px[lo]);
            if (c0 < c1) std::swap(c0, c1);
            uint32_t indices = 0;
            if (c0 != c1) {  // four-colour mode needs c0 > c1
                int pal[4][3];
                Palette(c0, c1, pal);
                for (int i = 0; i < 16; ++i) {
                    int best = 0, bestD = 1 << 30;
                    for (int k = 0; k < 4; ++k) {
                        const int dr = px[i][0] - pal[k][0], dg = px[i][1] - pal[k][1], db = px[i][2] - pal[k][2];
                        const int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                        if (d < bestD) bestD = d, best = k;
                    }
                    indices |= uint32_t(best) << (2 * i);
                }
            }
            uint8_t* o = out + (size_t(by) * bw + bx) * 8;
            o[0] = uint8_t(c0);
            o[1] = uint8_t(c0 >> 8);
            o[2] = uint8_t(c1);
            o[3] = uint8_t(c1 >> 8);
            for (int k = 0; k < 4; ++k) o[4 + k] = uint8_t(indices >> (8 * k));
        }
    }
}

void DecodeBc1(const uint8_t* blocks, Size s, uint8_t* bgra) {
    const uint32_t bw = (s.w + 3) / 4, bh = (s.h + 3) / 4;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = blocks + (size_t(by) * bw + bx) * 8;
            const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
            const uint32_t idx = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
            int pal[4][3];
            Palette(c0, c1, pal);
            for (uint32_t i = 0; i < 16; ++i) {
                const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= s.w || y >= s.h) continue;
                const int k = c0 > c1 ? int((idx >> (2 * i)) & 3) : 0;
                uint8_t* p = bgra + (size_t(y) * s.w + x) * 4;
                p[0] = uint8_t(pal[k][2]);
                p[1] = uint8_t(pal[k][1]);
                p[2] = uint8_t(pal[k][0]);
                p[3] = 255;
            }
        }
}

bool HasAlpha(const uint8_t* bgra, Size s) {
    const size_t n = size_t(s.w) * s.h;
    for (size_t i = 0; i < n; ++i)
        if (bgra[i * 4 + 3] != 255) return true;
    return false;
}

}  // namespace fiv::core
