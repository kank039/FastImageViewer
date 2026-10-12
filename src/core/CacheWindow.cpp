#include "CacheWindow.h"

#include <algorithm>

namespace fiv::core {

std::vector<WindowSlot> ComputeWindow(const WindowParams& p) {
    std::vector<WindowSlot> out;
    if (p.count <= 0) return out;

    const int current = std::clamp(p.current, 0, p.count - 1);
    const int dir = p.direction < 0 ? -1 : 1;
    const int ahead = std::max(0, p.ahead);
    const int behind = std::max(0, p.behind);

    std::vector<int> offsets;
    offsets.push_back(0);
    if (ahead >= 1) offsets.push_back(1);
    if (ahead >= 2) offsets.push_back(2);
    if (behind >= 1) offsets.push_back(-1);
    for (int i = 3; i <= ahead; ++i) offsets.push_back(i);
    for (int i = 2; i <= behind; ++i) offsets.push_back(-i);

    std::vector<bool> taken(static_cast<size_t>(p.count), false);
    for (int off : offsets) {
        int idx = current + off * dir;
        if (p.wrap) {
            idx %= p.count;
            if (idx < 0) idx += p.count;
        } else if (idx < 0 || idx >= p.count) {
            continue;
        }
        if (taken[static_cast<size_t>(idx)]) continue;
        taken[static_cast<size_t>(idx)] = true;
        out.push_back({idx, static_cast<int>(out.size()), off});
    }
    return out;
}

}  // namespace fiv::core
