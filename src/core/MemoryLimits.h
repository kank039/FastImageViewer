#pragma once
// Hardware caps (what the settings may ever allow) and live limits (what the
// cache may use right now without pushing anything into the pagefile or
// making the GPU demote textures to system memory).
#include <cstdint>

namespace fiv::core {

struct MemoryFacts {
    uint64_t totalRam = 0;
    uint64_t dedicatedVram = 0;
    uint64_t osVramBudget = 0;   // DXGI local budget at startup, 0 if unknown
    bool unifiedMemory = false;  // integrated GPU: textures live in system RAM
};

struct MemoryCaps {
    uint64_t maxRam = 0;       // slider ceiling for the RAM budget
    uint64_t maxVram = 0;      // slider ceiling for the VRAM budget (0 on unified memory)
    uint64_t defaultRam = 0;
    uint64_t defaultVram = 0;
    bool unified = false;
};

// VRAM: n GB card -> at most n-1 GB (half the card below 2 GB), never above the
//       OS budget minus a margin.
// RAM:  total minus max(2 GB, 25%) is left for Windows and other programs.
MemoryCaps ComputeCaps(const MemoryFacts& f);

struct LiveMemory {
    uint64_t totalRam = 0;
    uint64_t availRam = 0;     // physical memory available right now
    uint64_t vramBudget = 0;   // current DXGI budget for this process (0 = unknown)
    uint64_t vramUsage = 0;    // current DXGI usage of this process
    bool lowMemory = false;    // Windows low-memory notification is signalled
};

struct OurUsage {
    uint64_t ram = 0;  // transient decode memory (plus textures on unified memory)
    uint64_t gpu = 0;  // texture bytes actually allocated
};

struct Limits {
    uint64_t ram = 0;  // decode/transient pool; on unified memory the single pool
    uint64_t gpu = 0;  // texture pool (== ram on unified memory)
    bool unified = false;
};

uint64_t RamHeadroom(uint64_t totalRam);
constexpr uint64_t kVramHeadroom = 256ull * 1024 * 1024;
constexpr uint64_t kMinPool = 96ull * 1024 * 1024;

// settingRam/settingVram are the configured budgets, already clamped to caps.
Limits ComputeLimits(uint64_t settingRam, uint64_t settingVram, const MemoryCaps& caps,
                     const LiveMemory& live, const OurUsage& ours);

}  // namespace fiv::core
