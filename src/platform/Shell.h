#pragma once
// Clipboard, Explorer and "open in editor" integration.
#include <string>

#include "Win.h"

struct IWICImagingFactory;

namespace fiv {

bool CopyTextToClipboard(HWND owner, const std::wstring& text);

// Built on a worker thread, handed to the clipboard on the UI thread.
struct ClipboardImage {
    HGLOBAL dib = nullptr;   // CF_DIBV5 (32-bit with alpha); Windows synthesizes CF_DIB/CF_BITMAP
    HGLOBAL drop = nullptr;  // CF_HDROP, so pasting into Explorer copies the file
    HRESULT hr = E_FAIL;
    uint32_t width = 0, height = 0;
    bool scaled = false;     // reduced to stay inside the memory limit
};

// Decodes `path` with its EXIF orientation and then `quarterTurns` clockwise
// turns applied. Each of the (at most three) pixel buffers stays under
// maxBytes: larger images are scaled down.
ClipboardImage BuildClipboardImage(IWICImagingFactory* wic, const std::wstring& path, int orientation,
                                   int quarterTurns, uint64_t maxBytes);
// Takes ownership of the handles on success; frees them otherwise.
bool PutClipboardImage(HWND owner, ClipboardImage& img);
void FreeClipboardImage(ClipboardImage& img);

bool ShowInExplorer(const std::wstring& path);
// editor empty: the Windows "Open with" chooser.
bool OpenInEditor(HWND owner, const std::wstring& path, const std::wstring& editor);

}  // namespace fiv
