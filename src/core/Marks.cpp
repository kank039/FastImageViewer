#include "Marks.h"

#include <algorithm>

namespace fiv::core {

const char* FilterName(Filter f) {
    switch (f) {
        case Filter::All: return "All images";
        case Filter::Picks: return "Picks";
        case Filter::Rated: return "Rated (1+ stars)";
        case Filter::Unflagged: return "Not picked or rejected";
        case Filter::Rejects: return "Rejects";
        default: return "";
    }
}

Filter NextFilter(Filter f) { return Filter((int(f) + 1) % int(Filter::Count)); }

bool Matches(Filter f, const Mark& m) {
    switch (f) {
        case Filter::All: return true;
        case Filter::Picks: return m.flag == Flag::Pick;
        case Filter::Rated: return m.stars > 0;
        case Filter::Unflagged: return m.flag == Flag::None;
        case Filter::Rejects: return m.flag == Flag::Reject;
        default: return false;
    }
}

const char* SelectionName(Selection s) {
    switch (s) {
        case Selection::Picks: return "Picked";
        case Selection::Rejects: return "Rejected";
        case Selection::Stars1: return "1+ stars";
        case Selection::Stars2: return "2+ stars";
        case Selection::Stars3: return "3+ stars";
        case Selection::Stars4: return "4+ stars";
        case Selection::Stars5: return "5 stars";
        case Selection::Unflagged: return "Not picked or rejected";
        default: return "";
    }
}

bool Matches(Selection s, const Mark& m) {
    switch (s) {
        case Selection::Picks: return m.flag == Flag::Pick;
        case Selection::Rejects: return m.flag == Flag::Reject;
        case Selection::Stars1: case Selection::Stars2: case Selection::Stars3: case Selection::Stars4:
        case Selection::Stars5: return m.stars >= int(s) - int(Selection::Stars1) + 1;
        case Selection::Unflagged: return m.flag == Flag::None;
        default: return false;
    }
}

Mark TogglePick(Mark m) {
    m.flag = m.flag == Flag::Pick ? Flag::None : Flag::Pick;
    return m;
}

Mark ToggleReject(Mark m) {
    m.flag = m.flag == Flag::Reject ? Flag::None : Flag::Reject;
    return m;
}

Mark SetStars(Mark m, int stars) {
    m.stars = std::clamp(stars, 0, 5);
    return m;
}

}  // namespace fiv::core
