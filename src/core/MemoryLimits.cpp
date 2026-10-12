#include "MemoryLimits.h"

#include <algorithm>

#include "DecodePlan.h"

namespace fiv::core {

static uint64_t SatSub(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }

uint64_t RamHeadroom(uint64_t total) {
    if (total < 4 * kGiB) return 512 * kMiB;
    return std::clamp<uint64_t>(total / 10, 1 * kGiB, 2 * kGiB);
}

MemoryCaps ComputeCaps(const MemoryFacts& f) {
    MemoryCaps c;
    c.unified = f.unifiedMemory;

    const uint64_t ramReserve = std::max<uint64_t>(2 * kGiB, f.totalRam / 4);
    c.maxRam = f.totalRam > ramReserve ? f.totalRam - ramReserve : f.totalRam / 4;
    c.maxRam = std::max<uint64_t>(c.maxRam, 128 * kMiB);

    if (!c.unified) {
        uint64_t v = f.dedicatedVram >= 2 * kGiB ? f.dedicatedVram - 1 * kGiB : f.dedicatedVram / 2;
        if (f.osVramBudget) v = std::min(v, SatSub(f.osVramBudget, kVramHeadroom));
        c.maxVram = std::max<uint64_t>(v, 64 * kMiB);
        c.defaultVram = std::min<uint64_t>(c.maxVram, 1536 * kMiB);
        c.defaultRam = std::min<uint64_t>(c.maxRam, 1 * kGiB);
    } else {
        c.maxVram = 0;
        c.defaultVram = 0;
        c.defaultRam = std::min<uint64_t>(c.maxRam, 1536 * kMiB);
    }
    return c;
}

Limits ComputeLimits(uint64_t settingRam, uint64_t settingVram, const MemoryCaps& caps,
                     const LiveMemory& live, const OurUsage& ours) {
    Limits l;
    l.unified = caps.unified;

    // RAM: never take the system below its headroom. Our own usage is added
    // back so the limit does not shrink just because we are using it.
    uint64_t ram = std::min(settingRam, caps.maxRam);
    if (live.totalRam) {
        const uint64_t room = SatSub(live.availRam, RamHeadroom(live.totalRam));
        ram = std::min(ram, ours.ram + room);
    }
    if (live.lowMemory) ram = std::min(ram, ours.ram / 2);
    l.ram = std::max(ram, std::min(kMinPool, settingRam));

    if (caps.unified) {
        l.gpu = l.ram;
        return l;
    }

    // VRAM: stay inside the budget the OS grants this process; beyond it the
    // driver silently moves textures to system memory.
    uint64_t gpu = std::min(settingVram, caps.maxVram);
    if (live.vramBudget) {
        const uint64_t room = SatSub(SatSub(live.vramBudget, live.vramUsage), kVramHeadroom);
        gpu = std::min(gpu, ours.gpu + room);
    }
    l.gpu = std::max(gpu, std::min<uint64_t>(64 * kMiB, settingVram));
    return l;
}

}  // namespace fiv::core
