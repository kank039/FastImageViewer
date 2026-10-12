#include "System.h"

#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2  // K32GetProcessMemoryInfo from kernel32, no psapi.dll
#endif
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winioctl.h>

#include <thread>
#include <vector>

namespace fiv {

core::MemoryFacts GatherMemoryFacts(const Gpu& gpu) {
    core::MemoryFacts f;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) f.totalRam = ms.ullTotalPhys;
    f.dedicatedVram = gpu.Adapter().dedicatedVram;
    f.unifiedMemory = gpu.Adapter().unified;
    uint64_t budget = 0, usage = 0;
    if (gpu.QueryVram(budget, usage)) f.osVramBudget = budget;
    return f;
}

core::LiveMemory QueryLiveMemory(const Gpu& gpu, HANDLE lowMem) {
    core::LiveMemory l;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) {
        l.totalRam = ms.ullTotalPhys;
        l.availRam = ms.ullAvailPhys;
    }
    gpu.QueryVram(l.vramBudget, l.vramUsage);
    BOOL low = FALSE;
    if (lowMem && QueryMemoryResourceNotification(lowMem, &low)) l.lowMemory = low != FALSE;
    return l;
}

ProcessMemory QueryProcessMemory() {
    ProcessMemory p;
    PROCESS_MEMORY_COUNTERS_EX c{};
    c.cb = sizeof c;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof c)) {
        p.workingSet = c.WorkingSetSize;
        p.privateBytes = c.PrivateUsage;
    }
    return p;
}

StorageKind DetectStorage(const std::wstring& folder) {
    if (folder.size() >= 2 && folder[0] == L'\\' && folder[1] == L'\\' && folder.rfind(L"\\\\?\\", 0) != 0)
        return StorageKind::Network;
    wchar_t volume[MAX_PATH] = {};
    if (!GetVolumePathNameW(folder.c_str(), volume, MAX_PATH)) return StorageKind::Unknown;
    switch (GetDriveTypeW(volume)) {
        case DRIVE_REMOTE: return StorageKind::Network;
        case DRIVE_REMOVABLE:
        case DRIVE_CDROM: return StorageKind::Removable;
        default: break;
    }
    wchar_t guid[MAX_PATH] = {};
    if (!GetVolumeNameForVolumeMountPointW(volume, guid, MAX_PATH)) return StorageKind::Unknown;
    std::wstring dev = guid;
    if (!dev.empty() && dev.back() == L'\\') dev.pop_back();
    // Zero access rights: querying a property needs no admin.
    HANDLE h = CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return StorageKind::Unknown;
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR d{};
    DWORD ret = 0;
    const BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &d, sizeof d, &ret, nullptr);
    CloseHandle(h);
    if (!ok || ret < sizeof d) return StorageKind::Unknown;
    return d.IncursSeekPenalty ? StorageKind::Hdd : StorageKind::Ssd;
}

const char* StorageName(StorageKind k) {
    switch (k) {
        case StorageKind::Ssd: return "SSD";
        case StorageKind::Hdd: return "Hard disk";
        case StorageKind::Network: return "Network";
        case StorageKind::Removable: return "Removable";
        default: return "Unknown";
    }
}

int ReadersFor(StorageKind k) { return k == StorageKind::Ssd ? 2 : 1; }

int LogicalCores() {
    const unsigned n = std::thread::hardware_concurrency();
    return n ? int(n) : 2;
}

std::wstring SettingsFilePath() {
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        path = std::wstring(base) + L"\\FastImageViewer\\settings.ini";
    }
    CoTaskMemFree(base);
    return path;
}

bool ReadTextFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < 1024 * 1024;
    if (ok) {
        out.resize(size_t(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}

bool WriteTextFileAtomic(const std::wstring& path, const std::string& text) {
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return false;
    SHCreateDirectoryExW(nullptr, path.substr(0, slash).c_str(), nullptr);
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, text.data(), DWORD(text.size()), &wrote, nullptr) && wrote == text.size();
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

bool RecycleFile(HWND owner, const std::wstring& path) {
    std::wstring from = path;
    from.push_back(L'\0');  // double-null terminated list
    SHFILEOPSTRUCTW op{};
    op.hwnd = owner;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted &&
           GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
}

std::wstring ParentFolder(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    if (slash == 2 && path[1] == L':') return path.substr(0, 3);  // keep "C:\"
    return path.substr(0, slash);
}

DWORD MoveIntoSubfolder(const std::wstring& path, const std::wstring& sub) {
    const std::wstring folder = ParentFolder(path);
    const std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
    const std::wstring dir = folder + (folder.back() == L'\\' ? L"" : L"\\") + sub;
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return GetLastError();
    // No MOVEFILE_REPLACE_EXISTING (never overwrite), no MOVEFILE_COPY_ALLOWED (a rename, never a copy).
    if (!MoveFileExW(path.c_str(), (dir + L"\\" + name).c_str(), 0)) return GetLastError();
    return ERROR_SUCCESS;
}

bool IsDirectory(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring FullPath(const std::wstring& path) {
    const DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!n) return path;
    std::wstring out(n, L'\0');
    const DWORD got = GetFullPathNameW(path.c_str(), n, out.data(), nullptr);
    out.resize(got);
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

}  // namespace fiv
