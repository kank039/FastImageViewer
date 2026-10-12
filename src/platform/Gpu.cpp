#include "Gpu.h"

#include <d3dcompiler.h>
#include <dwmapi.h>
#if __has_include(<d3d11_3.h>)
#include <d3d11_3.h>  // D3D11_FEATURE_DATA_D3D11_OPTIONS2 on some SDK versions
#endif

namespace fiv {

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
constexpr DWORD kDwmSystemBackdropType = 38;  // DWMWA_SYSTEMBACKDROP_TYPE (Windows 11 22H2+)
constexpr DWORD kBackdropNone = 1;
constexpr DWORD kBackdropMica = 2;

bool Gpu::Create(HWND hwnd, std::string& error) {
    hwnd_ = hwnd;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 4, D3D11_SDK_VERSION,
                                   &device_, &fl, &context_);
    if (hr == E_INVALIDARG)  // runtimes without 11.1
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels + 1, 3, D3D11_SDK_VERSION,
                               &device_, &fl, &context_);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels + 1, 3, D3D11_SDK_VERSION,
                               &device_, &fl, &context_);
    if (FAILED(hr)) {
        error = "Direct3D 11 is not available on this system.";
        return false;
    }
    maxTextureDim_ = fl >= D3D_FEATURE_LEVEL_11_0 ? 16384u : 8192u;

    ComPtr<IDXGIDevice1> dxgiDevice;
    device_.As(&dxgiDevice);
    if (dxgiDevice) dxgiDevice->SetMaximumFrameLatency(1);

    ComPtr<IDXGIAdapter> adapter;
    if (dxgiDevice) dxgiDevice->GetAdapter(&adapter);
    ComPtr<IDXGIAdapter1> adapter1;
    if (adapter) adapter.As(&adapter1);
    if (adapter1) {
        DXGI_ADAPTER_DESC1 d{};
        adapter1->GetDesc1(&d);
        adapter_.name = ToUtf8(d.Description);
        adapter_.dedicatedVram = d.DedicatedVideoMemory;
        adapter_.sharedMemory = d.SharedSystemMemory;
        adapter_.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    }
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 opt2{};
    const bool uma = SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &opt2, sizeof opt2)) &&
                     opt2.UnifiedMemoryArchitecture;
    adapter_.unified = uma || adapter_.software || adapter_.dedicatedVram < 512ull * 1024 * 1024;

    if (adapter) adapter.As(&adapter3_);
    if (adapter3_) {
        budgetEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (budgetEvent_ && FAILED(adapter3_->RegisterVideoMemoryBudgetChangeNotificationEvent(budgetEvent_,
                                                                                               &budgetCookie_))) {
            CloseHandle(budgetEvent_);
            budgetEvent_ = nullptr;
        }
    }

    ComPtr<IDXGIFactory2> factory;
    if (adapter) adapter->GetParent(IID_PPV_ARGS(&factory));
    if (!factory) {
        error = "DXGI 1.2 is not available on this system.";
        return false;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    width_ = UINT(rc.right - rc.left) ? UINT(rc.right - rc.left) : 1;
    height_ = UINT(rc.bottom - rc.top) ? UINT(rc.bottom - rc.top) : 1;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width_;
    sd.Height = height_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    // Translucent path: composition swap chain hosted by a DirectComposition visual.
    hr = factory->CreateSwapChainForComposition(device_.Get(), &sd, nullptr, &swap_);
    if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcomp_));
    if (SUCCEEDED(hr)) hr = dcomp_->CreateTargetForHwnd(hwnd, TRUE, &target_);
    if (SUCCEEDED(hr)) hr = dcomp_->CreateVisual(&visual_);
    if (SUCCEEDED(hr)) hr = visual_->SetContent(swap_.Get());
    if (SUCCEEDED(hr)) hr = target_->SetRoot(visual_.Get());
    if (SUCCEEDED(hr)) hr = dcomp_->Commit();
    if (FAILED(hr)) {
        visual_.Reset();
        target_.Reset();
        dcomp_.Reset();
        swap_.Reset();
        // Opaque path.
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &sd, nullptr, nullptr, &swap_);
        if (FAILED(hr)) {
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &sd, nullptr, nullptr, &swap_);
        }
        if (FAILED(hr)) {
            error = "Could not create a swap chain for the window.";
            return false;
        }
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&samp, &pointSampler_);
    CreatePeakingShader();

    if (FAILED(CreateTarget())) {
        error = "Could not create the render target.";
        return false;
    }
    return true;
}

HRESULT Gpu::CreateTarget() {
    ComPtr<ID3D11Texture2D> back;
    HRESULT hr = swap_->GetBuffer(0, IID_PPV_ARGS(&back));
    if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_);
    return hr;
}

// Focus peaking: edges with a strong local gradient are painted red over a
// dimmed picture. Same inputs as Dear ImGui's own pixel shader, so it can be
// swapped in for a single draw call; it needs no extra texture memory.
void Gpu::CreatePeakingShader() {
    static const char kSource[] = R"(
struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
sampler sampler0;
Texture2D texture0;
float L(float2 uv) { return dot(texture0.SampleLevel(sampler0, uv, 0).rgb, float3(0.299, 0.587, 0.114)); }
float4 main(PS_INPUT input) : SV_Target {
    float w, h;
    texture0.GetDimensions(w, h);
    float2 t = float2(1.0 / w, 1.0 / h);
    float4 c = texture0.Sample(sampler0, input.uv);
    float gx = L(input.uv + float2(t.x, 0)) - L(input.uv - float2(t.x, 0));
    float gy = L(input.uv + float2(0, t.y)) - L(input.uv - float2(0, t.y));
    float edge = smoothstep(0.16, 0.30, sqrt(gx * gx + gy * gy));
    return float4(lerp(c.rgb * 0.55, float3(1.0, 0.12, 0.08), edge), c.a) * input.col;
})";
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kSource, sizeof kSource - 1, nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &code, &errors)))
        return;  // the feature is simply unavailable
    device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &peakingPS_);
}

void Gpu::Destroy() {
    if (adapter3_ && budgetCookie_) adapter3_->UnregisterVideoMemoryBudgetChangeNotification(budgetCookie_);
    budgetCookie_ = 0;
    if (budgetEvent_) CloseHandle(budgetEvent_);
    budgetEvent_ = nullptr;
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    rtv_.Reset();
    pointSampler_.Reset();
    peakingPS_.Reset();
    visual_.Reset();
    target_.Reset();
    dcomp_.Reset();
    swap_.Reset();
    adapter3_.Reset();
    context_.Reset();
    device_.Reset();
}

HRESULT Gpu::Resize(uint32_t w, uint32_t h) {
    if (!swap_ || w == 0 || h == 0) return S_OK;
    if (w == width_ && h == height_) return S_OK;
    width_ = w;
    height_ = h;
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    HRESULT hr = swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(hr)) hr = CreateTarget();
    return hr;
}

void Gpu::BeginFrame(const float clearColor[4]) {
    ID3D11RenderTargetView* rtv = rtv_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(width_), float(height_), 0, 1};
    context_->RSSetViewports(1, &vp);
    context_->ClearRenderTargetView(rtv, clearColor);
}

HRESULT Gpu::Present() { return swap_ ? swap_->Present(1, 0) : E_FAIL; }

bool Gpu::QueryVram(uint64_t& budget, uint64_t& usage) const {
    if (!adapter3_) return false;
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return false;
    budget = info.Budget;
    usage = info.CurrentUsage;
    return true;
}

bool ApplyBackdrop(HWND hwnd, bool wantMica, bool composited) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    if (wantMica && composited) {
        DWORD type = kBackdropMica;
        if (SUCCEEDED(DwmSetWindowAttribute(hwnd, kDwmSystemBackdropType, &type, sizeof type))) {
            MARGINS m{-1, -1, -1, -1};
            DwmExtendFrameIntoClientArea(hwnd, &m);
            return true;
        }
    }
    DWORD none = kBackdropNone;
    DwmSetWindowAttribute(hwnd, kDwmSystemBackdropType, &none, sizeof none);
    MARGINS m{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(hwnd, &m);
    return false;
}

}  // namespace fiv
