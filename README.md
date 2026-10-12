# Fast Image Viewer (v3, "revamped")

A native Windows 10/11 image viewer that shows the next image **instantly**, keeps the last few in memory so going back is instant too, and never trades your machine's health for it:

- **Hard memory ceiling.** Every byte is budgeted *before* it is allocated (the image header is read first, so sizes are exact, not guessed). Budgets are capped by your hardware: a GPU with *n* GB of VRAM can give the viewer at most *n − 1* GB, and RAM always leaves Windows its share.
- **Never spills to the page file or demotes textures.** The cache shrinks automatically when Windows reports memory pressure, when other programs take VRAM (DXGI budget notifications), or when the window is minimized.
- **Gentle on disks.** Each file is read once, sequentially. Hard disks and network shares get one reader at a time (no head thrashing). Prefetch I/O runs at background priority. Nothing is written except a tiny settings file, and only when a setting changes. OneDrive online-only files are not prefetched, because reading them downloads them.
- **Fast.** Images are decoded at your monitor's resolution, not full size (a 24 MP photo on a 1440p screen is ~15 MB instead of 96 MB), JPEGs use the codec's built-in 1/2–1/8 scaling, textures are created on worker threads, and the image on screen has its own express worker so it never waits behind prefetching.
- **Idle means idle.** Rendering is event-driven: no input, no frames.

Tech: C++20, Win32, Direct3D 11 + DirectComposition (Mica), WIC (Windows Imaging Component) for decoding, Dear ImGui for the UI. One self-contained `.exe`.

## Using it

Open an image from Explorer ("Open with"), drag one onto the window, or press **Ctrl+O**. A second launch hands the file to the open window.

| Keys | Action |
|---|---|
| → ↓ Space PgDn / ← ↑ Backspace PgUp | Next / previous |
| Home / End | First / last |
| Mouse wheel, + / − | Zoom at cursor (beyond screen size loads the full-resolution image) |
| Drag | Pan |
| Double-click, Z / 0 | 100 % / fit |
| F, F11, Alt+Enter | Fullscreen |
| Delete | Recycle Bin (Windows asks first if it would be permanent) |
| 1 … 9 | Move into subfolder `1` … `9` (never overwrites) |
| O / B / S / I (F3) / H (F1) | Overlay / sidebar / settings / stats / help |
| F5 | Rescan folder |

The **stats panel (F3)** shows the cache map, texture and decode memory against their limits, the live Windows VRAM budget, free system RAM, process memory and how many files have been read from disk, so you can see the guarantees hold.

## Settings that matter

**Settings → Cache & memory**

- **Prefetch ahead** (default 4) and **Keep behind** (default 5). The window follows your direction of travel.
- **Memory budget:** *Automatic* picks a safe size for your hardware. Manual sliders only go up to what the hardware can handle:
  - VRAM: *n − 1* GB on cards with 2 GB or more, half the card below that, and never above the OS budget.
  - RAM: total minus max(2 GB, 25 %).
  - On integrated GPUs, textures live in system RAM, so a single shared budget is shown.
- **Resolution:** *Fit to screen* (recommended) or *Full resolution*.
- **Release cache when minimized**, and **Don't prefetch online-only files**.

Settings are stored in `%LOCALAPPDATA%\FastImageViewer\settings.ini`.

## Building

See [GETTING_STARTED.md](GETTING_STARTED.md). In short, from a VS 2022+ x64 developer prompt with `VCPKG_ROOT` set:

```
cmake --preset release
cmake --build --preset release
```

The core logic (cache window, scheduler, budgets, EXIF, pixel ops, settings) is platform independent and unit-tested on every push: `cmake --preset core-tests && cmake --build --preset core-tests && ctest --preset core-tests`.

How it works: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Contributor rules: [.agents/rules/AGENTS.md](.agents/rules/AGENTS.md). History of v1/v2 crash hunting: [docs/HISTORY.md](docs/HISTORY.md).

## Contributor guidelines: do's and don'ts

**Do**
- Wrap `ComPtr<T>` in explicit scopes, or declare a `ComApartment` first, so every COM object is released before `CoUninitialize()`.
- Parse the command line with `CommandLineToArgvW(GetCommandLineW(), ...)`.
- Call `SetLastError(0)` before creating the single-instance mutex.
- Use non-throwing Win32 file APIs, or `std::error_code` overloads.
- Open files with `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE` (`OpenShared`), so Explorer can always move or delete them.
- Route every new cache allocation through the scheduler's accounting: size it with `core::PlanDecode` / `core::TransientBytes`.

**Don't**
- Don't start threads per image, or allocate decode buffers outside a budgeted job.
- Don't write files (logs, thumbnails, caches) to disk.
- Don't render continuously: request frames with `RequestFrames()`.
- Don't use `/ENTRY` linker hacks to hide the console. The app uses a real `wWinMain`.
