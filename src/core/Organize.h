#pragma once
// Sorting, burst detection and a focus score: the "which one do I keep"
// helpers. Portable and unit tested.
#include <cstdint>
#include <string>
#include <vector>

#include "DecodePlan.h"

namespace fiv::core {

// EXIF "YYYY:MM:DD HH:MM:SS" (camera local time) as seconds since 1970-01-01,
// or -1 if malformed.
int64_t ParseExifTime(const std::string& s);
// FILETIME (100 ns ticks since 1601) as seconds since 1970.
int64_t FileTimeToUnix(uint64_t fileTime);

enum class SortKey : int { Name, Taken, Modified, Size, Type, Count };
const char* SortName(SortKey k);

struct SortItem {
    int64_t taken = -1;     // capture time, -1 unknown (falls back to modified)
    int64_t modified = 0;   // seconds
    uint64_t size = 0;
    std::wstring type;      // lower-case extension
};
// Indices into `items` in display order. Input order is the name order and
// breaks every tie, so equal keys keep Explorer order.
std::vector<int> SortOrder(const std::vector<SortItem>& items, SortKey key, bool descending);

struct BurstInput {
    int64_t taken = -1;     // -1 unknown: never part of a burst
    std::string camera;     // shots from different cameras are never one burst
};
// Burst id per element: consecutive shots at most gapSeconds apart share an
// id; -1 for images that are not part of a burst of 2 or more.
std::vector<int> FindBursts(const std::vector<BurstInput>& in, int gapSeconds);

// Focus measure of a BGRA8 image: variance of the Laplacian of luma in the
// sharpest cell of a 4 x 4 grid (so a sharp subject on a soft background
// still scores high). Higher is sharper; 0 for images smaller than 8 x 8.
float Sharpness(const uint8_t* bgra, Size size);

}  // namespace fiv::core
