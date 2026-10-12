#include "Settings.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

#include "DecodePlan.h"

namespace fiv::core {

static uint32_t ToMB(uint64_t bytes) { return uint32_t(bytes / kMiB); }

void Validate(Settings& s, const MemoryCaps& caps) {
    s.ahead = std::clamp(s.ahead, 0, kMaxAhead);
    s.behind = std::clamp(s.behind, 0, kMaxBehind);
    s.overlayCorner = std::clamp(s.overlayCorner, 0, 3);
    s.sidebarWidth = std::clamp(s.sidebarWidth, 160, 900);
    s.thumbHeight = std::clamp(s.thumbHeight, 32, 240);
    s.decodeMode = std::clamp(s.decodeMode, 0, 1);
    s.backdrop = std::clamp(s.backdrop, 0, 2);
    s.slideshowSeconds = std::clamp(s.slideshowSeconds, 1, 120);

    const uint32_t maxRam = std::max(kMinBudgetMB, ToMB(caps.maxRam));
    const uint32_t maxVram = std::max(kMinBudgetMB, ToMB(caps.maxVram));
    if (s.ramBudgetMB == 0) s.ramBudgetMB = ToMB(caps.defaultRam);
    if (s.vramBudgetMB == 0) s.vramBudgetMB = ToMB(caps.defaultVram);
    s.ramBudgetMB = std::clamp(s.ramBudgetMB, kMinBudgetMB, maxRam);
    s.vramBudgetMB = caps.unified ? 0 : std::clamp(s.vramBudgetMB, kMinBudgetMB, maxVram);
}

uint64_t EffectiveRamSetting(const Settings& s, const MemoryCaps& caps) {
    if (s.autoBudget) return caps.defaultRam;
    return std::min<uint64_t>(uint64_t(s.ramBudgetMB) * kMiB, caps.maxRam);
}

uint64_t EffectiveVramSetting(const Settings& s, const MemoryCaps& caps) {
    if (caps.unified) return 0;
    if (s.autoBudget) return caps.defaultVram;
    return std::min<uint64_t>(uint64_t(s.vramBudgetMB) * kMiB, caps.maxVram);
}

namespace {

struct Field {
    const char* key;
    enum Kind { Bool, Int, UInt, Str } kind;
    void* ptr;
};

std::vector<Field> Fields(Settings& s) {
    static const char* kDest[9] = {"dest1", "dest2", "dest3", "dest4", "dest5", "dest6", "dest7", "dest8", "dest9"};
    std::vector<Field> f = {
        {"wrapAround", Field::Bool, &s.wrapAround},
        {"closeWhenEmpty", Field::Bool, &s.closeWhenEmpty},
        {"ahead", Field::Int, &s.ahead},
        {"behind", Field::Int, &s.behind},
        {"autoBudget", Field::Bool, &s.autoBudget},
        {"ramBudgetMB", Field::UInt, &s.ramBudgetMB},
        {"vramBudgetMB", Field::UInt, &s.vramBudgetMB},
        {"decodeMode", Field::Int, &s.decodeMode},
        {"releaseWhenMinimized", Field::Bool, &s.releaseWhenMinimized},
        {"skipCloudFiles", Field::Bool, &s.skipCloudFiles},
        {"backdrop", Field::Int, &s.backdrop},
        {"upscaleSmall", Field::Bool, &s.upscaleSmall},
        {"pixelatedZoom", Field::Bool, &s.pixelatedZoom},
        {"showOverlay", Field::Bool, &s.showOverlay},
        {"overlayCorner", Field::Int, &s.overlayCorner},
        {"ovName", Field::Bool, &s.ovName},
        {"ovType", Field::Bool, &s.ovType},
        {"ovIndex", Field::Bool, &s.ovIndex},
        {"ovSize", Field::Bool, &s.ovSize},
        {"ovDims", Field::Bool, &s.ovDims},
        {"ovZoom", Field::Bool, &s.ovZoom},
        {"showSidebar", Field::Bool, &s.showSidebar},
        {"sidebarWidth", Field::Int, &s.sidebarWidth},
        {"thumbHeight", Field::Int, &s.thumbHeight},
        {"showStats", Field::Bool, &s.showStats},
        {"winX", Field::Int, &s.winX},
        {"winY", Field::Int, &s.winY},
        {"winW", Field::Int, &s.winW},
        {"winH", Field::Int, &s.winH},
        {"winMaximized", Field::Bool, &s.winMaximized},
        {"showInfo", Field::Bool, &s.showInfo},
        {"pairRawJpeg", Field::Bool, &s.pairRawJpeg},
        {"renameOnConflict", Field::Bool, &s.renameOnConflict},
        {"wheelNavigates", Field::Bool, &s.wheelNavigates},
        {"slideshowSeconds", Field::Int, &s.slideshowSeconds},
        {"editorPath", Field::Str, &s.editorPath},
        {"autoAdvance", Field::Bool, &s.autoAdvance},
    };
    for (int i = 0; i < 9; ++i) f.push_back({kDest[i], Field::Str, &s.destinations[size_t(i)]});
    return f;
}

std::string Trim(const std::string& v) {
    size_t a = v.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = v.find_last_not_of(" \t\r\n");
    return v.substr(a, b - a + 1);
}

}  // namespace

std::string Serialize(const Settings& in) {
    Settings s = in;  // Fields() needs mutable pointers
    std::ostringstream o;
    o << "# FastImageViewer settings\n";
    for (const Field& f : Fields(s)) {
        o << f.key << '=';
        switch (f.kind) {
            case Field::Bool: o << (*static_cast<bool*>(f.ptr) ? 1 : 0); break;
            case Field::Int: o << *static_cast<int*>(f.ptr); break;
            case Field::UInt: o << *static_cast<uint32_t*>(f.ptr); break;
            case Field::Str: o << *static_cast<std::string*>(f.ptr); break;
        }
        o << '\n';
    }
    for (const auto& [id, chords] : s.keyBindings) o << "key." << id << '=' << chords << '\n';
    o << "disabledFormats=";
    for (size_t i = 0; i < s.disabledFormats.size(); ++i) o << (i ? "," : "") << s.disabledFormats[i];
    o << '\n';
    return o.str();
}

void Deserialize(const std::string& text, Settings& s) {
    auto fields = Fields(s);
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::string raw = line;  // destination paths keep their exact characters
        while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));
        if (key.rfind("key.", 0) == 0 && key.size() > 4) {
            s.keyBindings[key.substr(4)] = val;
            continue;
        }
        if (key == "disabledFormats") {
            s.disabledFormats.clear();
            std::istringstream list(val);
            std::string item;
            while (std::getline(list, item, ','))
                if (!Trim(item).empty()) s.disabledFormats.push_back(Trim(item));
            continue;
        }
        for (const Field& f : fields) {
            if (key != f.key) continue;
            if (f.kind == Field::Str) {
                *static_cast<std::string*>(f.ptr) = raw.substr(raw.find('=') + 1).substr(0, 1024);
                break;
            }
            char* end = nullptr;
            const long long n = std::strtoll(val.c_str(), &end, 10);
            if (end == val.c_str()) break;
            switch (f.kind) {
                case Field::Bool: *static_cast<bool*>(f.ptr) = n != 0; break;
                case Field::Int: *static_cast<int*>(f.ptr) = int(std::clamp<long long>(n, -1000000, 1000000)); break;
                case Field::UInt: *static_cast<uint32_t*>(f.ptr) = uint32_t(std::clamp<long long>(n, 0, 1 << 30)); break;
                case Field::Str: break;
            }
            break;
        }
    }
}

}  // namespace fiv::core
