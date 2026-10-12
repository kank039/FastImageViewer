#include "GifPlayer.h"

#include <wincodec.h>

#include <algorithm>

#include "FileStream.h"

namespace fiv {

namespace {

UINT MetaUInt(IWICMetadataQueryReader* q, const wchar_t* name, UINT fallback) {
    PROPVARIANT v;
    PropVariantInit(&v);
    UINT out = fallback;
    if (q && SUCCEEDED(q->GetMetadataByName(name, &v))) {
        if (v.vt == VT_UI1) out = v.bVal;
        else if (v.vt == VT_UI2) out = v.uiVal;
        else if (v.vt == VT_UI4) out = v.ulVal;
    }
    PropVariantClear(&v);
    return out;
}

HRESULT OpenDecoder(IWICImagingFactory* wic, std::vector<uint8_t>& bytes, ComPtr<IWICBitmapDecoder>& dec) {
    ComPtr<IWICStream> ws;
    HRESULT hr = wic->CreateStream(&ws);
    if (SUCCEEDED(hr)) hr = ws->InitializeFromMemory(bytes.data(), DWORD(bytes.size()));
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromStream(ws.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    return hr;
}

}  // namespace

HRESULT GifAnimator::Open(IWICImagingFactory* wic, const std::wstring& path, uint64_t maxFileBytes) {
    HANDLE h = OpenShared(path, FILE_FLAG_SEQUENTIAL_SCAN);
    if (h == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart <= 0 || uint64_t(size.QuadPart) > maxFileBytes) {
        CloseHandle(h);
        return E_OUTOFMEMORY;
    }
    file_.resize(size_t(size.QuadPart));
    size_t done = 0;
    while (done < file_.size()) {
        DWORD got = 0;
        if (!ReadFile(h, file_.data() + done, DWORD(std::min<size_t>(file_.size() - done, 1 << 20)), &got, nullptr) || !got) {
            CloseHandle(h);
            return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
        }
        done += got;
    }
    CloseHandle(h);

    ComPtr<IWICBitmapDecoder> dec;
    HRESULT hr = OpenDecoder(wic, file_, dec);
    UINT count = 0;
    if (SUCCEEDED(hr)) hr = dec->GetFrameCount(&count);
    if (FAILED(hr)) return hr;
    if (count < 2) return S_FALSE;  // not animated
    ComPtr<IWICMetadataQueryReader> q;
    dec->GetMetadataQueryReader(&q);
    UINT w = MetaUInt(q.Get(), L"/logscrdesc/Width", 0), hgt = MetaUInt(q.Get(), L"/logscrdesc/Height", 0);
    if (!w || !hgt) {
        ComPtr<IWICBitmapFrameDecode> f0;
        if (FAILED(hr = dec->GetFrame(0, &f0)) || FAILED(hr = f0->GetSize(&w, &hgt))) return hr;
    }
    if (!w || !hgt || w > 8192 || hgt > 8192) return E_INVALIDARG;
    canvas_ = {w, hgt};
    frames_ = count;
    return S_OK;
}

uint64_t GifAnimator::WorkingBytes() const {
    return file_.size() + uint64_t(canvas_.w) * canvas_.h * 4 * 3;
}

HRESULT GifAnimator::Next(IWICImagingFactory* wic, uint32_t& delayMs, uint32_t& index) {
    const size_t px = size_t(canvas_.w) * canvas_.h;
    if (pixels_.empty()) {
        pixels_.assign(px, 0);
        frame_.reserve(px);
    }
    // What the previous frame asked for.
    if (lastDisposal_ == core::Disposal::Background) core::ClearRect(pixels_.data(), canvas_, lastRect_);
    if (lastDisposal_ == core::Disposal::Previous && saved_.size() == px) pixels_ = saved_;
    if (next_ == 0) std::fill(pixels_.begin(), pixels_.end(), 0u);  // each loop starts clean

    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    HRESULT hr = OpenDecoder(wic, file_, dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(next_, &frame);
    UINT fw = 0, fh = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&fw, &fh);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr))
        hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;
    if (uint64_t(fw) * fh > px) return E_INVALIDARG;  // a frame larger than the canvas: refuse
    ComPtr<IWICMetadataQueryReader> q;
    frame->GetMetadataQueryReader(&q);
    const core::FrameRect rect{int(MetaUInt(q.Get(), L"/imgdesc/Left", 0)), int(MetaUInt(q.Get(), L"/imgdesc/Top", 0)),
                               int(fw), int(fh)};
    const core::Disposal disposal = core::ToDisposal(int(MetaUInt(q.Get(), L"/grctlext/Disposal", 0)));
    delayMs = core::FrameDelayMs(MetaUInt(q.Get(), L"/grctlext/Delay", 0));

    frame_.resize(size_t(fw) * fh);
    hr = conv->CopyPixels(nullptr, fw * 4, UINT(frame_.size() * 4), reinterpret_cast<BYTE*>(frame_.data()));
    if (FAILED(hr)) return hr;
    if (disposal == core::Disposal::Previous) saved_ = pixels_;
    core::DrawFrame(pixels_.data(), canvas_, frame_.data(), rect);
    lastDisposal_ = disposal;
    lastRect_ = rect;
    index = next_;
    next_ = (next_ + 1) % frames_;
    return S_OK;
}

}  // namespace fiv
