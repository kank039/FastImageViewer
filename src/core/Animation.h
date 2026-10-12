#pragma once
// GIF frame compositing. Frames are drawn one after another onto a canvas
// the size of the animation; each frame says what happens to its area
// before the next one is drawn (its "disposal").
#include <cstdint>

#include "DecodePlan.h"

namespace fiv::core {

struct FrameRect {
    int x = 0, y = 0, w = 0, h = 0;
};

enum class Disposal : int { None = 0, Keep = 1, Background = 2, Previous = 3 };
Disposal ToDisposal(int gifValue);  // unknown values behave like Keep

// Draws a BGRA frame (r.w x r.h) onto the canvas; transparent pixels (alpha
// 0) leave the canvas as it was. The rectangle is clipped to the canvas.
void DrawFrame(uint32_t* canvas, Size canvasSize, const uint32_t* frame, FrameRect r);
// Disposal::Background: the frame's area becomes transparent.
void ClearRect(uint32_t* canvas, Size canvasSize, FrameRect r);

// GIF delays are in 1/100 s; like browsers, 0 or 1 means 100 ms.
uint32_t FrameDelayMs(uint32_t centiseconds);

}  // namespace fiv::core
