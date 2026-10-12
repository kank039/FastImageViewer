#pragma once
// BC1 (DXT1) texture compression for the compact history tier: 4 bits per
// pixel instead of 32, so eight times as many already-seen images fit in the
// same video memory. Opaque images only (BC1 has 1-bit alpha at best).
#include <cstdint>

#include "DecodePlan.h"

namespace fiv::core {

Size BlockPadded(Size s);  // rounded up to whole 4 x 4 blocks
uint64_t Bc1LevelBytes(Size level);
uint64_t Bc1ChainBytes(Size padded, uint32_t levels);

// The compressed counterpart of a (screen-sized, mipmapped) BGRA plan.
DecodePlan PlanCompact(const DecodePlan& bgra);

// Encodes one BGRA8 level of any size (edge blocks repeat the last row and
// column) into Bc1LevelBytes(s) bytes. Endpoints from the block's luma
// extremes, then the nearest of the four palette colours per pixel.
void EncodeBc1(const uint8_t* bgra, Size s, uint8_t* out);

// Reference decoder (tests): one level back to BGRA8.
void DecodeBc1(const uint8_t* blocks, Size s, uint8_t* bgra);

// True if any pixel is not fully opaque (such images are not compressed).
bool HasAlpha(const uint8_t* bgra, Size s);

}  // namespace fiv::core
