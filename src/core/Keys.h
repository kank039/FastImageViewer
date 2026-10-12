#pragma once
// Keyboard actions and rebindable shortcuts. Portable: virtual-key codes are
// plain numbers here (they match the Windows VK_* values).
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fiv::core {

enum class Action : int {
    Next, Prev, NextSet, PrevSet, First, Last,
    ZoomIn, ZoomOut, Fit, ActualSize, Fullscreen, RotateCW, RotateCCW,
    Compare, CompareBurst, NextTile, PrevTile, Grid, Peaking,
    Delete, Move1, Move2, Move3, Move4, Move5, Move6, Move7, Move8, Move9, Undo,
    Pick, Reject, Unflag, Rate0, Rate1, Rate2, Rate3, Rate4, Rate5, PicksPanel, CycleFilter,
    Slideshow, CopyImage, CopyPath, ShowInExplorer, OpenInEditor,
    ToggleOverlay, ToggleSidebar, ToggleInfo, ToggleStats, Settings, Help,
    OpenFile, OpenFolder, Rescan,
    CommandPalette, UndoHistory, ShowWelcome,
    Count,
    None = -1,
};
constexpr int kActionCount = int(Action::Count);

struct Chord {
    uint16_t vk = 0;
    bool ctrl = false, shift = false, alt = false;
    bool empty() const { return vk == 0; }
    bool operator==(const Chord& o) const { return vk == o.vk && ctrl == o.ctrl && shift == o.shift && alt == o.alt; }
    bool operator!=(const Chord& o) const { return !(*this == o); }
};

struct ActionInfo {
    Action action;
    const char* id;        // stable name used in the settings file
    const char* label;     // shown in Settings > Keys and Help
    const char* defaults;  // default chords, e.g. "Right, Down, Space"
    bool repeats;          // holding the key repeats the action
};
const std::vector<ActionInfo>& AllActions();
const ActionInfo& InfoFor(Action a);

std::string ChordToString(const Chord& c);           // "Ctrl+Shift+Z"
bool ParseChord(const std::string& text, Chord& out);  // case-insensitive
std::string ChordsToString(const std::vector<Chord>& chords);  // "Right, Down"
bool ParseChords(const std::string& text, std::vector<Chord>& out);  // "" = unbound

bool IsModifierKey(uint16_t vk);

// Command palette matching: every query character must appear in order
// (case-insensitive). Higher is better; -1 = no match. Rewards matches at
// word starts and runs of consecutive characters, so "mv k" finds
// "Move to Keepers" before "Remove marks".
int FuzzyScore(const std::string& query, const std::string& text);

class KeyMap {
public:
    // Defaults, then user overrides (action id -> chord list text). Invalid
    // overrides are ignored so a bad settings file can never lock you out.
    void Build(const std::map<std::string, std::string>& overrides);
    Action Lookup(const Chord& c) const;
    const std::vector<Chord>& Bindings(Action a) const { return bindings_[size_t(a)]; }
    // Other actions using any of `chords` (for conflict warnings).
    std::vector<Action> Conflicts(Action a, const std::vector<Chord>& chords) const;

private:
    std::array<std::vector<Chord>, kActionCount> bindings_;
};

}  // namespace fiv::core
