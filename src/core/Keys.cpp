#include "Keys.h"

#include <algorithm>
#include <cctype>

namespace fiv::core {

namespace {

struct KeyName {
    const char* name;
    uint16_t vk;
};

// Windows virtual-key codes.
const KeyName kNames[] = {
    {"Backspace", 0x08}, {"Tab", 0x09},      {"Enter", 0x0D},     {"Pause", 0x13},    {"Esc", 0x1B},
    {"Space", 0x20},     {"PgUp", 0x21},     {"PgDn", 0x22},      {"End", 0x23},      {"Home", 0x24},
    {"Left", 0x25},      {"Up", 0x26},       {"Right", 0x27},     {"Down", 0x28},     {"Insert", 0x2D},
    {"Delete", 0x2E},    {"Num0", 0x60},     {"Num1", 0x61},      {"Num2", 0x62},     {"Num3", 0x63},
    {"Num4", 0x64},      {"Num5", 0x65},     {"Num6", 0x66},      {"Num7", 0x67},     {"Num8", 0x68},
    {"Num9", 0x69},      {"Num*", 0x6A},     {"Num+", 0x6B},      {"Num-", 0x6D},     {"Num.", 0x6E},
    {"Num/", 0x6F},      {"Semicolon", 0xBA}, {"Plus", 0xBB},     {"Comma", 0xBC},    {"Minus", 0xBD},
    {"Period", 0xBE},    {"Slash", 0xBF},    {"Backtick", 0xC0},  {"LBracket", 0xDB}, {"Backslash", 0xDC},
    {"RBracket", 0xDD},  {"Quote", 0xDE},
};

bool IEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

std::string KeyToString(uint16_t vk) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, char(vk));
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x70 + 1);
    for (const auto& k : kNames)
        if (k.vk == vk) return k.name;
    return "VK" + std::to_string(vk);
}

bool StringToKey(const std::string& s, uint16_t& vk) {
    if (s.size() == 1) {
        const char c = char(std::toupper(static_cast<unsigned char>(s[0])));
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) {
            vk = uint16_t(c);
            return true;
        }
    }
    if (s.size() >= 2 && (s[0] == 'F' || s[0] == 'f') && std::all_of(s.begin() + 1, s.end(), ::isdigit)) {
        const int n = std::stoi(s.substr(1));
        if (n >= 1 && n <= 24) {
            vk = uint16_t(0x70 + n - 1);
            return true;
        }
    }
    for (const auto& k : kNames)
        if (IEquals(s, k.name)) {
            vk = k.vk;
            return true;
        }
    if (s.size() > 2 && IEquals(s.substr(0, 2), "VK") && std::all_of(s.begin() + 2, s.end(), ::isdigit)) {
        const int n = std::stoi(s.substr(2));
        if (n > 0 && n < 256) {
            vk = uint16_t(n);
            return true;
        }
    }
    return false;
}

}  // namespace

const std::vector<ActionInfo>& AllActions() {
    static const std::vector<ActionInfo> kActions = {
        {Action::Next, "Next", "Next image", "Right, Down, Space", true},
        {Action::Prev, "Prev", "Previous image", "Left, Up, Backspace", true},
        {Action::NextSet, "NextSet", "Next set (compare: whole set)", "PgDn", true},
        {Action::PrevSet, "PrevSet", "Previous set", "PgUp", true},
        {Action::First, "First", "First image", "Home", false},
        {Action::Last, "Last", "Last image", "End", false},
        {Action::ZoomIn, "ZoomIn", "Zoom in", "Plus, Num+", true},
        {Action::ZoomOut, "ZoomOut", "Zoom out", "Minus, Num-", true},
        {Action::Fit, "Fit", "Fit to window", "0, Num0", false},
        {Action::ActualSize, "ActualSize", "Toggle 100% / fit", "Z", false},
        {Action::Fullscreen, "Fullscreen", "Fullscreen", "F, F11, Alt+Enter", false},
        {Action::RotateCW, "RotateCW", "Rotate view clockwise", "R", false},
        {Action::RotateCCW, "RotateCCW", "Rotate view counter-clockwise", "Shift+R", false},
        {Action::Compare, "Compare", "Compare 2 / 4 / off", "C", false},
        {Action::CompareBurst, "CompareBurst", "Compare the current burst", "Shift+C", false},
        {Action::NextTile, "NextTile", "Next tile (compare)", "Tab", true},
        {Action::PrevTile, "PrevTile", "Previous tile (compare)", "Shift+Tab", true},
        {Action::Grid, "Grid", "Grid view", "G", false},
        {Action::Peaking, "Peaking", "Focus peaking", "K", false},
        {Action::Delete, "Delete", "Move to Recycle Bin", "Delete", false},
        {Action::Move1, "Move1", "Move to destination 1", "1", false},
        {Action::Move2, "Move2", "Move to destination 2", "2", false},
        {Action::Move3, "Move3", "Move to destination 3", "3", false},
        {Action::Move4, "Move4", "Move to destination 4", "4", false},
        {Action::Move5, "Move5", "Move to destination 5", "5", false},
        {Action::Move6, "Move6", "Move to destination 6", "6", false},
        {Action::Move7, "Move7", "Move to destination 7", "7", false},
        {Action::Move8, "Move8", "Move to destination 8", "8", false},
        {Action::Move9, "Move9", "Move to destination 9", "9", false},
        {Action::Undo, "Undo", "Undo delete / move", "Ctrl+Z", false},
        {Action::Pick, "Pick", "Flag as pick", "P", false},
        {Action::Reject, "Reject", "Flag as reject", "X", false},
        {Action::Unflag, "Unflag", "Remove flag", "U", false},
        {Action::Rate0, "Rate0", "Clear stars", "Ctrl+0", false},
        {Action::Rate1, "Rate1", "1 star", "Ctrl+1", false},
        {Action::Rate2, "Rate2", "2 stars", "Ctrl+2", false},
        {Action::Rate3, "Rate3", "3 stars", "Ctrl+3", false},
        {Action::Rate4, "Rate4", "4 stars", "Ctrl+4", false},
        {Action::Rate5, "Rate5", "5 stars", "Ctrl+5", false},
        {Action::PicksPanel, "PicksPanel", "Picks panel (apply flags)", "Shift+P", false},
        {Action::CycleFilter, "CycleFilter", "Show: all / picks / not rejected / rejects", "Ctrl+F", false},
        {Action::Slideshow, "Slideshow", "Slideshow", "L", false},
        {Action::CopyImage, "CopyImage", "Copy image", "Ctrl+C", false},
        {Action::CopyPath, "CopyPath", "Copy file path", "Ctrl+Shift+C", false},
        {Action::ShowInExplorer, "ShowInExplorer", "Show in Explorer", "Ctrl+E", false},
        {Action::OpenInEditor, "OpenInEditor", "Open in editor", "E", false},
        {Action::ToggleOverlay, "ToggleOverlay", "Overlay", "O", false},
        {Action::ToggleSidebar, "ToggleSidebar", "Sidebar", "B", false},
        {Action::ToggleInfo, "ToggleInfo", "Info panel", "I", false},
        {Action::ToggleStats, "ToggleStats", "Stats panel", "F3", false},
        {Action::Settings, "Settings", "Settings", "S", false},
        {Action::Help, "Help", "Help", "H, F1", false},
        {Action::OpenFile, "OpenFile", "Open image", "Ctrl+O", false},
        {Action::OpenFolder, "OpenFolder", "Open folder", "Ctrl+Shift+O", false},
        {Action::Rescan, "Rescan", "Rescan folder", "F5", false},
    };
    return kActions;
}

const ActionInfo& InfoFor(Action a) { return AllActions()[size_t(a)]; }

bool IsModifierKey(uint16_t vk) {
    return vk == 0x10 || vk == 0x11 || vk == 0x12 || vk == 0x5B || vk == 0x5C || (vk >= 0xA0 && vk <= 0xA5) || vk == 0x14;
}

std::string ChordToString(const Chord& c) {
    if (c.empty()) return {};
    std::string s;
    if (c.ctrl) s += "Ctrl+";
    if (c.shift) s += "Shift+";
    if (c.alt) s += "Alt+";
    return s + KeyToString(c.vk);
}

bool ParseChord(const std::string& text, Chord& out) {
    Chord c;
    std::string rest = Trim(text);
    if (rest.empty()) return false;
    for (;;) {
        // A trailing "+" belongs to the key name only for "Num+" (handled by StringToKey).
        const size_t plus = rest.find('+');
        if (plus == std::string::npos || plus + 1 == rest.size()) break;
        const std::string mod = Trim(rest.substr(0, plus));
        if (IEquals(mod, "Ctrl") || IEquals(mod, "Control")) c.ctrl = true;
        else if (IEquals(mod, "Shift")) c.shift = true;
        else if (IEquals(mod, "Alt")) c.alt = true;
        else break;
        rest = Trim(rest.substr(plus + 1));
    }
    if (!StringToKey(rest, c.vk) || IsModifierKey(c.vk)) return false;
    out = c;
    return true;
}

std::string ChordsToString(const std::vector<Chord>& chords) {
    std::string s;
    for (const auto& c : chords) s += (s.empty() ? "" : ", ") + ChordToString(c);
    return s;
}

bool ParseChords(const std::string& text, std::vector<Chord>& out) {
    std::vector<Chord> chords;
    size_t start = 0;
    const std::string t = Trim(text);
    if (t.empty()) {
        out.clear();
        return true;
    }
    while (start <= t.size()) {
        const size_t comma = t.find(',', start);
        const std::string part = t.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        Chord c;
        if (!ParseChord(part, c)) return false;
        if (std::find(chords.begin(), chords.end(), c) == chords.end()) chords.push_back(c);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    out = std::move(chords);
    return true;
}

void KeyMap::Build(const std::map<std::string, std::string>& overrides) {
    for (const auto& info : AllActions()) {
        std::vector<Chord> chords;
        ParseChords(info.defaults, chords);
        auto it = overrides.find(info.id);
        std::vector<Chord> custom;
        if (it != overrides.end() && ParseChords(it->second, custom)) chords = custom;
        bindings_[size_t(info.action)] = chords;
    }
}

Action KeyMap::Lookup(const Chord& c) const {
    for (int a = 0; a < kActionCount; ++a)
        for (const auto& b : bindings_[size_t(a)])
            if (b == c) return Action(a);
    return Action::None;
}

std::vector<Action> KeyMap::Conflicts(Action a, const std::vector<Chord>& chords) const {
    std::vector<Action> out;
    for (int other = 0; other < kActionCount; ++other) {
        if (Action(other) == a) continue;
        for (const auto& b : bindings_[size_t(other)])
            if (std::find(chords.begin(), chords.end(), b) != chords.end()) {
                out.push_back(Action(other));
                break;
            }
    }
    return out;
}

}  // namespace fiv::core
