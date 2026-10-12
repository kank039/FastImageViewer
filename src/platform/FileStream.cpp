#include "FileStream.h"

#include <cstring>

namespace fiv {

HANDLE OpenShared(const std::wstring& path, DWORD extraFlags) {
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | extraFlags, nullptr);
}

HRESULT FileStream::Open(const std::wstring& path, IStream** out) {
    *out = nullptr;
    HANDLE h = OpenShared(path, 0);
    if (h == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    *out = new FileStream(h, path);
    return S_OK;
}

FileStream::~FileStream() {
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
}

HRESULT FileStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ISequentialStream) || riid == __uuidof(IStream)) {
        *ppv = static_cast<IStream*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG FileStream::AddRef() { return ++refs_; }

ULONG FileStream::Release() {
    ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
}

HRESULT FileStream::Read(void* pv, ULONG cb, ULONG* pcbRead) {
    DWORD read = 0;
    if (!ReadFile(h_, pv, cb, &read, nullptr)) {
        if (pcbRead) *pcbRead = 0;
        return HRESULT_FROM_WIN32(GetLastError());
    }
    if (pcbRead) *pcbRead = read;
    return read < cb ? S_FALSE : S_OK;
}

HRESULT FileStream::Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPos) {
    DWORD method = origin == STREAM_SEEK_SET ? FILE_BEGIN : origin == STREAM_SEEK_CUR ? FILE_CURRENT : FILE_END;
    LARGE_INTEGER pos{};
    if (!SetFilePointerEx(h_, move, &pos, method)) return HRESULT_FROM_WIN32(GetLastError());
    if (newPos) newPos->QuadPart = ULONGLONG(pos.QuadPart);
    return S_OK;
}

HRESULT FileStream::Stat(STATSTG* stat, DWORD flags) {
    if (!stat) return STG_E_INVALIDPOINTER;
    std::memset(stat, 0, sizeof *stat);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h_, &info)) return HRESULT_FROM_WIN32(GetLastError());
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = (ULONGLONG(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    stat->mtime = info.ftLastWriteTime;
    stat->ctime = info.ftCreationTime;
    stat->atime = info.ftLastAccessTime;
    stat->grfMode = STGM_READ | STGM_SHARE_DENY_NONE;
    if (!(flags & STATFLAG_NONAME)) {
        const size_t bytes = (path_.size() + 1) * sizeof(wchar_t);
        stat->pwcsName = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
        if (!stat->pwcsName) return E_OUTOFMEMORY;
        std::memcpy(stat->pwcsName, path_.c_str(), bytes);
    }
    return S_OK;
}

}  // namespace fiv
