#include "Shell.h"

#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "../core/Pixels.h"
#include "FileStream.h"

namespace fiv {

namespace {

HGLOBAL GlobalCopy(const void* data, size_t bytes) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return nullptr;
    void* p = GlobalLock(h);
    std::memcpy(p, data, bytes);
    GlobalUnlock(h);
    return h;
}

}  // namespace

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    HGLOBAL h = GlobalCopy(text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    if (!h) return false;
    if (!OpenClipboard(owner)) {
        GlobalFree(h);
        return false;
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(h);
    return ok;
}

ClipboardImage BuildClipboardImage(IWICImagingFactory* wic, const std::wstring& path, int orientation,
                                   int quarterTurns, uint64_t maxBytes) {
    ClipboardImage out;
    if (!wic) return out;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    HRESULT hr = FileStream::Open(path, &stream);
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr))
        hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeMedianCut);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = conv->GetSize(&w, &h);
    if (FAILED(hr) || !w || !h) {
        out.hr = FAILED(hr) ? hr : WINCODEC_ERR_BADIMAGE;
        return out;
    }
    ComPtr<IWICBitmapSource> src = conv;
    const uint64_t bytes = uint64_t(w) * h * 4;
    if (bytes > maxBytes) {  // scale down so neither buffer exceeds the limit
        const double f = std::sqrt(double(maxBytes) / double(bytes)) * 0.99;
        const UINT sw = std::max(1u, UINT(w * f)), sh = std::max(1u, UINT(h * f));
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(hr = wic->CreateBitmapScaler(&scaler)) ||
            FAILED(hr = scaler->Initialize(src.Get(), sw, sh, WICBitmapInterpolationModeFant))) {
            out.hr = hr;
            return out;
        }
        src = scaler;
        w = sw;
        h = sh;
        out.scaled = true;
    }
    std::vector<uint32_t> px(size_t(w) * h);
    hr = src->CopyPixels(nullptr, w * 4, UINT(px.size() * 4), reinterpret_cast<BYTE*>(px.data()));
    if (FAILED(hr)) {
        out.hr = hr;
        return out;
    }
    src.Reset();
    conv.Reset();
    frame.Reset();
    dec.Reset();
    stream.Reset();
    // EXIF orientation, then the view rotation (orientations 6, 3, 8 turn clockwise).
    static const int kTurn[4] = {1, 6, 3, 8};
    for (int step : {orientation, kTurn[quarterTurns & 3]}) {
        if (step <= 1 || step > 8) continue;
        std::vector<uint32_t> o(px.size());
        core::ApplyOrientation(px.data(), {w, h}, step, o.data());
        const core::Size s = core::OrientedSize({w, h}, step);
        w = s.w;
        h = s.h;
        px.swap(o);
    }

    BITMAPV5HEADER hdr{};
    hdr.bV5Size = sizeof hdr;
    hdr.bV5Width = LONG(w);
    hdr.bV5Height = LONG(h);  // bottom-up
    hdr.bV5Planes = 1;
    hdr.bV5BitCount = 32;
    hdr.bV5Compression = BI_BITFIELDS;
    hdr.bV5SizeImage = w * h * 4;
    hdr.bV5RedMask = 0x00FF0000;
    hdr.bV5GreenMask = 0x0000FF00;
    hdr.bV5BlueMask = 0x000000FF;
    hdr.bV5AlphaMask = 0xFF000000;
    hdr.bV5CSType = 0x73524742;  // LCS_sRGB ('sRGB'); the macro is a multi-char constant
    hdr.bV5Intent = LCS_GM_IMAGES;
    HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, sizeof hdr + size_t(hdr.bV5SizeImage));
    if (!dib) {
        out.hr = E_OUTOFMEMORY;
        return out;
    }
    auto* p = static_cast<uint8_t*>(GlobalLock(dib));
    std::memcpy(p, &hdr, sizeof hdr);
    for (UINT y = 0; y < h; ++y)
        std::memcpy(p + sizeof hdr + size_t(h - 1 - y) * w * 4, px.data() + size_t(y) * w, size_t(w) * 4);
    GlobalUnlock(dib);

    std::vector<uint8_t> drop(sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t), 0);
    auto* df = reinterpret_cast<DROPFILES*>(drop.data());
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    std::memcpy(drop.data() + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
    out.drop = GlobalCopy(drop.data(), drop.size());
    out.dib = dib;
    out.width = w;
    out.height = h;
    out.hr = S_OK;
    return out;
}

void FreeClipboardImage(ClipboardImage& img) {
    if (img.dib) GlobalFree(img.dib);
    if (img.drop) GlobalFree(img.drop);
    img.dib = img.drop = nullptr;
}

bool PutClipboardImage(HWND owner, ClipboardImage& img) {
    if (!img.dib || !OpenClipboard(owner)) {
        FreeClipboardImage(img);
        return false;
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_DIBV5, img.dib) != nullptr;
    if (ok) img.dib = nullptr;  // owned by the clipboard now
    if (img.drop && SetClipboardData(CF_HDROP, img.drop)) img.drop = nullptr;
    CloseClipboard();
    FreeClipboardImage(img);
    return ok;
}

bool ShowInExplorer(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (!pidl) return false;
    const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
    return SUCCEEDED(hr);
}

bool OpenInEditor(HWND owner, const std::wstring& path, const std::wstring& editor) {
    if (editor.empty()) {
        OPENASINFO info{};
        info.pcszFile = path.c_str();
        info.oaifInFlags = OAIF_EXEC | OAIF_HIDE_REGISTRATION;
        return SUCCEEDED(SHOpenWithDialog(owner, &info));
    }
    const std::wstring args = L"\"" + path + L"\"";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", editor.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}

}  // namespace fiv
