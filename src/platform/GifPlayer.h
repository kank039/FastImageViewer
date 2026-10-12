#pragma once
// Animated GIF playback. The file is read into RAM once; each Next() call
// (on a worker, never two at a time) decodes one frame and composites it
// onto the canvas. Memory: the file, the canvas, one saved canvas for
// "restore previous" frames and one frame buffer - all known up front, so
// the caller can reserve them from the budget before starting.
#include <memory>
#include <string>
#include <vector>

#include "../core/Animation.h"
#include "Win.h"

struct IWICImagingFactory;

namespace fiv {

class GifAnimator {
public:
    // Reads the header: frame count and canvas size. Fails for single frames.
    HRESULT Open(IWICImagingFactory* wic, const std::wstring& path, uint64_t maxFileBytes);
    // RAM needed once playing (file + 2 canvases + the largest possible frame).
    uint64_t WorkingBytes() const;
    core::Size Canvas() const { return canvas_; }
    uint32_t Frames() const { return frames_; }
    // Composites the next frame (wrapping to the first). delayMs: how long it shows.
    HRESULT Next(IWICImagingFactory* wic, uint32_t& delayMs, uint32_t& index);
    const uint32_t* Pixels() const { return pixels_.data(); }

private:
    std::vector<uint8_t> file_;
    core::Size canvas_;
    uint32_t frames_ = 0, next_ = 0;
    std::vector<uint32_t> pixels_, saved_, frame_;
    core::Disposal lastDisposal_ = core::Disposal::None;
    core::FrameRect lastRect_;
};

}  // namespace fiv
