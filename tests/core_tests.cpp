// Unit tests for the portable core. No framework: build and run `core_tests`.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/core/CacheWindow.h"
#include "../src/core/DecodePlan.h"
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

    Settings junk;
    Deserialize("ahead=abc\n=5\nnope\nbehind=7\n", junk);
    CHECK(junk.ahead == 4 && junk.behind == 7);
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
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
