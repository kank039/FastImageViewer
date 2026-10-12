#include "WicCodec.h"

#include <propidl.h>

#include "WicCompat.h"

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

// Fallback for codecs/WIC builds that do not expose EXIF through the query
// reader: parse the JPEG header ourselves (first 256 KB only).
int ReadJpegHeaderOrientation(const std::wstring& path) {
    HANDLE h = OpenShared(path, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    std::vector<uint8_t> head(256 * 1024);
    DWORD got = 0;
    const BOOL ok = ReadFile(h, head.data(), DWORD(head.size()), &got, nullptr);
    CloseHandle(h);
    return ok ? core::JpegExifOrientation(head.data(), got) : 0;
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

}  // namespace

ProbeResult ProbeImage(WorkerContext& ctx, ID3D11Device* thumbDevice, const std::wstring& path,
                       bool honorExifOrientation) {
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
    if (honorExifOrientation) {
        r.orientation = ReadOrientation(frame.Get());
        GUID container{};
        if (r.orientation == 0 && SUCCEEDED(dec->GetContainerFormat(&container)) && container == GUID_ContainerFormatJpeg)
            r.orientation = ReadJpegHeaderOrientation(path);
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

    BackgroundMode bg(req.background);
    const Size want = core::OrientedSize(req.plan.size, req.orientation);  // size before rotation

    std::optional<IoGateLock> io;
    io.emplace(gate, req.priority, cancel);
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
            if (aborted()) {
                CloseHandle(h);
                return fail(E_ABORT);
            }
            io->YieldTo(req.priority, cancel);
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
        if (aborted()) return fail(E_ABORT);
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
