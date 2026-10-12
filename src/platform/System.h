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
// ICC profile file of the monitor the window is on ("" if none is assigned).
std::wstring DisplayProfilePath(HWND hwnd);
bool ReadTextFile(const std::wstring& path, std::string& out);
bool WriteTextFileAtomic(const std::wstring& path, const std::string& text);

std::wstring ParentFolder(const std::wstring& path);
bool IsDirectory(const std::wstring& path);
std::wstring FullPath(const std::wstring& path);

}  // namespace fiv
