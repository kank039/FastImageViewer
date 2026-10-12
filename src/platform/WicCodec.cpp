#include "WicCodec.h"

#include "../core/Organize.h"

#include <propidl.h>

#include "WicCompat.h"

#include <cmath>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <new>
#include <optional>
#include <vector>

#include "../core/Pixels.h"
#include "FileStream.h"

namespace fiv {

using core::Size;

namespace {

constexpr UINT kBandRows = 256;
constexpr DWORD kReadChunk = 2 * 1024 * 1024;

// 0 when WIC exposes no orientation for this frame.
int ReadOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(&q))) return 0;
    const wchar_t* names[] = {L"System.Photo.Orientation", L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"};
    for (const wchar_t* n : names) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(q->GetMetadataByName(n, &v))) {
            int o = v.vt == VT_UI2 ? int(v.uiVal) : v.vt == VT_UI4 ? int(v.ulVal) : 0;
            PropVariantClear(&v);
            if (o >= 1 && o <= 8) return o;
        }
    }
    return 0;
}

// First 256 KB of a JPEG (already in the OS cache after WIC read the header):
// used for the progressive check and as an EXIF fallback for WIC builds that do
// not expose orientation through the query reader.
std::vector<uint8_t> ReadHead(const std::wstring& path) {
    std::vector<uint8_t> head(256 * 1024);
    HANDLE h = OpenShared(path, 0);
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE || !ReadFile(h, head.data(), DWORD(head.size()), &got, nullptr)) got = 0;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    head.resize(got);
    return head;
}

UINT BitsPerPixel(IWICImagingFactory* wic, REFWICPixelFormatGUID fmt) {
    ComPtr<IWICComponentInfo> info;
    ComPtr<IWICPixelFormatInfo> pf;
    UINT bpp = 0;
    if (SUCCEEDED(wic->CreateComponentInfo(fmt, &info)) && SUCCEEDED(info.As(&pf))) pf->GetBitsPerPixel(&bpp);
    return bpp;
}

std::unique_ptr<uint8_t[]> Alloc(uint64_t bytes) {
    if (bytes > SIZE_MAX) return nullptr;
    return std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[size_t(bytes)]);
}

HRESULT CreateTexture(ID3D11Device* device, const uint8_t* chain, Size size, uint32_t levels,
                      ComPtr<ID3D11ShaderResourceView>& out) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = size.w;
    d.Height = size.h;
    d.MipLevels = levels;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> init(levels);
    const uint8_t* p = chain;
    for (uint32_t i = 0; i < levels; ++i) {
        Size l = core::MipLevelSize(size, i);
        init[i].pSysMem = p;
        init[i].SysMemPitch = l.w * 4;
        p += size_t(l.w) * l.h * 4;
    }
    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = device->CreateTexture2D(&d, init.data(), &tex);
    if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(tex.Get(), nullptr, &out);
    return hr;
}

void MakeThumbnail(IWICImagingFactory* wic, ID3D11Device* device, IWICBitmapFrameDecode* frame, int orientation,
                   ProbeResult& r) {
    ComPtr<IWICBitmapSource> thumb;
    if (FAILED(frame->GetThumbnail(&thumb))) return;
    UINT w = 0, h = 0;
    thumb->GetSize(&w, &h);
    if (w == 0 || h == 0 || w > 1024 || h > 1024) return;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(thumb.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeMedianCut)))
        return;
    std::vector<uint32_t> px(size_t(w) * h), oriented;
    if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(px.size() * 4), reinterpret_cast<BYTE*>(px.data())))) return;
    Size s{w, h};
    const uint32_t* data = px.data();
    if (orientation != 1) {
        oriented.resize(px.size());
        core::ApplyOrientation(px.data(), s, orientation, oriented.data());
        s = core::OrientedSize(s, orientation);
        data = oriented.data();
    }
    if (SUCCEEDED(CreateTexture(device, reinterpret_cast<const uint8_t*>(data), s, 1, r.thumb))) r.thumbSize = s;
}


// ---- Colour management -------------------------------------------------------

ComPtr<IWICColorContext> ExifColorSpace(IWICImagingFactory* wic, UINT value) {
    ComPtr<IWICColorContext> c;
    if (FAILED(wic->CreateColorContext(&c)) || FAILED(c->InitializeFromExifColorSpace(value))) return nullptr;
    return c;
}

// The frame's own profile; null when it has none or says sRGB (nothing to convert).
ComPtr<IWICColorContext> EmbeddedProfile(IWICImagingFactory* wic, IWICBitmapFrameDecode* frame) {
    UINT count = 0;
    if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0 || count > 8) return nullptr;
    std::vector<ComPtr<IWICColorContext>> owned(count);
    std::vector<IWICColorContext*> raw(count);
    for (UINT i = 0; i < count; ++i) {
        if (FAILED(wic->CreateColorContext(&owned[i]))) return nullptr;
        raw[i] = owned[i].Get();
    }
    if (FAILED(frame->GetColorContexts(count, raw.data(), &count))) return nullptr;
    for (UINT i = 0; i < count; ++i) {
        WICColorContextType type{};
        if (FAILED(owned[i]->GetType(&type))) continue;
        if (type == WICColorContextProfile) return owned[i];
        UINT exif = 0;
        if (type == WICColorContextExifColorSpace && SUCCEEDED(owned[i]->GetExifColorSpace(&exif)) && exif == 2)
            return owned[i];  // Adobe RGB (1 = sRGB needs nothing)
    }
    return nullptr;
}

ComPtr<IWICColorContext> DestinationProfile(WorkerContext& ctx, const DecodeRequest& req) {
    const std::wstring key = req.colorMode == 2 ? req.displayProfile : L"";
    if (ctx.colorDst && ctx.colorDstKey == key) {
        ComPtr<IWICColorContext> c;
        ctx.colorDst.As(&c);
        return c;
    }
    ComPtr<IWICColorContext> c;
    if (!key.empty() && SUCCEEDED(ctx.wic->CreateColorContext(&c)) && FAILED(c->InitializeFromFilename(key.c_str())))
        c.Reset();
    if (!c) c = ExifColorSpace(ctx.wic, 1);  // sRGB
    ctx.colorDst = c;
    ctx.colorDstKey = key;
    return c;
}

// ---- HDR ------------------------------------------------------------------------

bool IsHdrFormat(IWICImagingFactory* wic, REFWICPixelFormatGUID fmt) {
    ComPtr<IWICComponentInfo> info;
    ComPtr<IWICPixelFormatInfo2> pf;
    WICPixelFormatNumericRepresentation rep{};
    if (FAILED(wic->CreateComponentInfo(fmt, &info)) || FAILED(info.As(&pf)) || FAILED(pf->GetNumericRepresentation(&rep)))
        return false;
    return rep == WICPixelFormatNumericRepresentationFloat || rep == WICPixelFormatNumericRepresentationFixed;
}

// Linear scRGB (1.0 = SDR white) to sRGB-encoded BGRA8. Everything up to 80%
// of SDR white is kept as is; brighter values roll off smoothly towards white,
// so HDR highlights keep detail instead of clipping.
class ToneMapSource final : public IWICBitmapSource {
public:
    explicit ToneMapSource(ComPtr<IWICBitmapSource> floatSource) : src_(std::move(floatSource)) {
        for (int i = 0; i < kLut; ++i) {
            const float c = float(i) / float(kLut - 1);
            lut_[i] = uint8_t(std::lround(255.0f * (c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f)));
        }
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IWICBitmapSource)) {
            *ppv = static_cast<IWICBitmapSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE GetSize(UINT* w, UINT* h) override { return src_->GetSize(w, h); }
    HRESULT STDMETHODCALLTYPE GetPixelFormat(WICPixelFormatGUID* f) override {
        if (!f) return E_INVALIDARG;
        *f = GUID_WICPixelFormat32bppBGRA;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetResolution(double* x, double* y) override { return src_->GetResolution(x, y); }
    HRESULT STDMETHODCALLTYPE CopyPalette(IWICPalette*) override { return WINCODEC_ERR_PALETTEUNAVAILABLE; }
    HRESULT STDMETHODCALLTYPE CopyPixels(const WICRect* rc, UINT stride, UINT size, BYTE* out) override {
        UINT w = 0, h = 0;
        HRESULT hr = src_->GetSize(&w, &h);
        if (FAILED(hr)) return hr;
        const WICRect r = rc ? *rc : WICRect{0, 0, INT(w), INT(h)};
        if (r.Width <= 0 || r.Height <= 0) return S_OK;
        if (uint64_t(stride) * (r.Height - 1) + uint64_t(r.Width) * 4 > size) return E_INVALIDARG;
        constexpr INT kChunk = 16;  // rows per request: a small float buffer, macroblock aligned for JPEG XR
        std::vector<float> buf(size_t(r.Width) * 4 * kChunk);
        for (INT y = 0; y < r.Height; y += kChunk) {
            const INT rows = std::min(kChunk, r.Height - y);
            const WICRect part{r.X, r.Y + y, r.Width, rows};
            const UINT fstride = UINT(r.Width) * 16;
            hr = src_->CopyPixels(&part, fstride, fstride * UINT(rows), reinterpret_cast<BYTE*>(buf.data()));
            if (FAILED(hr)) return hr;
            for (INT row = 0; row < rows; ++row) {
                const float* in = buf.data() + size_t(row) * r.Width * 4;
                BYTE* o = out + size_t(y + row) * stride;
                for (INT x = 0; x < r.Width; ++x, in += 4, o += 4) {
                    float rgb[3] = {std::max(0.0f, in[0]), std::max(0.0f, in[1]), std::max(0.0f, in[2])};
                    const float l = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
                    constexpr float k = 0.8f;
                    if (l > k) {
                        const float mapped = k + (1 - k) * (1 - std::exp(-(l - k) / (1 - k)));
                        for (float& c : rgb) c *= mapped / l;
                    }
                    for (int c = 0; c < 3; ++c) o[2 - c] = lut_[std::min(kLut - 1, int(std::min(rgb[c], 1.0f) * (kLut - 1) + 0.5f))];
                    o[3] = uint8_t(std::clamp(in[3], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
        }
        return S_OK;
    }

private:
    static constexpr int kLut = 4096;
    ComPtr<IWICBitmapSource> src_;
    std::atomic<ULONG> refs_{1};
    uint8_t lut_[kLut];
};

}  // namespace

// WIC photo-metadata policies: covers HEIC, WebP, AVIF, PNG eXIf and more.
void ReadPolicyInfo(IWICBitmapFrameDecode* frame, core::PhotoInfo& p) {
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(&q))) return;
    auto get = [&](const wchar_t* name, PROPVARIANT& v) {
        PropVariantInit(&v);
        return SUCCEEDED(q->GetMetadataByName(name, &v));
    };
    auto str = [&](const wchar_t* name) {
        PROPVARIANT v;
        std::string s;
        if (get(name, v)) {
            if (v.vt == VT_LPWSTR && v.pwszVal) s = ToUtf8(v.pwszVal);
            else if (v.vt == VT_LPSTR && v.pszVal) s = v.pszVal;
            PropVariantClear(&v);
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
        return s;
    };
    auto num = [&](const wchar_t* name) {
        PROPVARIANT v;
        double d = 0;
        if (get(name, v)) {
            switch (v.vt) {
                case VT_R8: d = v.dblVal; break;
                case VT_R4: d = v.fltVal; break;
                case VT_UI1: d = v.bVal; break;
                case VT_UI2: d = v.uiVal; break;
                case VT_UI4: d = v.ulVal; break;
                case VT_I4: d = v.lVal; break;
                default: break;
            }
            PropVariantClear(&v);
        }
        return d;
    };
    if (p.make.empty()) p.make = str(L"System.Photo.CameraManufacturer");
    if (p.model.empty()) p.model = str(L"System.Photo.CameraModel");
    if (p.lens.empty()) p.lens = str(L"System.Photo.LensModel");
    if (p.dateTaken.empty()) {
        PROPVARIANT v;
        if (get(L"System.Photo.DateTaken", v)) {
            SYSTEMTIME st{};
            if (v.vt == VT_FILETIME && FileTimeToSystemTime(&v.filetime, &st)) {
                char b[32];
                std::snprintf(b, sizeof b, "%04u:%02u:%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
                              st.wMinute, st.wSecond);
                p.dateTaken = b;
            }
            PropVariantClear(&v);
        }
    }
    if (p.exposureDen == 0) {
        const double t = num(L"System.Photo.ExposureTime");
        if (t > 0 && t < 1) {
            p.exposureNum = 1;
            p.exposureDen = uint32_t(std::lround(1.0 / t));
        } else if (t >= 1) {
            p.exposureNum = uint32_t(std::lround(t * 10));
            p.exposureDen = 10;
        }
    }
    if (p.fNumber == 0) p.fNumber = num(L"System.Photo.FNumber");
    if (p.iso == 0) p.iso = uint32_t(num(L"System.Photo.ISOSpeed"));
    if (p.focalLength == 0) p.focalLength = num(L"System.Photo.FocalLength");
    if (p.focal35 == 0) p.focal35 = uint32_t(num(L"System.Photo.FocalLengthInFilm"));
    if (!p.hasBias) {
        PROPVARIANT v;
        if (get(L"System.Photo.ExposureBias", v)) {
            if (v.vt == VT_R8) {
                p.exposureBias = v.dblVal;
                p.hasBias = true;
            }
            PropVariantClear(&v);
        }
    }
}

ProbeResult ProbeImage(WorkerContext& ctx, ID3D11Device* thumbDevice, const std::wstring& path,
                       bool honorExifOrientation, bool headExif) {
    ProbeResult r;
    if (!ctx.wic) return r;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    r.hr = FileStream::Open(path, &stream);
    if (SUCCEEDED(r.hr)) r.hr = ctx.wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(r.hr)) r.hr = dec->GetFrame(0, &frame);
    UINT w = 0, h = 0;
    if (SUCCEEDED(r.hr)) r.hr = frame->GetSize(&w, &h);
    if (FAILED(r.hr)) return r;
    if (w == 0 || h == 0) {
        r.hr = WINCODEC_ERR_BADIMAGE;
        return r;
    }
    r.stored = {w, h};
    GUID container{};
    const bool jpeg = SUCCEEDED(dec->GetContainerFormat(&container)) && container == GUID_ContainerFormatJpeg;
    std::vector<uint8_t> head;
    if (jpeg || headExif) head = ReadHead(path);
    if (headExif) core::ParseExifHead(head.data(), head.size(), r.photo);
    if (r.photo.Empty() || r.photo.lens.empty()) ReadPolicyInfo(frame.Get(), r.photo);
    if (jpeg) {
        // Unknown counts as progressive: over-reserving is safe, under-reserving is not.
        r.progressive = core::JpegIsProgressive(head.data(), head.size()) != 0;
    }
    if (honorExifOrientation) {
        r.orientation = ReadOrientation(frame.Get());
        if (r.orientation == 0 && jpeg) r.orientation = core::JpegExifOrientation(head.data(), head.size());
    }
    if (r.orientation < 1 || r.orientation > 8) r.orientation = 1;

    ComPtr<IWICBitmapSourceTransform> st;
    if (SUCCEEDED(frame.As(&st))) {
        UINT cw = w / 2 ? w / 2 : 1, ch = h / 2 ? h / 2 : 1;
        r.canPrescale = SUCCEEDED(st->GetClosestSize(&cw, &ch)) && (cw < w || ch < h);
    }
    if (thumbDevice) MakeThumbnail(ctx.wic, thumbDevice, frame.Get(), r.orientation, r);
    return r;
}

DecodeResult DecodeImage(WorkerContext& ctx, ID3D11Device* device, IoGate& gate, const DecodeRequest& req,
                         const CancelFlag& cancel) {
    DecodeResult res;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    auto aborted = [&] { return cancel.load(std::memory_order_relaxed); };
    auto fail = [&](HRESULT hr, bool retry = false) {
        res.hr = hr;
        res.retry = retry;
        return res;
    };
    if (aborted()) return fail(E_ABORT);
    if (!ctx.wic || !req.plan.valid()) return fail(E_UNEXPECTED);

    BackgroundMode bg(req.Priority() > 1);
    // Re-evaluated at every checkpoint so a prefetch that becomes the current
    // image immediately leaves background mode and stops yielding the disk.
    auto checkpoint = [&] {
        bg.Set(req.Priority() > 1);
        return !aborted();
    };
    const Size want = core::OrientedSize(req.plan.size, req.orientation);  // size before rotation

    std::optional<IoGateLock> io;
    io.emplace(gate, req.Priority(), cancel);
    if (!io->held()) return fail(E_ABORT);

    // 1. Input: whole file into RAM in one sequential pass (friendliest for
    //    disks), or a shared-read stream for very large files.
    std::unique_ptr<uint8_t[]> fileBuf;
    ComPtr<IStream> stream;
    if (req.readIntoMemory) {
        HANDLE h = OpenShared(req.path, FILE_FLAG_SEQUENTIAL_SCAN);
        if (h == INVALID_HANDLE_VALUE) return fail(HRESULT_FROM_WIN32(GetLastError()));
        LARGE_INTEGER size{};
        GetFileSizeEx(h, &size);
        if (uint64_t(size.QuadPart) != req.fileBytes || size.QuadPart == 0) {
            CloseHandle(h);
            return fail(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), true);  // changed since it was listed
        }
        fileBuf = Alloc(req.fileBytes);
        if (!fileBuf) {
            CloseHandle(h);
            return fail(E_OUTOFMEMORY, true);
        }
        uint64_t done = 0;
        while (done < req.fileBytes) {
            DWORD want32 = DWORD(std::min<uint64_t>(kReadChunk, req.fileBytes - done)), got = 0;
            if (!ReadFile(h, fileBuf.get() + done, want32, &got, nullptr) || got == 0) {
                HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
                CloseHandle(h);
                return fail(FAILED(hr) ? hr : WINCODEC_ERR_STREAMREAD);
            }
            done += got;
            if (!checkpoint()) {
                CloseHandle(h);
                return fail(E_ABORT);
            }
            io->YieldTo(req.Priority(), cancel);
            if (!io->held()) {
                CloseHandle(h);
                return fail(E_ABORT);
            }
        }
        CloseHandle(h);
        io.reset();  // decoding needs no disk: let the next reader in

        ComPtr<IWICStream> ws;
        HRESULT hr = ctx.wic->CreateStream(&ws);
        if (SUCCEEDED(hr)) hr = ws->InitializeFromMemory(fileBuf.get(), DWORD(req.fileBytes));
        if (FAILED(hr)) return fail(hr);
        stream = ws;
    } else {
        HRESULT hr = FileStream::Open(req.path, &stream);
        if (FAILED(hr)) return fail(hr);
    }

    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = ctx.wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    UINT sw = 0, sh = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&sw, &sh);
    if (FAILED(hr)) return fail(hr);
    if (sw != req.stored.w || sh != req.stored.h) return fail(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), true);

    ComPtr<IWICBitmapSource> src = frame;

    // 2. Let the codec decode at 1/2, 1/4 or 1/8 scale when it can (JPEG):
    //    several times faster and much less memory than a full decode.
    ComPtr<IWICBitmap> pre;
    if (req.canPrescale) {
        const uint32_t shift = core::PrescaleShift(req.stored, want);
        ComPtr<IWICBitmapSourceTransform> st;
        if (shift > 0 && SUCCEEDED(frame.As(&st))) {
            const Size target = core::PrescaledSize(req.stored, shift);
            UINT cw = target.w, ch = target.h;
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            if (SUCCEEDED(st->GetClosestSize(&cw, &ch)) && cw >= want.w && ch >= want.h && cw < sw &&
                uint64_t(cw) * ch <= uint64_t(target.w) * target.h && SUCCEEDED(st->GetClosestPixelFormat(&fmt))) {
                const UINT bpp = BitsPerPixel(ctx.wic, fmt);
                if (bpp >= 8 && bpp <= 32 && SUCCEEDED(ctx.wic->CreateBitmap(cw, ch, fmt, WICBitmapCacheOnLoad, &pre))) {
                    ComPtr<IWICBitmapLock> lock;
                    WICRect all{0, 0, INT(cw), INT(ch)};
                    UINT stride = 0, bytes = 0;
                    BYTE* ptr = nullptr;
                    HRESULT phr = pre->Lock(&all, WICBitmapLockWrite, &lock);
                    if (SUCCEEDED(phr)) phr = lock->GetStride(&stride);
                    if (SUCCEEDED(phr)) phr = lock->GetDataPointer(&bytes, &ptr);
                    if (SUCCEEDED(phr))
                        phr = st->CopyPixels(nullptr, cw, ch, &fmt, WICBitmapTransformRotate0, stride, bytes, ptr);
                    lock.Reset();
                    if (SUCCEEDED(phr))
                        src = pre;
                    else
                        pre.Reset();
                }
            }
        }
    }
    if (aborted()) return fail(E_ABORT);

    // 3a. HDR sources (floating or fixed point) are tone-mapped to SDR while
    //     streaming, in 16-row chunks.
    WICPixelFormatGUID srcFormat{};
    if (!pre && SUCCEEDED(frame->GetPixelFormat(&srcFormat)) && IsHdrFormat(ctx.wic, srcFormat)) {
        ComPtr<IWICFormatConverter> toFloat;
        if (SUCCEEDED(ctx.wic->CreateFormatConverter(&toFloat)) &&
            SUCCEEDED(toFloat->Initialize(frame.Get(), GUID_WICPixelFormat128bppRGBAFloat, WICBitmapDitherTypeNone,
                                          nullptr, 0, WICBitmapPaletteTypeCustom))) {
            ComPtr<IWICBitmapSource> mapped;
            mapped.Attach(new (std::nothrow) ToneMapSource(toFloat));
            if (mapped) {
                src = mapped;
                res.hdr = true;
            }
        }
    }

    // 3. Convert to BGRA and scale to exactly the planned size.
    ComPtr<IWICFormatConverter> conv;
    hr = ctx.wic->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr))
        hr = conv->Initialize(src.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) return fail(hr);
    src = conv;

    UINT cw = 0, ch = 0;
    src->GetSize(&cw, &ch);
    if (cw != want.w || ch != want.h) {
        ComPtr<IWICBitmapScaler> scaler;
        hr = ctx.wic->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) {
            hr = scaler->Initialize(src.Get(), want.w, want.h, kWicHighQualityCubic);
            if (FAILED(hr)) hr = scaler->Initialize(src.Get(), want.w, want.h, WICBitmapInterpolationModeFant);
        }
        if (FAILED(hr)) return fail(hr);
        src = scaler;
    }

    // 3b. Colour management, on the scaled pixels (cheapest): the embedded
    //     profile (or sRGB) to sRGB or to the display's profile.
    if (req.colorMode != 0 && !res.hdr) {
        ComPtr<IWICColorContext> from = EmbeddedProfile(ctx.wic, frame.Get());
        const bool toDisplay = req.colorMode == 2 && !req.displayProfile.empty();
        if (!from && toDisplay) from = ExifColorSpace(ctx.wic, 1);
        ComPtr<IWICColorContext> to = from ? DestinationProfile(ctx, req) : nullptr;
        ComPtr<IWICColorTransform> xf;
        if (from && to && SUCCEEDED(ctx.wic->CreateColorTransformer(&xf)) &&
            SUCCEEDED(xf->Initialize(src.Get(), from.Get(), to.Get(), GUID_WICPixelFormat32bppBGRA))) {
            src = xf;  // if anything fails the pixels are shown as stored
            res.colorConverted = true;
        }
    }

    // 4. Pull pixels in bands so a cancelled job stops within milliseconds.
    std::unique_ptr<uint8_t[]> chain = Alloc(req.plan.gpuBytes);
    if (!chain) return fail(E_OUTOFMEMORY, true);
    std::unique_ptr<uint8_t[]> scratch;
    uint8_t* dst = chain.get();
    if (req.orientation != 1) {
        scratch = Alloc(uint64_t(want.w) * want.h * 4);
        if (!scratch) return fail(E_OUTOFMEMORY, true);
        dst = scratch.get();
    }
    const UINT stride = want.w * 4;
    for (UINT y = 0; y < want.h; y += kBandRows) {
        const UINT rows = std::min(kBandRows, want.h - y);
        WICRect r{0, INT(y), INT(want.w), INT(rows)};
        hr = src->CopyPixels(&r, stride, stride * rows, dst + size_t(y) * stride);
        if (FAILED(hr)) return fail(hr);
        if (!checkpoint()) return fail(E_ABORT);
    }
    io.reset();
    src.Reset();
    conv.Reset();
    pre.Reset();
    frame.Reset();
    dec.Reset();
    stream.Reset();
    fileBuf.reset();

    if (req.orientation != 1) {
        core::ApplyOrientation(reinterpret_cast<const uint32_t*>(scratch.get()), want, req.orientation,
                               reinterpret_cast<uint32_t*>(chain.get()));
        scratch.reset();
    }
    core::GenerateMips(chain.get(), req.plan.size, req.plan.mipLevels);
    {  // Histogram from a <=512 px mip level: ~4 KB, no extra decoding.
        const uint32_t level = core::HistogramLevel(req.plan.size, req.plan.mipLevels);
        auto h = std::make_shared<core::Histogram>();
        core::ComputeHistogram(chain.get() + core::MipChainBytes(req.plan.size, level),
                               core::MipLevelSize(req.plan.size, level), *h);
        res.histogram = std::move(h);
    }
    if (req.wantSharpness) {  // focus score from a <=1024 px level (a few ms)
        const uint32_t level = core::HistogramLevel(req.plan.size, req.plan.mipLevels, 1024);
        const core::Size ls = core::MipLevelSize(req.plan.size, level);
        if (std::max(ls.w, ls.h) <= 2048)
            res.sharpness = core::Sharpness(chain.get() + core::MipChainBytes(req.plan.size, level), ls);
    }
    if (aborted()) return fail(E_ABORT);

    // 5. Immutable texture, created on this worker thread (D3D11 devices are
    //    free-threaded), so the UI thread never stalls on an upload.
    hr = CreateTexture(device, chain.get(), req.plan.size, req.plan.mipLevels, res.srv);
    if (FAILED(hr)) return fail(hr, hr == E_OUTOFMEMORY);

    QueryPerformanceCounter(&t1);
    res.ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
    res.hr = S_OK;
    return res;
}

std::set<std::wstring> InstalledDecoderExtensions() {
    std::set<std::wstring> out;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return out;
    ComPtr<IEnumUnknown> en;
    if (FAILED(wic->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault, &en))) return out;
    for (;;) {
        ComPtr<IUnknown> unk;
        ULONG got = 0;
        if (en->Next(1, &unk, &got) != S_OK) break;
        ComPtr<IWICBitmapDecoderInfo> info;
        if (FAILED(unk.As(&info))) continue;
        UINT n = 0;
        if (FAILED(info->GetFileExtensions(0, nullptr, &n)) || n == 0) continue;
        std::wstring list(n, L'\0');
        if (FAILED(info->GetFileExtensions(n, list.data(), &n))) continue;
        list.resize(wcslen(list.c_str()));
        size_t start = 0;
        while (start <= list.size()) {
            size_t comma = list.find(L',', start);
            std::wstring ext = list.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
            if (!ext.empty()) out.insert(ToLower(ext));
            if (comma == std::wstring::npos) break;
            start = comma + 1;
        }
    }
    return out;
}

}  // namespace fiv
