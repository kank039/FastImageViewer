# Getting Started: building Fast Image Viewer

## 1. Prerequisites (Windows 10/11)

- **Visual Studio 2022 or newer** (or Build Tools) with the "Desktop development with C++" workload
- **CMake 3.21+** and **Ninja** (both ship with Visual Studio)
- **vcpkg**, with the `VCPKG_ROOT` environment variable pointing at it

The only third-party dependency is Dear ImGui (Win32 + DX11 backends). vcpkg installs it automatically from `vcpkg.json`.

## 2. Build

Open an **x64 Native Tools Command Prompt** and run:

```
cd path\to\FastImageViewer
cmake --preset release
cmake --build --preset release
```

The result is `build\release\FastImageViewer.exe`.

Everything is linked statically (`x64-windows-static`, `/MT`), so the `.exe` is self-contained: **there are no DLLs to copy**. The v2 "DLL drop" step and its silent-crash failure mode are gone.

Use `--preset debug` for a debug build.

## 3. Run

```
build\release\FastImageViewer.exe "C:\Users\Public\Pictures\Sample Image.jpg"
```

Try a path containing spaces and non-ASCII characters; both are supported. Press **F3** to watch the cache and memory statistics while you browse.

## 4. Tests

The cache logic is platform independent:

```
cmake --preset core-tests
cmake --build --preset core-tests
ctest --preset core-tests
```

This works on Windows, Linux and macOS. CI runs it with AddressSanitizer and UBSan, and also builds the Windows `.exe` with MSVC (`.github/workflows/build.yml`). The built `.exe` is downloadable from each CI run.

## 5. Installer

`installer_script.iss` (Inno Setup) packages the single `.exe` and registers it under "Open with" for every supported image extension. Build the release preset first, then compile the script.
