#include "Win.h"

#include <wincodec.h>

#include <cstdio>

namespace fiv {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::wstring ToLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), DWORD(s.size()));
    return s;
}

std::string FormatBytes(uint64_t b) {
    char buf[32];
    if (b >= 1024ull * 1024 * 1024)
        std::snprintf(buf, sizeof buf, "%.2f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024ull * 1024)
        std::snprintf(buf, sizeof buf, "%.1f MB", b / (1024.0 * 1024));
    else if (b >= 1024)
        std::snprintf(buf, sizeof buf, "%.0f KB", b / 1024.0);
    else
        std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
    return buf;
}

std::string DescribeError(HRESULT hr) {
    switch (hr) {
        case WINCODEC_ERR_COMPONENTNOTFOUND: return "No decoder for this format is installed";
        case WINCODEC_ERR_BADHEADER:
        case WINCODEC_ERR_BADIMAGE:
        case WINCODEC_ERR_BADSTREAMDATA:
        case WINCODEC_ERR_STREAMREAD:
        case WINCODEC_ERR_FRAMEMISSING: return "The file is damaged or is not a valid image";
        case WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT:
        case WINCODEC_ERR_UNSUPPORTEDOPERATION: return "This image variant is not supported by the codec";
        case E_OUTOFMEMORY: return "Not enough memory";
        case E_ABORT: return "Cancelled";
        case HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND):
        case HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND): return "The file no longer exists";
        case HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION):
        case HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION): return "The file is locked by another program";
        case HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED): return "Access denied";
        case HRESULT_FROM_WIN32(ERROR_CLOUD_FILE_PROVIDER_NOT_RUNNING):
        case HRESULT_FROM_WIN32(ERROR_CLOUD_FILE_NETWORK_UNAVAILABLE): return "Cloud file is not available offline";
        default: break;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "Could not open image (0x%08lX)", (unsigned long)hr);
    return buf;
}

uint64_t NowMs() { return GetTickCount64(); }

}  // namespace fiv
