#pragma once
// Read-only IStream over a Win32 file handle.
//
// Opened with FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE so the
// viewer never blocks Explorer from renaming, moving or deleting a file that
// is being decoded. WIC reads only the bytes it needs through this stream
// (headers for probing, everything for large files that are not read into RAM).
#include <objidl.h>

#include <atomic>
#include <string>

#include "Win.h"

namespace fiv {

class FileStream final : public IStream {
public:
    // Returns S_OK and a stream with refcount 1, or a failure HRESULT.
    static HRESULT Open(const std::wstring& path, IStream** out);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    // ISequentialStream
    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override;
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    // IStream
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPos) override;
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    FileStream(HANDLE h, std::wstring path) : h_(h), path_(std::move(path)) {}
    ~FileStream();
    HANDLE h_;
    std::wstring path_;
    std::atomic<ULONG> refs_{1};
};

// Opens a file for reading with full sharing. INVALID_HANDLE_VALUE on failure.
HANDLE OpenShared(const std::wstring& path, DWORD extraFlags);

}  // namespace fiv
