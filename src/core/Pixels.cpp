#include "Pixels.h"

#include <algorithm>

namespace fiv::core {

bool OrientationSwapsAxes(int o) { return o >= 5 && o <= 8; }

Size OrientedSize(Size s, int o) { return OrientationSwapsAxes(o) ? Size{s.h, s.w} : s; }

namespace {

uint16_t Rd16(const uint8_t* p, bool le) { return le ? uint16_t(p[0] | p[1] << 8) : uint16_t(p[0] << 8 | p[1]); }
uint32_t Rd32(const uint8_t* p, bool le) {
    return le ? uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24)
              : uint32_t(uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]);
}

int TiffOrientation(const uint8_t* t, size_t n) {
    if (n < 8) return 0;
    bool le;
    if (t[0] == 'I' && t[1] == 'I') le = true;
    else if (t[0] == 'M' && t[1] == 'M') le = false;
    else return 0;
    if (Rd16(t + 2, le) != 42) return 0;
    const uint32_t ifd = Rd32(t + 4, le);
    if (ifd > n || n - ifd < 2) return 0;
    const uint16_t count = Rd16(t + ifd, le);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t e = size_t(ifd) + 2 + size_t(i) * 12;
        if (e + 12 > n) return 0;
        if (Rd16(t + e, le) == 0x0112 && Rd16(t + e + 2, le) == 3) {
            const int v = Rd16(t + e + 8, le);
            return v >= 1 && v <= 8 ? v : 0;
        }
    }
    return 0;
}

}  // namespace

int JpegExifOrientation(const uint8_t* d, size_t n) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return 0;
    size_t pos = 2;
    while (pos + 4 <= n) {
        if (d[pos] != 0xFF) return 0;
        const uint8_t marker = d[pos + 1];
        if (marker == 0xFF) {  // fill byte
            ++pos;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) return 0;  // image data starts: no EXIF before it
        const size_t len = size_t(d[pos + 2]) << 8 | d[pos + 3];
        if (len < 2) return 0;
        const size_t seg = pos + 4, segLen = len - 2;
        if (marker == 0xE1 && segLen >= 14 && seg + 6 <= n && std::equal(d + seg, d + seg + 6, "Exif\0\0")) {
            const size_t avail = std::min(segLen - 6, n - (seg + 6));
            return TiffOrientation(d + seg + 6, avail);
        }
        pos += 2 + len;
    }
    return 0;
}

int JpegIsProgressive(const uint8_t* d, size_t n) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return -1;
    size_t pos = 2;
    while (pos + 4 <= n) {
        if (d[pos] != 0xFF) return -1;
        const uint8_t m = d[pos + 1];
        if (m == 0xFF) {
            ++pos;
            continue;
        }
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            pos += 2;
            continue;
        }
        // SOF0..SOF15 except DHT (C4), JPG (C8) and DAC (CC).
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC)
            return (m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE) ? 1 : 0;
        if (m == 0xDA || m == 0xD9) return -1;
        const size_t len = size_t(d[pos + 2]) << 8 | d[pos + 3];
        if (len < 2) return -1;
        pos += 2 + len;
    }
    return -1;
}

void ApplyOrientation(const uint32_t* src, Size s, int o, uint32_t* dst) {
    const uint32_t W = s.w, H = s.h;
    const Size d = OrientedSize(s, o);
    // Block the loops so transposing orientations stay cache friendly.
    constexpr uint32_t B = 64;
    for (uint32_t by = 0; by < d.h; by += B) {
        for (uint32_t bx = 0; bx < d.w; bx += B) {
            const uint32_t ey = std::min(by + B, d.h), ex = std::min(bx + B, d.w);
            for (uint32_t y = by; y < ey; ++y) {
                uint32_t* row = dst + size_t(y) * d.w;
                for (uint32_t x = bx; x < ex; ++x) {
                    uint32_t sx, sy;
                    switch (o) {
                        case 2: sx = W - 1 - x; sy = y; break;
                        case 3: sx = W - 1 - x; sy = H - 1 - y; break;
                        case 4: sx = x; sy = H - 1 - y; break;
                        case 5: sx = y; sy = x; break;
                        case 6: sx = y; sy = H - 1 - x; break;
                        case 7: sx = W - 1 - y; sy = H - 1 - x; break;
                        case 8: sx = W - 1 - y; sy = x; break;
                        default: sx = x; sy = y; break;
                    }
                    row[x] = src[size_t(sy) * W + sx];
                }
            }
        }
    }
}

void GenerateMips(uint8_t* chain, Size level0, uint32_t levels) {
    uint8_t* prev = chain;
    Size ps = level0;
    for (uint32_t level = 1; level < levels; ++level) {
        uint8_t* cur = prev + size_t(ps.w) * ps.h * kBytesPerPixel;
        const Size cs = MipLevelSize(level0, level);
        for (uint32_t y = 0; y < cs.h; ++y) {
            const uint32_t y0 = std::min(2 * y, ps.h - 1), y1 = std::min(2 * y + 1, ps.h - 1);
            const uint8_t* r0 = prev + size_t(y0) * ps.w * 4;
            const uint8_t* r1 = prev + size_t(y1) * ps.w * 4;
            uint8_t* out = cur + size_t(y) * cs.w * 4;
            for (uint32_t x = 0; x < cs.w; ++x) {
                const uint32_t x0 = std::min(2 * x, ps.w - 1) * 4, x1 = std::min(2 * x + 1, ps.w - 1) * 4;
                for (int c = 0; c < 4; ++c)
                    out[x * 4 + c] = uint8_t((r0[x0 + c] + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) >> 2);
            }
        }
        prev = cur;
        ps = cs;
    }
}

}  // namespace fiv::core
