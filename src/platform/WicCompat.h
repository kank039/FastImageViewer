#pragma once
// MinGW's wincodec.h predates IWICBitmapSourceTransform. MSVC builds use the
// Windows SDK declaration; this block only lets MinGW compile-check the code.
#include <wincodec.h>

#if defined(__MINGW32__) && !defined(__IWICBitmapSourceTransform_INTERFACE_DEFINED__)
#define __IWICBitmapSourceTransform_INTERFACE_DEFINED__
struct IWICBitmapSourceTransform : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CopyPixels(const WICRect* prc, UINT uiWidth, UINT uiHeight,
                                                 WICPixelFormatGUID* pguidDstFormat,
                                                 WICBitmapTransformOptions dstTransform, UINT nStride,
                                                 UINT cbBufferSize, BYTE* pbBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetClosestSize(UINT* puiWidth, UINT* puiHeight) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetClosestPixelFormat(WICPixelFormatGUID* pguidDstFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE DoesSupportTransform(WICBitmapTransformOptions dstTransform,
                                                           BOOL* pfIsSupported) = 0;
};
__CRT_UUID_DECL(IWICBitmapSourceTransform, 0x3b16811b, 0x6a43, 0x4ec9, 0xb7, 0x13, 0x3d, 0x5a, 0x0c, 0x13, 0xb9,
                0x40)
#endif

// WICBitmapInterpolationModeHighQualityCubic (Windows 10+), by value so older headers work.
constexpr WICBitmapInterpolationMode kWicHighQualityCubic = WICBitmapInterpolationMode(4);
