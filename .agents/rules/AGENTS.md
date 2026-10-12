# AI Agent Context & Guidelines

**Target audience:** autonomous AI agents, LLM coding assistants, automated refactoring tools.

## Project overview
**Fast Image Viewer** is a native Windows 10/11 image viewer. Read `docs/ARCHITECTURE.md` before changing the cache.

**Core directives**
* **Do NOT introduce heavy frameworks** (Qt, Electron).
* **Do NOT introduce third-party image decoders** (stb_image, libjpeg, ...). All decoding goes through WIC (`src/platform/WicCodec.cpp`).
* Rendering is **Direct3D 11** with a DirectComposition swap chain, for the Mica backdrop. It falls back to an opaque HWND swap chain. Do not reintroduce OpenGL: the VRAM budget logic depends on DXGI.
* The only third-party library is Dear ImGui (vcpkg, static).

## Memory and disk rules (the point of v3)
1. **Every image allocation is budgeted.**
   - Size work with `core::PlanDecode` and `core::TransientBytes` before allocating.
   - Only start decodes through `core::Scheduler`.
   - Never allocate pixel buffers outside a scheduled job.
2. **No thread per image.** Use `JobSystem::Submit`. Thread 0 is the express lane for the image on screen (priority ≤ 1).
3. **Cancellation must not free accounting early.** A cancelled decode keeps its bytes counted until the worker reports back (scheduler "zombies").
4. **No disk writes** except `settings.ini`, and only through `SaveSettingsNow`, which skips identical content. No logs, thumbnail caches or temp files. `io.IniFilename` stays `nullptr`.
5. **No continuous rendering.** Call `RequestFrames()` when something visible changes.
6. **Open files with `OpenShared`/`FileStream`** (full share flags, read-only). Never block Explorer from moving or deleting a file.
7. **Prefetch runs at background priority** (`BackgroundMode`). Never prefetch cloud placeholder files (`FileEntry::CloudOnly`).
8. Keep `src/core` free of Windows headers, and add unit tests in `tests/core_tests.cpp` for any logic you put there.

## Critical directives and known pitfalls
These traps caused fatal "silent crashes" in earlier versions (see `docs/HISTORY.md`).

### 1. COM smart pointer scope
`ComPtr<T>` must be destroyed **before** `CoUninitialize()`. Either wrap COM work in an explicit `{ }` scope, or declare a `ComApartment` object first in the scope, so it is destroyed last. Worker threads (`JobSystem::Worker`) and `wWinMain` follow this pattern.

### 2. Mutex "garbage memory" bug
Call `SetLastError(0)` before `CreateMutexW`, then check `ERROR_ALREADY_EXISTS`.

### 3. Entry point and Unicode arguments
The app uses `wWinMain` with the WIN32 subsystem. Always parse arguments with `CommandLineToArgvW(GetCommandLineW(), &argc)`. Never use `/ENTRY` linker hacks.

### 4. No throwing filesystem calls
Use Win32 APIs (`FindFirstFileExW`, `GetFileAttributesExW`, `MoveFileExW`) or `std::error_code` overloads. Hidden, protected or cloud files must never be able to throw.

### 5. Headers
`<windows.h>` must come before `<shellapi.h>` and other shell headers. Include `platform/Win.h`, or a header that includes it, first. `Yield` is a Windows macro; don't use it as an identifier.

### 6. Debugging silent launch failures
The build is fully static, so missing DLLs are no longer a cause. If the app still exits silently, temporarily comment out `SetErrorMode(...)` in `src/main.cpp` so Windows can show its error dialogs.
