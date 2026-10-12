#pragma once
// Hardware facts, live memory state, storage type, settings file and file operations.
#include <string>

#include "../core/MemoryLimits.h"
#include "Gpu.h"
#include "Win.h"

namespace fiv {

core::MemoryFacts GatherMemoryFacts(const Gpu& gpu);
core::LiveMemory QueryLiveMemory(const Gpu& gpu, HANDLE lowMemoryNotification);

struct ProcessMemory {
    uint64_t workingSet = 0;  // resident in RAM
    uint64_t privateBytes = 0;
};
ProcessMemory QueryProcessMemory();

enum class StorageKind { Ssd, Hdd, Network, Removable, Unknown };
StorageKind DetectStorage(const std::wstring& folder);
const char* StorageName(StorageKind k);
int ReadersFor(StorageKind k);  // concurrent disk reads that will not make the device seek back and forth

int LogicalCores();

std::wstring SettingsFilePath();
bool ReadTextFile(const std::wstring& path, std::string& out);
bool WriteTextFileAtomic(const std::wstring& path, const std::string& text);

// Recycle Bin. If the file cannot be recycled (network share, bin disabled)
// Windows asks before deleting permanently.
bool RecycleFile(HWND owner, const std::wstring& path);
// Moves into <folder>\<sub>\ (created if needed). Never overwrites. Returns a Win32 error code.
DWORD MoveIntoSubfolder(const std::wstring& path, const std::wstring& sub);

std::wstring ParentFolder(const std::wstring& path);
bool IsDirectory(const std::wstring& path);
std::wstring FullPath(const std::wstring& path);

}  // namespace fiv
