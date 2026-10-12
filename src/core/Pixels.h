#pragma once
// CPU pixel operations on tightly packed 32-bit pixels.
#include <cstddef>
#include <cstdint>

#include "DecodePlan.h"

namespace fiv::core {

// EXIF orientation 1..8. Anything else is treated as 1.
bool OrientationSwapsAxes(int orientation);
Size OrientedSize(Size stored, int orientation);

// EXIF orientation (1..8) from the start of a JPEG file, or 0 if absent.
// Bounds-checked: safe on truncated or hostile data.
int JpegExifOrientation(const uint8_t* data, size_t size);

// Writes the display-oriented image into dst (size OrientedSize(stored)).
void ApplyOrientation(const uint32_t* src, Size stored, int orientation, uint32_t* dst);

// Fills levels 1..levels-1 of a contiguous mip chain whose level 0 is already
// at the start of `chain` (BGRA8, box filter).
void GenerateMips(uint8_t* chain, Size level0, uint32_t levels);

}  // namespace fiv::core
