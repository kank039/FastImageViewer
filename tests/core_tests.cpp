// Unit tests for the portable core. No framework: build and run `core_tests`.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/core/Animation.h"
#include "../src/core/Bc1.h"
#include "../src/core/CacheWindow.h"
#include "../src/core/DecodePlan.h"
#include "../src/core/Exif.h"
#include "../src/core/FileGroups.h"
#include "../src/core/Histogram.h"
#include "../src/core/Keys.h"
#include "../src/core/Marks.h"
#include "../src/core/Organize.h"
#include "../src/core/MemoryLimits.h"
#include "../src/core/Pixels.h"
#include "../src/core/Scheduler.h"
#include "../src/core/Settings.h"

using namespace fiv::core;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                           \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                     \
    } while (0)

static std::vector<int> Indices(const std::vector<WindowSlot>& v) {
    std::vector<int> r;
    for (auto& s : v) r.push_back(s.index);
    return r;
}

static void TestWindow() {
    std::puts("window");
    WindowParams p;
    p.count = 100;
    p.current = 50;
    p.ahead = 4;
    p.behind = 5;
    auto w = ComputeWindow(p);
    CHECK((Indices(w) == std::vector<int>{50, 51, 52, 49, 53, 54, 48, 47, 46, 45}));
    CHECK(w[0].rank == 0 && w[9].rank == 9);

    p.direction = -1;  // travelling backward flips the window
    CHECK((Indices(ComputeWindow(p)) == std::vector<int>{50, 49, 48, 51, 47, 46, 52, 53, 54, 55}));

    p.direction = 1;
    p.current = 99;  // wrap at the end
    CHECK((Indices(ComputeWindow(p)) == std::vector<int>{99, 0, 1, 98, 2, 3, 97, 96, 95, 94}));

    p.wrap = false;  // clamp at the end
    CHECK((Indices(ComputeWindow(p)) == std::vector<int>{99, 98, 97, 96, 95, 94}));

    p.count = 3;  // tiny folder with wrap: no duplicates
    p.current = 0;
    p.wrap = true;
    CHECK((Indices(ComputeWindow(p)) == std::vector<int>{0, 1, 2}));

    p.count = 0;
    CHECK(ComputeWindow(p).empty());

    p.count = 10;
    p.current = 5;
    p.ahead = 0;
    p.behind = 0;
    CHECK((Indices(ComputeWindow(p)) == std::vector<int>{5}));
}

static void TestPlan() {
    std::puts("plan");
    CHECK(FullMipLevels({1, 1}) == 1);
    CHECK(FullMipLevels({1024, 512}) == 11);
    CHECK(MipChainBytes({2, 2}, 2) == (4 + 1) * 4);

    PlanRequest r;
    r.source = {6000, 4000};
    r.target = {3840, 2160};
    DecodePlan p = PlanDecode(r);
    CHECK(p.size.w == 3240 && p.size.h == 2160);
    CHECK(p.gpuBytes == MipChainBytes(p.size, p.mipLevels));

    r.target = {};  // full resolution
    p = PlanDecode(r);
    CHECK(p.size.w == 6000 && p.size.h == 4000);

    r.maxTextureDim = 4096;  // GPU limit
    p = PlanDecode(r);
    CHECK(p.size.w <= 4096 && p.size.h <= 4096 && p.size.w == 4096);

    r.maxTextureDim = 16384;
    r.maxGpuBytes = 20 * kMiB;  // byte cap
    p = PlanDecode(r);
    CHECK(p.valid() && p.gpuBytes <= 20 * kMiB && p.gpuBytes > 15 * kMiB);

    r = {};
    r.source = {800, 600};
    r.target = {3840, 2160};
    p = PlanDecode(r);  // never upscales
    CHECK(p.size.w == 800 && p.size.h == 600);

    r.source = {20000, 100};  // extreme panorama
    p = PlanDecode(r);
    CHECK(p.size.w == 3840 && p.size.h == 19);

    CHECK(PrescaleShift({6000, 4000}, {1500, 1000}) == 2);
    CHECK(PrescaleShift({6000, 4000}, {1501, 1000}) == 1);
    CHECK(PrescaleShift({6000, 4000}, {6000, 4000}) == 0);
    CHECK(PrescaleShift({6000, 4000}, {100, 100}) == 3);

    TransientRequest t;
    t.fileBytes = 10 * kMiB;
    t.stored = {4000, 3000};
    r = {};
    r.source = {4000, 3000};
    r.target = {1920, 1080};
    t.plan = PlanDecode(r);
    uint64_t base = TransientBytes(t);
    CHECK(base == 10 * kMiB + 2 * t.plan.gpuBytes + 4 * kMiB + 4000ull * 3000 * 2);
    t.readIntoMemory = false;
    CHECK(TransientBytes(t) == base - 10 * kMiB);
    t.reorients = true;
    CHECK(TransientBytes(t) > base - 10 * kMiB);
    const uint64_t seq = TransientBytes(t);
    t.progressive = true;
    CHECK(TransientBytes(t) == seq + 4000ull * 3000 * 4);
}

static void TestOrientation() {
    std::puts("orientation");
    // 3x2 stored image:  a b c / d e f
    const uint32_t src[6] = {'a', 'b', 'c', 'd', 'e', 'f'};
    struct Case {
        int o;
        const char* expect;  // row-major output
    } cases[] = {
        {1, "abcdef"},
        {2, "cbafed"},
        {3, "fedcba"},
        {4, "defabc"},
        {5, "adbecf"},  // transpose
        {6, "daebfc"},  // rotate 90 CW
        {7, "fcebda"},  // transverse
        {8, "cfbead"},  // rotate 90 CCW
    };
    for (const Case& c : cases) {
        uint32_t dst[6] = {};
        ApplyOrientation(src, {3, 2}, c.o, dst);
        std::string got;
        for (uint32_t v : dst) got += char(v);
        if (got != c.expect) std::printf("  orientation %d: got %s want %s\n", c.o, got.c_str(), c.expect);
        CHECK(got == c.expect);
        Size os = OrientedSize({3, 2}, c.o);
        CHECK(c.o >= 5 ? (os.w == 2 && os.h == 3) : (os.w == 3 && os.h == 2));
    }
}

static std::vector<uint8_t> JpegWithExif(int orientation, bool bigEndian) {
    std::vector<uint8_t> tiff;
    auto u16 = [&](uint16_t v) {
        if (bigEndian) { tiff.push_back(uint8_t(v >> 8)); tiff.push_back(uint8_t(v)); }
        else { tiff.push_back(uint8_t(v)); tiff.push_back(uint8_t(v >> 8)); }
    };
    auto u32 = [&](uint32_t v) {
        if (bigEndian) { u16(uint16_t(v >> 16)); u16(uint16_t(v)); }
        else { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
    };
    tiff.push_back(bigEndian ? 'M' : 'I');
    tiff.push_back(bigEndian ? 'M' : 'I');
    u16(42); u32(8);
    u16(2);                                     // two entries
    u16(0x010F); u16(2); u32(4); u32(0);        // Make (ignored)
    u16(0x0112); u16(3); u32(1); u16(uint16_t(orientation)); u16(0);
    u32(0);
    std::vector<uint8_t> j = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00};  // SOI + tiny APP0
    const size_t len = 2 + 6 + tiff.size();
    j.insert(j.end(), {0xFF, 0xE1, uint8_t(len >> 8), uint8_t(len)});
    j.insert(j.end(), {'E', 'x', 'i', 'f', 0, 0});
    j.insert(j.end(), tiff.begin(), tiff.end());
    j.insert(j.end(), {0xFF, 0xDA, 0x00, 0x02});  // start of scan
    return j;
}

static void TestExif() {
    std::puts("exif");
    for (int o = 1; o <= 8; ++o) {
        auto le = JpegWithExif(o, false), be = JpegWithExif(o, true);
        CHECK(JpegExifOrientation(le.data(), le.size()) == o);
        CHECK(JpegExifOrientation(be.data(), be.size()) == o);
    }
    auto j = JpegWithExif(6, false);
    for (size_t cut = 0; cut < j.size(); ++cut) CHECK(JpegExifOrientation(j.data(), cut) == 0 || cut > 40);
    j[30] = 0xFF;  // corrupt an IFD byte: must not crash or read out of bounds
    (void)JpegExifOrientation(j.data(), j.size());
    // Progressive detection: SOF0 baseline, SOF2 progressive, none = unknown.
    std::vector<uint8_t> base = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0, 0, 0xFF, 0xC4, 0x00, 0x02, 0xFF, 0xC0, 0x00, 0x02};
    CHECK(JpegIsProgressive(base.data(), base.size()) == 0);
    base[13] = 0xC2;
    CHECK(JpegIsProgressive(base.data(), base.size()) == 1);
    CHECK(JpegIsProgressive(base.data(), 8) == -1);
    const uint8_t png[] = {0x89, 'P', 'N', 'G'};
    CHECK(JpegExifOrientation(png, sizeof png) == 0);
    const uint8_t evil[] = {0xFF, 0xD8, 0xFF, 0xE1, 0xFF, 0xFF, 'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0, 0xF0, 0xFF, 0xFF, 0x7F};
    CHECK(JpegExifOrientation(evil, sizeof evil) == 0);
}

static void TestMips() {
    std::puts("mips");
    Size s{3, 2};
    uint32_t levels = FullMipLevels(s);  // 3x2 -> 1x1 -> 1x1? (3,2)->(1,1): 2 levels
    CHECK(levels == 2);
    std::vector<uint8_t> chain(MipChainBytes(s, levels), 0);
    for (int i = 0; i < 6; ++i) {
        chain[i * 4 + 0] = uint8_t(i * 40);
        chain[i * 4 + 3] = 255;
    }
    GenerateMips(chain.data(), s, levels);
    // level1 (1x1) averages pixels (0,0),(1,0),(0,1),(1,1) = 0,40,120,160 -> 80
    CHECK(chain[24] == 80 && chain[27] == 255);
}

static void TestCaps() {
    std::puts("caps");
    MemoryFacts f;
    f.totalRam = 16 * kGiB;
    f.dedicatedVram = 8 * kGiB;
    f.osVramBudget = 7 * kGiB + 512 * kMiB;
    MemoryCaps c = ComputeCaps(f);
    CHECK(c.maxVram == 7 * kGiB);  // n-1
    CHECK(c.maxRam == 12 * kGiB);  // 16 - max(2, 4)
    CHECK(c.defaultVram <= c.maxVram && c.defaultRam <= c.maxRam);

    f.osVramBudget = 6 * kGiB;  // OS budget is tighter than n-1
    CHECK(ComputeCaps(f).maxVram == 6 * kGiB - kVramHeadroom);

    f.dedicatedVram = 1 * kGiB;  // small card: half
    f.osVramBudget = 0;
    CHECK(ComputeCaps(f).maxVram == 512 * kMiB);

    f.totalRam = 4 * kGiB;
    CHECK(ComputeCaps(f).maxRam == 2 * kGiB);

    f.unifiedMemory = true;
    c = ComputeCaps(f);
    CHECK(c.unified && c.maxVram == 0);

    // Live limits
    f = {};
    f.totalRam = 16 * kGiB;
    f.dedicatedVram = 8 * kGiB;
    c = ComputeCaps(f);
    LiveMemory live;
    live.totalRam = 16 * kGiB;
    live.availRam = 8 * kGiB;
    live.vramBudget = 7 * kGiB;
    live.vramUsage = 1 * kGiB;
    OurUsage ours;
    Limits l = ComputeLimits(1 * kGiB, 2 * kGiB, c, live, ours);
    CHECK(l.ram == 1 * kGiB && l.gpu == 2 * kGiB);

    live.availRam = 2 * kGiB;  // system is tight: headroom (1.6 GiB) is protected
    l = ComputeLimits(1 * kGiB, 2 * kGiB, c, live, ours);
    CHECK(l.ram == 2 * kGiB - RamHeadroom(16 * kGiB));

    live.vramUsage = 6 * kGiB + 900 * kMiB;  // a game took the VRAM
    ours.gpu = 500 * kMiB;
    l = ComputeLimits(1 * kGiB, 2 * kGiB, c, live, ours);
    CHECK(l.gpu == 500 * kMiB);  // keep what we have, take nothing more

    live.lowMemory = true;
    ours.ram = 400 * kMiB;
    l = ComputeLimits(1 * kGiB, 2 * kGiB, c, live, ours);
    CHECK(l.ram == 200 * kMiB);
}

// ---- Scheduler -------------------------------------------------------------

struct FakeHost : SchedulerHost {
    struct Started {
        uint64_t job;
        EntryKey key;
        bool probe;
        uint64_t item;
    };
    std::vector<Started> started;
    std::vector<uint64_t> cancelled;
    std::vector<EntryKey> released;
    void StartProbe(uint64_t job, uint64_t item, int) override { started.push_back({job, {}, true, item}); }
    void StartDecode(uint64_t job, const EntryKey& key, const DecodePlan&, int) override {
        started.push_back({job, key, false, 0});
    }
    void CancelJob(uint64_t job) override { cancelled.push_back(job); }
    void SetJobPriority(uint64_t, int) override {}
    void ReleaseEntry(const EntryKey& key) override { released.push_back(key); }
};

static Want MakeWant(uint64_t item, int rank, uint64_t gpuMiB) {
    Want w;
    w.key = {item, Variant::Fit};
    w.rank = rank;
    w.sizeKnown = true;
    w.plan.size = {100, 100};
    w.plan.mipLevels = 1;
    w.plan.gpuBytes = gpuMiB * kMiB;
    w.transientBytes = gpuMiB * 2 * kMiB;
    return w;
}

static void CompleteAll(Scheduler& s, FakeHost& h, Outcome o = Outcome::Ok) {
    auto st = h.started;
    h.started.clear();
    for (auto& x : st) {
        if (x.probe)
            s.OnProbeDone(x.job);
        else
            s.OnDecodeDone(x.job, o);
    }
}

static void TestScheduler() {
    std::puts("scheduler");
    FakeHost h;
    Scheduler s(h);
    s.SetConfig({2, 2});
    Limits lim{1000 * kMiB, 100 * kMiB, false};

    // Five wants of 30 MiB with a 100 MiB texture pool: only three fit.
    std::vector<Want> wants;
    for (int i = 0; i < 5; ++i) wants.push_back(MakeWant(10 + i, i, 30));
    s.Pump(wants, lim, true);
    CHECK(h.started.size() == 2);  // two decode slots
    CompleteAll(s, h);
    s.Pump(wants, lim, true);
    CHECK(h.started.size() == 1);  // third fits
    CompleteAll(s, h);
    s.Pump(wants, lim, true);
    CHECK(h.started.empty());  // fourth would exceed 100 MiB
    CHECK(s.ResidentGpu() == 90 * kMiB);
    CHECK(s.CommittedGpu() <= lim.gpu);

    // Navigate: item 13 becomes rank 0; item 10 drops out of the window.
    wants.clear();
    for (int i = 0; i < 4; ++i) wants.push_back(MakeWant(13 + i, i, 30));
    s.Pump(wants, lim, true);
    CHECK(h.released.size() == 3);  // 10, 11, 12 left the window
    CHECK(h.started.size() == 2);
    CompleteAll(s, h);
    s.Pump(wants, lim, true);
    CompleteAll(s, h);
    CHECK(s.ResidentGpu() == 90 * kMiB);

    // Rank 0 gets an express slot even when prefetch fills both slots.
    {
        FakeHost hx;
        Scheduler sx(hx);
        sx.SetConfig({2, 2});
        std::vector<Want> wx{MakeWant(1, 0, 1), MakeWant(2, 1, 1), MakeWant(3, 2, 1)};
        std::vector<Want> busy{MakeWant(2, 1, 1), MakeWant(3, 2, 1)};
        sx.Pump(busy, lim, true);
        CHECK(hx.started.size() == 2);
        sx.Pump(wx, lim, true);
        CHECK(hx.started.size() == 3 && hx.started[2].key.item == 1);
    }

    // Budget shrinks: lowest ranks are evicted first, rank 0 survives.
    h.released.clear();
    lim.gpu = 40 * kMiB;
    s.Pump(wants, lim, true);
    CHECK(h.released.size() == 2);
    CHECK(s.IsResident({13, Variant::Fit}));
    CHECK(s.ResidentGpu() <= lim.gpu);

    // Cancelled jobs keep their bytes counted until the worker reports back.
    Scheduler s2(h);
    h.started.clear();
    std::vector<Want> w2{MakeWant(1, 0, 50)};
    s2.Pump(w2, {1000 * kMiB, 100 * kMiB, false}, true);
    CHECK(h.started.size() == 1);
    uint64_t job = h.started[0].job;
    h.started.clear();
    std::vector<Want> w3{MakeWant(2, 0, 60)};
    s2.Pump(w3, {1000 * kMiB, 100 * kMiB, false}, true);
    CHECK(h.started.empty());  // 50 (zombie) + 60 > 100: must wait
    CHECK(s2.OnDecodeDone(job, Outcome::Ok) == false);  // zombie result is discarded
    s2.Pump(w3, {1000 * kMiB, 100 * kMiB, false}, true);
    CHECK(h.started.size() == 1);

    // Transient RAM pool limits concurrency.
    Scheduler s3(h);
    h.started.clear();
    std::vector<Want> w4;
    for (int i = 0; i < 4; ++i) w4.push_back(MakeWant(100 + i, i, 10));  // transient 20 MiB each
    s3.Pump(w4, {45 * kMiB, 1000 * kMiB, false}, true);
    CHECK(h.started.size() == 2);

    // Skimming: only rank 0 starts while prefetch is held back.
    Scheduler s4(h);
    h.started.clear();
    s4.Pump(w4, {1000 * kMiB, 1000 * kMiB, false}, false);
    CHECK(h.started.size() == 1 && h.started[0].key.item == 100);

    // Unknown sizes are probed first, limited by maxProbes (+1 for rank 0).
    Scheduler s5(h);
    h.started.clear();
    std::vector<Want> w5;
    for (int i = 0; i < 6; ++i) {
        Want w;
        w.key = {uint64_t(200 + i), Variant::Fit};
        w.rank = i;
        w5.push_back(w);
    }
    s5.Pump(w5, {1000 * kMiB, 1000 * kMiB, false}, true);
    CHECK(h.started.size() == 2 && h.started[0].probe && h.started[0].item == 200);

    // Unified memory: textures and transient share one pool.
    Scheduler s6(h);
    h.started.clear();
    std::vector<Want> w6{MakeWant(300, 0, 30), MakeWant(301, 1, 30)};
    s6.Pump(w6, {100 * kMiB, 100 * kMiB, true}, true);  // 30+60 fits, second 30+60 more does not
    CHECK(h.started.size() == 1);

    // Failed decodes are not retried while they stay in the window.
    Scheduler s7(h);
    h.started.clear();
    std::vector<Want> w7{MakeWant(400, 0, 10)};
    s7.Pump(w7, lim, true);
    CompleteAll(s7, h, Outcome::Failed);
    s7.Pump(w7, lim, true);
    CHECK(h.started.empty() && s7.IsFailed({400, Variant::Fit}));

    // One-off reservations (clipboard copy) count as transient RAM.
    Scheduler s8(h);
    h.started.clear();
    const Limits l8{100 * kMiB, 1000 * kMiB, false};
    CHECK(s8.TransientRoom(l8) == 100 * kMiB);
    s8.ReserveTransient(70 * kMiB);
    CHECK(s8.TransientRoom(l8) == 30 * kMiB && s8.TransientInUse() == 70 * kMiB);
    // Each needs 20 MiB of transient RAM.
    std::vector<Want> w9{MakeWant(501, 1, 10), MakeWant(502, 2, 10)};
    s8.Pump(w9, l8, true);
    CHECK(h.started.size() == 1);  // the second would need 40 of the 30 left
    s8.ReleaseTransient(70 * kMiB);
    s8.ReleaseTransient(1);  // never underflows
    CHECK(s8.TransientInUse() == 20 * kMiB);
    s8.Pump(w9, l8, true);
    CHECK(h.started.size() == 2);

    // Video memory held outside the cache (animation) evicts lower ranks.
    FakeHost h9;
    Scheduler s9(h9);
    const Limits l9{1000 * kMiB, 100 * kMiB, false};
    std::vector<Want> w10{MakeWant(600, 0, 40), MakeWant(601, 1, 40)};
    s9.Pump(w10, l9, true);
    CompleteAll(s9, h9);
    CHECK(s9.ResidentGpu() == 80 * kMiB && s9.GpuRoom(l9) == 20 * kMiB);
    s9.ReserveGpu(30 * kMiB);
    s9.Pump(w10, l9, true);
    CHECK(h9.released.size() == 1 && s9.IsResident({600, Variant::Fit}));  // rank 1 made room
    CHECK(s9.CommittedGpu() == 70 * kMiB);
    s9.ReleaseGpu(30 * kMiB);
    CHECK(s9.CommittedGpu() == 40 * kMiB);
}

static void TestSettings() {
    std::puts("settings");
    MemoryCaps caps;
    caps.maxRam = 4 * kGiB;
    caps.maxVram = 3 * kGiB;
    caps.defaultRam = 1 * kGiB;
    caps.defaultVram = 1536 * kMiB;

    Settings s;
    s.ahead = 99;
    s.behind = -3;
    s.ramBudgetMB = 999999;
    s.vramBudgetMB = 1;
    Validate(s, caps);
    CHECK(s.ahead == kMaxAhead && s.behind == 0);
    CHECK(s.ramBudgetMB == 4096 && s.vramBudgetMB == kMinBudgetMB);

    s.autoBudget = false;
    CHECK(EffectiveRamSetting(s, caps) == 4 * kGiB);
    s.autoBudget = true;
    CHECK(EffectiveVramSetting(s, caps) == caps.defaultVram);

    s.disabledFormats = {"GIF", "RAW"};
    s.overlayCorner = 2;
    s.backdrop = 1;
    Settings back;
    Deserialize(Serialize(s), back);
    CHECK(back.ahead == s.ahead && back.behind == s.behind && back.overlayCorner == 2 && back.backdrop == 1);
    CHECK(back.disabledFormats.size() == 2 && back.disabledFormats[1] == "RAW");

    s.destinations[0] = "D:\\Keepers";
    s.destinations[8] = "Rejects";
    s.destinations[2] = "C:\\a = b #c";
    s.renameOnConflict = false;
    Settings back2;
    Deserialize(Serialize(s), back2);
    CHECK(back2.destinations[0] == "D:\\Keepers" && back2.destinations[8] == "Rejects" && back2.destinations[3].empty());
    CHECK(!back2.renameOnConflict && back2.pairRawJpeg);
    CHECK(back2.destinations[2] == "C:\\a = b #c");

    Settings junk;
    Deserialize("ahead=abc\n=5\nnope\nbehind=7\n", junk);
    CHECK(junk.ahead == 4 && junk.behind == 7);
}

// ---- Pairing ---------------------------------------------------------------

static std::vector<std::pair<size_t, std::vector<size_t>>> Groups(const std::vector<GroupInput>& in, GroupOptions o) {
    std::vector<std::pair<size_t, std::vector<size_t>>> r;
    for (auto& g : GroupFiles(in, o)) r.push_back({g.primary, g.companions});
    return r;
}

static void TestGroups() {
    std::puts("groups");
    using K = FileKind;
    // 0 a.jpg, 1 a.cr2, 2 a.xmp, 3 b.cr2, 4 c.png, 5 d.xmp, 6 notes.txt, 7 a.heic
    std::vector<GroupInput> in = {{L"a", K::Primary}, {L"a", K::Raw},     {L"a", K::Sidecar}, {L"b", K::Raw},
                                  {L"c", K::Primary}, {L"d", K::Sidecar}, {L"notes", K::Ignore}, {L"a", K::Primary}};
    using G = std::vector<std::pair<size_t, std::vector<size_t>>>;
    // Pairing, RAW format off: a.jpg carries a.cr2 + a.xmp; b.cr2 hidden; a.heic standalone.
    CHECK((Groups(in, {true, false}) == G{{0, {1, 2}}, {4, {}}, {7, {}}}));
    // Pairing, RAW on: lone RAW b.cr2 is shown.
    CHECK((Groups(in, {true, true}) == G{{0, {1, 2}}, {3, {}}, {4, {}}, {7, {}}}));
    // No pairing, RAW on: everything shown separately, sidecars ignored.
    CHECK((Groups(in, {false, true}) == G{{0, {}}, {1, {}}, {3, {}}, {4, {}}, {7, {}}}));
    // No pairing, RAW off.
    CHECK((Groups(in, {false, false}) == G{{0, {}}, {4, {}}, {7, {}}}));
    CHECK(GroupFiles({}, {}).empty());
    CHECK((SuffixedNames({L"IMG_1.jpg", L"IMG_1.CR2", L"IMG_1.xmp"}, 1) ==
           std::vector<std::wstring>{L"IMG_1.jpg", L"IMG_1.CR2", L"IMG_1.xmp"}));
    CHECK((SuffixedNames({L"IMG_1.jpg", L"IMG_1.CR2"}, 3) == std::vector<std::wstring>{L"IMG_1 (3).jpg", L"IMG_1 (3).CR2"}));
    CHECK((SuffixedNames({L"a.b.jpg", L"noext", L".hidden"}, 2) ==
           std::vector<std::wstring>{L"a.b (2).jpg", L"noext (2)", L".hidden (2)"}));
}

// ---- EXIF ------------------------------------------------------------------

struct TiffBuilder {
    bool be;
    std::vector<uint8_t> d;
    void U16(uint16_t v) {
        if (be) { d.push_back(uint8_t(v >> 8)); d.push_back(uint8_t(v)); }
        else { d.push_back(uint8_t(v)); d.push_back(uint8_t(v >> 8)); }
    }
    void U32(uint32_t v) {
        if (be) { U16(uint16_t(v >> 16)); U16(uint16_t(v)); }
        else { U16(uint16_t(v)); U16(uint16_t(v >> 16)); }
    }
    void Put16(size_t at, uint16_t v) {
        if (be) { d[at] = uint8_t(v >> 8); d[at + 1] = uint8_t(v); }
        else { d[at] = uint8_t(v); d[at + 1] = uint8_t(v >> 8); }
    }
    void Put32(size_t at, uint32_t v) {
        if (be) { Put16(at, uint16_t(v >> 16)); Put16(at + 2, uint16_t(v)); }
        else { Put16(at, uint16_t(v)); Put16(at + 2, uint16_t(v >> 16)); }
    }
};

// IFD entries: (tag, type, count, inline value or payload bytes)
struct Ent {
    uint16_t tag, type;
    uint32_t count;
    std::vector<uint8_t> payload;  // raw value bytes in the builder's byte order (filled by helpers)
};

static std::vector<uint8_t> BuildExif(bool be) {
    TiffBuilder b{be, {}};
    auto str = [&](const char* s) { std::vector<uint8_t> v(s, s + strlen(s) + 1); return v; };
    auto rat = [&](uint32_t n, uint32_t dd) {
        TiffBuilder t{be, {}};
        t.U32(n);
        t.U32(dd);
        return t.d;
    };
    auto sh = [&](uint16_t v) {
        TiffBuilder t{be, {}};
        t.U16(v);
        t.U16(0);
        return t.d;
    };
    std::vector<Ent> ifd0 = {{0x010F, 2, 6, str("Canon")}, {0x0110, 2, 13, str("Canon EOS R5")}, {0x8769, 4, 1, {}}};
    std::vector<Ent> exif = {{0x829A, 5, 1, rat(1, 250)},           {0x829D, 5, 1, rat(28, 10)},
                             {0x8827, 3, 1, sh(400)},               {0x9003, 2, 20, str("2024:05:01 12:34:56")},
                             {0x9204, 10, 1, rat(uint32_t(-1), 3)}, {0x920A, 5, 1, rat(50, 1)},
                             {0xA405, 3, 1, sh(75)},                {0xA434, 2, 24, str("RF24-70mm F2.8 L IS USM")}};
    b.d.push_back(be ? 'M' : 'I');
    b.d.push_back(be ? 'M' : 'I');
    b.U16(42);
    b.U32(8);
    auto writeIfd = [&](std::vector<Ent>& ents, size_t& exifPtrAt) {
        const size_t start = b.d.size();
        b.U16(uint16_t(ents.size()));
        std::vector<size_t> valAt;
        for (auto& e : ents) {
            b.U16(e.tag);
            b.U16(e.type);
            b.U32(e.count);
            valAt.push_back(b.d.size());
            b.U32(0);
        }
        b.U32(0);  // next IFD
        for (size_t i = 0; i < ents.size(); ++i) {
            auto& e = ents[i];
            if (e.tag == 0x8769) { exifPtrAt = valAt[i]; continue; }
            if (e.payload.size() <= 4) {
                for (size_t k = 0; k < e.payload.size(); ++k) b.d[valAt[i] + k] = e.payload[k];
            } else {
                b.Put32(valAt[i], uint32_t(b.d.size()));
                b.d.insert(b.d.end(), e.payload.begin(), e.payload.end());
            }
        }
        return start;
    };
    size_t exifPtrAt = 0, unused = 0;
    writeIfd(ifd0, exifPtrAt);
    const size_t exifStart = writeIfd(exif, unused);
    b.Put32(exifPtrAt, uint32_t(exifStart));
    return b.d;
}

static void TestExifInfo() {
    std::puts("exif info");
    for (bool be : {false, true}) {
        const auto tiff = BuildExif(be);
        PhotoInfo p;
        CHECK(ParseExifHead(tiff.data(), tiff.size(), p));  // TIFF-based file (DNG, NEF, ...)
        CHECK(p.make == "Canon" && p.model == "Canon EOS R5" && p.lens == "RF24-70mm F2.8 L IS USM");
        CHECK(p.exposureNum == 1 && p.exposureDen == 250 && p.iso == 400 && p.focal35 == 75);
        CHECK(p.fNumber > 2.79 && p.fNumber < 2.81 && p.focalLength == 50 && p.hasBias);
        CHECK(p.dateTaken == "2024:05:01 12:34:56");
        CHECK(CameraLine(p) == "Canon EOS R5  \xC2\xB7  RF24-70mm F2.8 L IS USM");
        CHECK(ExposureLine(p) == "1/250 s  \xC2\xB7  f/2.8  \xC2\xB7  ISO 400  \xC2\xB7  50 mm (75 mm eq.)  \xC2\xB7  -0.3 EV");

        // Same data inside a JPEG APP1 segment.
        std::vector<uint8_t> j = {0xFF, 0xD8};
        const size_t len = 2 + 6 + tiff.size();
        j.insert(j.end(), {0xFF, 0xE1, uint8_t(len >> 8), uint8_t(len), 'E', 'x', 'i', 'f', 0, 0});
        j.insert(j.end(), tiff.begin(), tiff.end());
        j.insert(j.end(), {0xFF, 0xDA, 0, 2});
        PhotoInfo q;
        CHECK(ParseExifHead(j.data(), j.size(), q) && q.model == p.model && q.iso == 400);
        // Every truncation must be safe (ASan checks the reads).
        for (size_t cut = 0; cut < j.size(); ++cut) {
            PhotoInfo r;
            (void)ParseExifHead(j.data(), cut, r);
        }
        // Corrupt offsets must be safe too.
        auto bad = j;
        for (size_t i = 12; i < bad.size(); i += 7) bad[i] ^= 0xA5;
        PhotoInfo r;
        (void)ParseExifHead(bad.data(), bad.size(), r);
    }
    CHECK(FormatExposure(1, 250) == "1/250 s");
    CHECK(FormatExposure(10, 2500) == "1/250 s");
    CHECK(FormatExposure(2, 1) == "2 s");
    CHECK(FormatExposure(13, 10) == "1.3 s");
    CHECK(FormatExposure(0, 0).empty());
    CHECK(FormatExifDate("2024:05:01 12:34:56") == "2024-05-01 12:34:56");
    PhotoInfo nikon;
    nikon.make = "NIKON CORPORATION";
    nikon.model = "NIKON Z 6";
    CHECK(CameraLine(nikon) == "NIKON Z 6");
    nikon.model = "Z 6";
    CHECK(CameraLine(nikon) == "NIKON CORPORATION Z 6");
}

static void TestHistogram() {
    std::puts("histogram");
    // 4 pixels BGRA: white, black, pure red, transparent
    const uint8_t px[] = {255, 255, 255, 255, 0, 0, 0, 255, 0, 0, 255, 255, 9, 9, 9, 0};
    Histogram h;
    ComputeHistogram(px, {2, 2}, h);
    CHECK(h.total == 3);
    CHECK(h.r[255] == 2 && h.g[255] == 1 && h.b[0] == 2 && h.luma[255] == 1 && h.luma[0] == 1);
    CHECK(h.clippedHigh == 2 && h.clippedLow == 1);
    CHECK(h.MaxBin() == 2);
    CHECK(HistogramLevel({4000, 3000}, FullMipLevels({4000, 3000})) == 3);  // 500x375
    CHECK(HistogramLevel({300, 200}, FullMipLevels({300, 200})) == 0);
}

static void TestMarks() {
    std::puts("marks");
    Mark m;
    CHECK(m.empty() && Matches(Filter::All, m) && Matches(Filter::Unflagged, m) && !Matches(Filter::Picks, m));
    m = TogglePick(m);
    CHECK(m.flag == Flag::Pick && Matches(Filter::Picks, m) && !Matches(Filter::Unflagged, m));
    m = ToggleReject(m);  // a reject replaces the pick
    CHECK(m.flag == Flag::Reject && Matches(Filter::Rejects, m) && !Matches(Filter::Picks, m));
    m = ToggleReject(m);
    CHECK(m.flag == Flag::None && m.empty());
    m = SetStars(m, 9);
    CHECK(m.stars == 5 && Matches(Filter::Rated, m) && !m.empty());
    CHECK(SetStars(m, -1).stars == 0);
    m = SetStars(Mark{}, 3);
    CHECK(Matches(Selection::Stars1, m) && Matches(Selection::Stars3, m) && !Matches(Selection::Stars4, m));
    CHECK(Matches(Selection::Unflagged, m) && !Matches(Selection::Picks, m));
    // Ctrl+F visits every filter and comes back to All.
    Filter f = Filter::All;
    for (int i = 0; i < int(Filter::Count); ++i) {
        CHECK(FilterName(f)[0] != 0);
        f = NextFilter(f);
    }
    CHECK(f == Filter::All);
    for (int i = 0; i < int(Selection::Count); ++i) CHECK(SelectionName(Selection(i))[0] != 0);
}

static void TestOrganize() {
    std::puts("organize");
    CHECK(ParseExifTime("1970:01:01 00:00:00") == 0);
    CHECK(ParseExifTime("2024:05:01 12:34:56") == 1714566896);
    CHECK(ParseExifTime("2024:02:29 00:00:01") - ParseExifTime("2024:02:28 00:00:01") == 86400);
    CHECK(ParseExifTime("2024:13:01 00:00:00") == -1 && ParseExifTime("    :  :     :  :  ") == -1 && ParseExifTime("") == -1);
    CHECK(FileTimeToUnix(116444736000000000ull) == 0);

    std::vector<SortItem> items(4);
    items[0] = {100, 50, 30, L".jpg"};
    items[1] = {-1, 90, 10, L".png"};  // unknown capture time: modified (90) is used
    items[2] = {80, 70, 30, L".cr2"};
    items[3] = {100, 10, 20, L".jpg"};
    CHECK((SortOrder(items, SortKey::Name, false) == std::vector<int>{0, 1, 2, 3}));
    CHECK((SortOrder(items, SortKey::Name, true) == std::vector<int>{3, 2, 1, 0}));
    CHECK((SortOrder(items, SortKey::Taken, false) == std::vector<int>{2, 1, 0, 3}));  // ties keep name order
    CHECK((SortOrder(items, SortKey::Taken, true) == std::vector<int>{3, 0, 1, 2}));
    CHECK((SortOrder(items, SortKey::Modified, false) == std::vector<int>{3, 0, 2, 1}));
    CHECK((SortOrder(items, SortKey::Size, false) == std::vector<int>{1, 3, 0, 2}));
    CHECK((SortOrder(items, SortKey::Type, false) == std::vector<int>{2, 0, 3, 1}));

    std::vector<BurstInput> b = {{10, "A"}, {11, "A"}, {12, "A"}, {30, "A"}, {31, "B"}, {32, "B"}, {-1, "B"}, {33, "B"}};
    const auto id = FindBursts(b, 2);
    CHECK(id[0] == 0 && id[1] == 0 && id[2] == 0);
    CHECK(id[3] == -1);                 // 18 s later, and the next one is another camera
    CHECK(id[4] == 1 && id[5] == 1);
    CHECK(id[6] == -1 && id[7] == -1);  // unknown time breaks the chain
    CHECK(FindBursts({}, 2).empty());

    // A sharp checkerboard scores far above the same pattern blurred, and a
    // flat image scores zero.
    const uint32_t w = 64, h = 64;
    std::vector<uint8_t> sharp(w * h * 4), soft(w * h * 4), flat(w * h * 4, 128);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t v = ((x / 2 + y / 2) & 1) ? 255 : 0;
            const uint8_t s = uint8_t(96 + (int(v) - 128) / 8);
            for (int c = 0; c < 4; ++c) {
                sharp[(y * w + x) * 4 + c] = v;
                soft[(y * w + x) * 4 + c] = s;
            }
        }
    const float a = Sharpness(sharp.data(), {w, h}), c = Sharpness(soft.data(), {w, h});
    CHECK(a > 10 * c && c > 0);
    CHECK(Sharpness(flat.data(), {w, h}) == 0);
    CHECK(Sharpness(flat.data(), {4, 4}) == 0);
}

static void TestAnimation() {
    std::puts("animation");
    const Size cs{4, 3};
    std::vector<uint32_t> canvas(12, 0xFF000001u);
    const uint32_t frame[4] = {0xFF0000AAu, 0x00000000u, 0xFF0000BBu, 0xFF0000CCu};  // 2 x 2, one transparent
    DrawFrame(canvas.data(), cs, frame, {1, 1, 2, 2});
    CHECK(canvas[1 * 4 + 1] == 0xFF0000AAu && canvas[1 * 4 + 2] == 0xFF000001u);  // transparent keeps the canvas
    CHECK(canvas[2 * 4 + 1] == 0xFF0000BBu && canvas[2 * 4 + 2] == 0xFF0000CCu);
    CHECK(canvas[0] == 0xFF000001u);
    DrawFrame(canvas.data(), cs, frame, {3, 2, 2, 2});  // clipped to the canvas corner
    CHECK(canvas[2 * 4 + 3] == 0xFF0000AAu);
    DrawFrame(canvas.data(), cs, frame, {-1, -1, 2, 2});  // only the bottom-right pixel lands
    CHECK(canvas[0] == 0xFF0000CCu);
    DrawFrame(canvas.data(), cs, frame, {10, 10, 2, 2});  // fully outside: nothing
    ClearRect(canvas.data(), cs, {1, 1, 2, 5});
    CHECK(canvas[1 * 4 + 1] == 0 && canvas[2 * 4 + 2] == 0 && canvas[2 * 4 + 3] == 0xFF0000AAu);
    CHECK(FrameDelayMs(0) == 100 && FrameDelayMs(1) == 100 && FrameDelayMs(4) == 40);
    CHECK(ToDisposal(2) == Disposal::Background && ToDisposal(3) == Disposal::Previous && ToDisposal(7) == Disposal::Keep);
}

static void TestRegion() {
    std::puts("region");
    // Cropping the stored image with StoredRect and then orienting it equals
    // cropping the oriented image, for every EXIF orientation.
    const Size stored{7, 5};
    std::vector<uint32_t> src(35);
    for (uint32_t i = 0; i < 35; ++i) src[i] = i + 1;
    for (int o = 1; o <= 8; ++o) {
        const Size os = OrientedSize(stored, o);
        std::vector<uint32_t> oriented(35);
        ApplyOrientation(src.data(), stored, o, oriented.data());
        const Rect r{1, 2, std::min(3u, os.w - 1), std::min(2u, os.h - 2)};
        const Rect sr = StoredRect(r, stored, o);
        CHECK(sr.x + sr.w <= stored.w && sr.y + sr.h <= stored.h);
        std::vector<uint32_t> crop(size_t(sr.w) * sr.h), cropOriented(crop.size());
        for (uint32_t y = 0; y < sr.h; ++y)
            for (uint32_t x = 0; x < sr.w; ++x) crop[y * sr.w + x] = src[(sr.y + y) * stored.w + sr.x + x];
        ApplyOrientation(crop.data(), {sr.w, sr.h}, o, cropOriented.data());
        bool same = OrientedSize({sr.w, sr.h}, o) == Size{r.w, r.h};
        for (uint32_t y = 0; same && y < r.h; ++y)
            for (uint32_t x = 0; x < r.w; ++x) same &= cropOriented[y * r.w + x] == oriented[(r.y + y) * os.w + r.x + x];
        CHECK(same);
    }
    // A region around the view, with margin, clamped to the image and the size cap.
    DecodePlan p = PlanRegion({40000, 30000}, {10000, 10000, 2000, 1000}, 0.25f, 4096);
    CHECK(p.valid() && p.mipLevels == 1 && p.region == (Rect{9500, 9750, 3000, 1500}));
    CHECK(p.size == (Size{3000, 1500}) && p.gpuBytes == 3000ull * 1500 * 4);
    p = PlanRegion({40000, 30000}, {0, 29500, 9000, 800}, 0.25f, 4096);  // capped, at the bottom edge
    CHECK(p.region.w == 4096 && p.region.y + p.region.h == 30000 && p.region.x == 2452);
    CHECK(!PlanRegion({100, 100}, {200, 0, 10, 10}, 0.25f, 4096).valid());
    CHECK((Rect{0, 0, 10, 10}.Contains(Rect{2, 2, 8, 8})) && !(Rect{0, 0, 10, 10}.Contains(Rect{2, 2, 9, 8})));
    // A region plan differs from the same size without a region.
    DecodePlan q = p;
    q.region = {};
    CHECK(q != p);
}

static void TestBc1() {
    std::puts("bc1");
    CHECK((BlockPadded({5, 8}) == Size{8, 8}) && Bc1LevelBytes({5, 8}) == 32 && Bc1LevelBytes({1, 1}) == 8);
    // A screen-sized plan compresses to an eighth (plus padding).
    PlanRequest r;
    r.source = {6000, 4000};
    r.target = {1920, 1080};
    const DecodePlan fit = PlanDecode(r), c = PlanCompact(fit);
    CHECK(c.valid() && c.compressed && c.size == fit.size && c != fit);
    CHECK(c.gpuBytes * 7 < fit.gpuBytes && c.gpuBytes * 9 > fit.gpuBytes);
    // Its transient memory includes the BGRA chain it is built from.
    TransientRequest tq;
    tq.stored = {6000, 4000};
    tq.plan = c;
    tq.readIntoMemory = false;
    CHECK(TransientBytes(tq) > MipChainBytes(fit.size, fit.mipLevels));

    // Round trip: flat colours come back almost exactly, a gradient within a
    // few levels, and a two-colour block keeps both colours.
    const Size s{10, 7};  // not a multiple of 4
    std::vector<uint8_t> img(size_t(s.w) * s.h * 4), back(img.size()), blocks(Bc1LevelBytes(s));
    for (uint32_t y = 0; y < s.h; ++y)
        for (uint32_t x = 0; x < s.w; ++x) {
            uint8_t* p = &img[(y * s.w + x) * 4];
            p[0] = uint8_t(40 + x * 12);  // B gradient
            p[1] = 128;
            p[2] = uint8_t(200 - y * 10);
            p[3] = 255;
        }
    EncodeBc1(img.data(), s, blocks.data());
    DecodeBc1(blocks.data(), s, back.data());
    int worst = 0, sum = 0;
    for (size_t i = 0; i < img.size(); ++i) {
        const int d = std::abs(int(img[i]) - int(back[i]));
        worst = std::max(worst, d);
        sum += d;
    }
    std::printf("  bc1 gradient error: mean %.2f, worst %d\n", double(sum) / double(img.size()), worst);
    CHECK(sum <= int(img.size()) * 5 && worst <= 32);  // BC1 is lossy: a 2D gradient within a block is approximated
    std::vector<uint8_t> two(16 * 4);
    for (int i = 0; i < 16; ++i) {
        const bool white = (i & 1) != 0;
        two[i * 4 + 0] = two[i * 4 + 1] = two[i * 4 + 2] = white ? 255 : 0;
        two[i * 4 + 3] = 255;
    }
    std::vector<uint8_t> twoBack(two.size()), twoBlocks(8);
    EncodeBc1(two.data(), {4, 4}, twoBlocks.data());
    DecodeBc1(twoBlocks.data(), {4, 4}, twoBack.data());
    CHECK(twoBack == two);
    CHECK(!HasAlpha(img.data(), s));
    img[3] = 200;
    CHECK(HasAlpha(img.data(), s));
}

static void TestKeys() {
    std::puts("keys");
    for (size_t i = 0; i < AllActions().size(); ++i) CHECK(int(AllActions()[i].action) == int(i));
    CHECK(int(AllActions().size()) == kActionCount);
    Chord c;
    CHECK(ParseChord("Ctrl+Shift+Z", c) && c.vk == 'Z' && c.ctrl && c.shift && !c.alt);
    CHECK(ChordToString(c) == "Ctrl+Shift+Z");
    CHECK(ParseChord("ctrl + num+", c) && c.vk == 0x6B && c.ctrl);
    CHECK(ParseChord("F11", c) && c.vk == 0x7A);
    CHECK(ParseChord("pgdn", c) && c.vk == 0x22 && ChordToString(c) == "PgDn");
    CHECK(!ParseChord("Ctrl+", c) && !ParseChord("Hyper+X", c) && !ParseChord("", c) && !ParseChord("Shift", c));
    std::vector<Chord> list;
    CHECK(ParseChords("Right, Down, Space", list) && list.size() == 3 && ChordsToString(list) == "Right, Down, Space");
    CHECK(ParseChords("", list) && list.empty());  // explicitly unbound
    CHECK(!ParseChords("Right, Nope", list));
    // Every default binding parses, and defaults have no duplicates between actions.
    KeyMap km;
    km.Build({});
    for (const auto& info : AllActions()) {
        std::vector<Chord> d;
        CHECK(ParseChords(info.defaults, d) && !d.empty());
        CHECK(km.Conflicts(info.action, d).empty());
    }
    Chord ctrlZ;
    ParseChord("Ctrl+Z", ctrlZ);
    CHECK(km.Lookup(ctrlZ) == Action::Undo);
    // Overrides replace defaults; invalid overrides are ignored.
    km.Build({{"Undo", "Ctrl+Y"}, {"Next", "garbage+"}});
    CHECK(km.Lookup(ctrlZ) == Action::None);
    Chord right;
    ParseChord("Right", right);
    CHECK(km.Lookup(right) == Action::Next);

    Settings s;
    s.keyBindings["Undo"] = "Ctrl+Y, Ctrl+Z";
    s.editorPath = "C:\\Program Files\\GIMP\\gimp.exe";
    Settings back;
    Deserialize(Serialize(s), back);
    CHECK(back.keyBindings.size() == 1 && back.keyBindings["Undo"] == "Ctrl+Y, Ctrl+Z" && back.editorPath == s.editorPath);
}

int main() {
    TestWindow();
    TestPlan();
    TestOrientation();
    TestExif();
    TestMips();
    TestCaps();
    TestScheduler();
    TestSettings();
    TestGroups();
    TestExifInfo();
    TestHistogram();
    TestBc1();
    TestRegion();
    TestAnimation();
    TestOrganize();
    TestMarks();
    TestKeys();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
