#pragma once
// One place for Windows headers so every file sees the same macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <objbase.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace fiv {

using Microsoft::WRL::ComPtr;

std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);
std::wstring ToLower(std::wstring s);

std::string FormatBytes(uint64_t bytes);
// Human readable reason for a failed file/codec operation.
std::string DescribeError(HRESULT hr);

uint64_t NowMs();

// COM for the calling thread. Declare it before any ComPtr in the same scope
// so it is destroyed last (see AGENTS.md: ComPtr must die before CoUninitialize).
class ComApartment {
public:
    explicit ComApartment(DWORD model) : ok_(SUCCEEDED(CoInitializeEx(nullptr, model))) {}
    ~ComApartment() {
        if (ok_) CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    bool ok() const { return ok_; }

private:
    bool ok_;
};

}  // namespace fiv
