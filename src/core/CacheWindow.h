#pragma once
// Which folder entries the cache should hold, and in what priority order.
// Portable: no Windows headers, unit-tested on every platform.
#include <vector>

namespace fiv::core {

struct WindowParams {
    int count = 0;      // files in the folder
    int current = 0;    // index on screen
    int direction = 1;  // +1 travelling forward, -1 travelling backward
    int ahead = 4;      // prefetched in the direction of travel
    int behind = 5;     // retained against the direction of travel
    bool wrap = true;   // folder end wraps to start
};

struct WindowSlot {
    int index = 0;   // folder index
    int rank = 0;    // 0 = current; lower rank = more important
    int offset = 0;  // signed distance from current in travel direction (+ ahead, - behind)
};

// Priority order: current, +1, +2, -1, +3 .. +ahead, -2 .. -behind.
// The image you are about to see always wins; the one you just left is next
// so a single step back is instant, then the rest of the look-ahead, then the
// remaining history. Duplicate indices (small folders with wrap) are dropped.
std::vector<WindowSlot> ComputeWindow(const WindowParams& p);

}  // namespace fiv::core
