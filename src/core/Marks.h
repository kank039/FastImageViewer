#pragma once
// Session-only culling marks: pick / reject flags and 0-5 stars. They live in
// RAM for as long as the program runs and are never written to the image
// files or anywhere else.
#include <cstdint>

namespace fiv::core {

enum class Flag : int8_t { None = 0, Pick = 1, Reject = -1 };

struct Mark {
    Flag flag = Flag::None;
    int stars = 0;  // 0..5
    bool empty() const { return flag == Flag::None && stars == 0; }
    bool operator==(const Mark& o) const { return flag == o.flag && stars == o.stars; }
};

// What the viewer shows (Ctrl+F cycles through these).
enum class Filter : int { All, Picks, Rated, Unflagged, Rejects, Count };
const char* FilterName(Filter f);
Filter NextFilter(Filter f);
bool Matches(Filter f, const Mark& m);

// Which images a batch action applies to (Picks panel).
enum class Selection : int { Picks, Rejects, Stars1, Stars2, Stars3, Stars4, Stars5, Unflagged, Count };
const char* SelectionName(Selection s);
bool Matches(Selection s, const Mark& m);

// Keyboard edits. Pick and Reject toggle; Unflag clears the flag and the stars.
Mark TogglePick(Mark m);
Mark ToggleReject(Mark m);
Mark SetStars(Mark m, int stars);

}  // namespace fiv::core
