#include "Exif.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fiv::core {

namespace {

struct Tiff {
    const uint8_t* d;
    size_t n;
    bool le;

    bool Has(size_t off, size_t len) const { return off <= n && len <= n - off; }
    uint16_t U16(size_t o) const { return le ? uint16_t(d[o] | d[o + 1] << 8) : uint16_t(d[o] << 8 | d[o + 1]); }
    uint32_t U32(size_t o) const {
        return le ? uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | uint32_t(d[o + 3]) << 24)
                  : uint32_t(uint32_t(d[o]) << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]);
    }
};

size_t TypeSize(uint16_t type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;  // BYTE ASCII SBYTE UNDEFINED
        case 3: case 8: return 2;                  // SHORT SSHORT
        case 4: case 9: return 4;                  // LONG SLONG
        case 5: case 10: return 8;                 // RATIONAL SRATIONAL
        default: return 0;
    }
}

// Offset of an entry's value bytes, or SIZE_MAX if out of bounds.
size_t ValueOffset(const Tiff& t, size_t e, uint16_t type, uint32_t count) {
    const size_t sz = TypeSize(type);
    if (!sz || count == 0 || count > (1u << 20)) return SIZE_MAX;
    const size_t bytes = sz * count;
    const size_t off = bytes <= 4 ? e + 8 : t.U32(e + 8);
    return t.Has(off, bytes) ? off : SIZE_MAX;
}

std::string Ascii(const Tiff& t, size_t e, uint16_t type, uint32_t count) {
    if (type != 2) return {};
    const size_t off = ValueOffset(t, e, type, count);
    if (off == SIZE_MAX) return {};
    std::string s(reinterpret_cast<const char*>(t.d + off), std::min<size_t>(count, 128));
    s.resize(strnlen(s.c_str(), s.size()));
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 32) c = ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

bool Rational(const Tiff& t, size_t e, uint16_t type, uint32_t count, int64_t& num, int64_t& den) {
    if ((type != 5 && type != 10) || count < 1) return false;
    const size_t off = ValueOffset(t, e, type, 1);
    if (off == SIZE_MAX) return false;
    if (type == 5) {
        num = t.U32(off);
        den = t.U32(off + 4);
    } else {
        num = int32_t(t.U32(off));
        den = int32_t(t.U32(off + 4));
    }
    return den != 0;
}

uint32_t Integer(const Tiff& t, size_t e, uint16_t type) {
    if (type == 3) return t.U16(e + 8);
    if (type == 4) return t.U32(e + 8);
    return 0;
}

// Walks one IFD; returns the Exif sub-IFD offset (0 if none).
uint32_t ReadIfd(const Tiff& t, uint32_t ifd, PhotoInfo& p, bool& any) {
    if (!t.Has(ifd, 2)) return 0;
    const uint16_t count = t.U16(ifd);
    uint32_t exifIfd = 0;
    for (uint32_t i = 0; i < count && i < 512; ++i) {
        const size_t e = size_t(ifd) + 2 + size_t(i) * 12;
        if (!t.Has(e, 12)) break;
        const uint16_t tag = t.U16(e), type = t.U16(e + 2);
        const uint32_t cnt = t.U32(e + 4);
        int64_t num = 0, den = 0;
        switch (tag) {
            case 0x010F: p.make = Ascii(t, e, type, cnt); break;
            case 0x0110: p.model = Ascii(t, e, type, cnt); break;
            case 0x0132:
                if (p.dateTaken.empty()) p.dateTaken = Ascii(t, e, type, cnt);
                break;
            case 0x8769: exifIfd = Integer(t, e, type); break;
            case 0x829A:
                if (Rational(t, e, type, cnt, num, den) && num >= 0 && den > 0) {
                    p.exposureNum = uint32_t(num);
                    p.exposureDen = uint32_t(den);
                }
                break;
            case 0x829D:
                if (Rational(t, e, type, cnt, num, den)) p.fNumber = double(num) / double(den);
                break;
            case 0x8827: p.iso = Integer(t, e, type); break;
            case 0x9003: p.dateTaken = Ascii(t, e, type, cnt); break;
            case 0x9204:
                if (Rational(t, e, type, cnt, num, den)) {
                    p.exposureBias = double(num) / double(den);
                    p.hasBias = true;
                }
                break;
            case 0x920A:
                if (Rational(t, e, type, cnt, num, den)) p.focalLength = double(num) / double(den);
                break;
            case 0xA405: p.focal35 = Integer(t, e, type); break;
            case 0xA434: p.lens = Ascii(t, e, type, cnt); break;
            default: continue;
        }
        any = true;
    }
    return exifIfd;
}

bool ParseTiff(const uint8_t* d, size_t n, PhotoInfo& p) {
    if (n < 8) return false;
    Tiff t{d, n, false};
    if (d[0] == 'I' && d[1] == 'I') t.le = true;
    else if (d[0] == 'M' && d[1] == 'M') t.le = false;
    else return false;
    const uint16_t magic = t.U16(2);
    if (magic != 42 && magic != 0x4F52 && magic != 0x5352 && magic != 0x55) return false;  // TIFF, ORF, RW2
    bool any = false;
    const uint32_t exif = ReadIfd(t, t.U32(4), p, any);
    if (exif) ReadIfd(t, exif, p, any);
    return any;
}

std::string Fmt(const char* f, double v) {
    char b[32];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

std::string Trimmed(double v, int decimals) {
    std::string s = Fmt(decimals == 1 ? "%.1f" : "%.0f", v);
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    return s;
}

bool StartsWithI(const std::string& s, const std::string& prefix) {
    if (prefix.empty() || s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

}  // namespace

bool ParseExifHead(const uint8_t* d, size_t n, PhotoInfo& out) {
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
        size_t pos = 2;
        while (pos + 4 <= n) {
            if (d[pos] != 0xFF) return false;
            const uint8_t m = d[pos + 1];
            if (m == 0xFF) {
                ++pos;
                continue;
            }
            if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
                pos += 2;
                continue;
            }
            if (m == 0xDA || m == 0xD9) return false;
            const size_t len = size_t(d[pos + 2]) << 8 | d[pos + 3];
            if (len < 2) return false;
            const size_t seg = pos + 4;
            if (m == 0xE1 && len >= 16 && seg + 6 <= n && std::memcmp(d + seg, "Exif\0\0", 6) == 0) {
                const size_t avail = std::min(len - 8, n - (seg + 6));
                return ParseTiff(d + seg + 6, avail, out);
            }
            pos += 2 + len;
        }
        return false;
    }
    return ParseTiff(d, n, out);
}

std::string FormatExposure(uint32_t num, uint32_t den) {
    if (den == 0 || num == 0) return {};
    const double v = double(num) / double(den);
    if (v >= 0.3) return Trimmed(v, 1) + " s";
    return "1/" + Fmt("%.0f", 1.0 / v) + " s";
}

std::string FormatExifDate(const std::string& e) {
    std::string s = e;
    if (s.size() >= 10 && s[4] == ':' && s[7] == ':') {
        s[4] = '-';
        s[7] = '-';
    }
    return s;
}

std::string CameraLine(const PhotoInfo& p) {
    std::string cam;
    const std::string firstWord = p.make.substr(0, p.make.find(' '));
    if (!p.model.empty())
        cam = StartsWithI(p.model, firstWord) || p.make.empty() ? p.model : p.make + " " + p.model;
    else
        cam = p.make;
    if (!p.lens.empty()) cam += cam.empty() ? p.lens : "  \xC2\xB7  " + p.lens;
    return cam;
}

std::string ExposureLine(const PhotoInfo& p) {
    std::string out;
    auto add = [&](const std::string& s) {
        if (s.empty()) return;
        if (!out.empty()) out += "  \xC2\xB7  ";
        out += s;
    };
    add(FormatExposure(p.exposureNum, p.exposureDen));
    if (p.fNumber > 0) add("f/" + Trimmed(p.fNumber, 1));
    if (p.iso) add("ISO " + std::to_string(p.iso));
    if (p.focalLength > 0) {
        std::string f = Trimmed(p.focalLength, 0) + " mm";
        if (p.focal35 && std::lround(p.focalLength) != long(p.focal35)) f += " (" + std::to_string(p.focal35) + " mm eq.)";
        add(f);
    }
    if (p.hasBias && std::fabs(p.exposureBias) >= 0.05) add(Fmt("%+.1f EV", p.exposureBias));
    return out;
}

}  // namespace fiv::core
