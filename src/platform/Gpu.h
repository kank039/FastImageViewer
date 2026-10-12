#pragma once
// Direct3D 11 device + swap chain.
//
// Why D3D11 and not OpenGL: DXGI tells us exactly how much video memory the OS
// lets this process use (QueryVideoMemoryInfo) and signals when that changes,
// textures can be created from worker threads (no upload stalls on the UI
// thread), and drivers do not keep hidden system-RAM shadow copies.
// A DirectComposition swap chain with premultiplied alpha lets the Windows 11
// Mica backdrop show through; if that is unavailable we fall back to a plain
// opaque swap chain.
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_4.h>

#include <string>

#include "Win.h"

namespace fiv {

struct AdapterInfo {
    std::string name;
    uint64_t dedicatedVram = 0;
    uint64_t sharedMemory = 0;
    bool unified = false;   // integrated GPU / UMA: textures live in system RAM
    bool software = false;  // WARP fallback
};

class Gpu {
public:
    ~Gpu() { Destroy(); }
    bool Create(HWND hwnd, std::string& error);
    void Destroy();

    HRESULT Resize(uint32_t w, uint32_t h);
    void BeginFrame(const float clearColor[4]);
    HRESULT Present();

    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }
    ID3D11SamplerState* PointSampler() const { return pointSampler_.Get(); }
    ID3D11PixelShader* PeakingShader() const { return peakingPS_.Get(); }  // null if it failed to compile
    bool Composited() const { return visual_ != nullptr; }
    uint32_t MaxTextureDim() const { return maxTextureDim_; }
    const AdapterInfo& Adapter() const { return adapter_; }
    HANDLE BudgetEvent() const { return budgetEvent_; }
    bool QueryVram(uint64_t& budget, uint64_t& usage) const;
    bool Alive() const { return swap_ != nullptr; }

    static bool IsDeviceLost(HRESULT hr) {
        return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }

private:
    HRESULT CreateTarget();
    void CreatePeakingShader();

    HWND hwnd_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIAdapter3> adapter3_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11SamplerState> pointSampler_;
    ComPtr<ID3D11PixelShader> peakingPS_;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual> visual_;
    HANDLE budgetEvent_ = nullptr;
    DWORD budgetCookie_ = 0;
    uint32_t width_ = 1, height_ = 1;
    uint32_t maxTextureDim_ = 8192;
    AdapterInfo adapter_;
};

// Dark title bar plus Mica when requested and supported. Returns true when the
// client area is translucent (clear with alpha 0), false when it must be opaque.
bool ApplyBackdrop(HWND hwnd, bool wantMica, bool composited);

}  // namespace fiv
