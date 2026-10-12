#include "Organize.h"

#include <algorithm>
#include <cstdio>

namespace fiv::core {

namespace {

// Days from 1970-01-01 to a civil date (proleptic Gregorian).
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

}  // namespace

int64_t ParseExifTime(const std::string& s) {
    if (s.size() < 19) return -1;
    auto num = [&](size_t at, size_t len, int& out) {
        out = 0;
        for (size_t i = at; i < at + len; ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
            out = out * 10 + (s[i] - '0');
        }
        return true;
    };
    int y, mo, d, h, mi, se;
    if (!num(0, 4, y) || !num(5, 2, mo) || !num(8, 2, d) || !num(11, 2, h) || !num(14, 2, mi) || !num(17, 2, se))
        return -1;
    if (y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return -1;
    return DaysFromCivil(y, unsigned(mo), unsigned(d)) * 86400 + h * 3600 + mi * 60 + se;
}

int64_t FileTimeToUnix(uint64_t t) { return int64_t(t / 10000000ull) - 11644473600ll; }

const char* SortName(SortKey k) {
    switch (k) {
        case SortKey::Name: return "Name";
        case SortKey::Taken: return "Date taken";
        case SortKey::Modified: return "Date modified";
        case SortKey::Size: return "File size";
        case SortKey::Type: return "File type";
        default: return "";
    }
}

std::vector<int> SortOrder(const std::vector<SortItem>& items, SortKey key, bool descending) {
    std::vector<int> order(items.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = int(i);
    auto cmp = [&](int a, int b) -> int {  // <0, 0, >0
        const SortItem &x = items[size_t(a)], &y = items[size_t(b)];
        switch (key) {
            case SortKey::Taken: {
                const int64_t tx = x.taken >= 0 ? x.taken : x.modified, ty = y.taken >= 0 ? y.taken : y.modified;
                return tx < ty ? -1 : tx > ty ? 1 : 0;
            }
            case SortKey::Modified: return x.modified < y.modified ? -1 : x.modified > y.modified ? 1 : 0;
            case SortKey::Size: return x.size < y.size ? -1 : x.size > y.size ? 1 : 0;
            case SortKey::Type: return x.type.compare(y.type);
            default: return 0;
        }
    };
    if (key == SortKey::Name) {
        if (descending) std::reverse(order.begin(), order.end());
        return order;
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const int c = cmp(a, b);
        if (c != 0) return descending ? c > 0 : c < 0;
        return descending ? a > b : a < b;
    });
    return order;
}

std::vector<int> FindBursts(const std::vector<BurstInput>& in, int gapSeconds) {
    std::vector<int> id(in.size(), -1);
    int next = 0;
    size_t i = 0;
    while (i < in.size()) {
        size_t j = i + 1;
        while (j < in.size() && in[j - 1].taken >= 0 && in[j].taken >= 0 && in[j].camera == in[j - 1].camera &&
               std::llabs(in[j].taken - in[j - 1].taken) <= gapSeconds)
            ++j;
        if (j - i >= 2) {
            for (size_t k = i; k < j; ++k) id[k] = next;
            ++next;
        }
        i = j;
    }
    return id;
}

float Sharpness(const uint8_t* bgra, Size size) {
    if (size.w < 8 || size.h < 8) return 0;
    const uint32_t w = size.w, h = size.h;
    auto luma = [&](uint32_t x, uint32_t y) {
        const uint8_t* p = bgra + (size_t(y) * w + x) * 4;
        return float(p[2]) * 0.299f + float(p[1]) * 0.587f + float(p[0]) * 0.114f;
    };
    constexpr int kCells = 4;
    double sum[kCells][kCells] = {}, sumSq[kCells][kCells] = {};
    uint32_t count[kCells][kCells] = {};
    for (uint32_t y = 1; y + 1 < h; ++y) {
        const int cy = int(uint64_t(y) * kCells / h);
        for (uint32_t x = 1; x + 1 < w; ++x) {
            const float lap = 4 * luma(x, y) - luma(x - 1, y) - luma(x + 1, y) - luma(x, y - 1) - luma(x, y + 1);
            const int cx = int(uint64_t(x) * kCells / w);
            sum[cy][cx] += lap;
            sumSq[cy][cx] += double(lap) * lap;
            ++count[cy][cx];
        }
    }
    double best = 0;
    for (int cy = 0; cy < kCells; ++cy)
        for (int cx = 0; cx < kCells; ++cx) {
            if (!count[cy][cx]) continue;
            const double mean = sum[cy][cx] / count[cy][cx];
            best = std::max(best, sumSq[cy][cx] / count[cy][cx] - mean * mean);
        }
    return float(best);
}

}  // namespace fiv::core
