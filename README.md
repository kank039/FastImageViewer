# Fast Image Viewer (v3, "revamped")

A native Windows 10/11 image viewer that shows the next image **instantly**, keeps the last few in memory so going back is instant too, and never trades your machine's health for it:

- **Hard memory ceiling.** Every byte is budgeted *before* it is allocated (the image header is read first, so sizes are exact, not guessed). Budgets are capped by your hardware: a GPU with *n* GB of VRAM can give the viewer at most *n − 1* GB, and RAM always leaves Windows its share.
- **Never spills to the page file or demotes textures.** The cache shrinks automatically when Windows reports memory pressure, when other programs take VRAM (DXGI budget notifications), or when the window is minimized.
- **Gentle on disks.** Each file is read once, sequentially. Hard disks and network shares get one reader at a time (no head thrashing). Prefetch I/O runs at background priority. Nothing is written except a tiny settings file, and only when a setting changes. OneDrive online-only files are not prefetched, because reading them downloads them.
- **Fast.** Images are decoded at your monitor's resolution, not full size (a 24 MP photo on a 1440p screen is ~15 MB instead of 96 MB), JPEGs use the codec's built-in 1/2–1/8 scaling, textures are created on worker threads, and the image on screen has its own express worker so it never waits behind prefetching.
- **Idle means idle.** Rendering is event-driven: no input, no frames.

Tech: C++20, Win32, Direct3D 11 + DirectComposition (Mica), WIC (Windows Imaging Component) for decoding, Dear ImGui for the UI. One self-contained `.exe`.

## Using it

Open an image from Explorer ("Open with"), drag one onto the window, or press **Ctrl+O**. A second launch hands the file to the open window. Every key below can be changed in **Settings → Controls**; **H** shows the current list.

| Keys (defaults) | Action |
|---|---|
| → ↓ Space / ← ↑ Backspace | Next / previous |
| PgDn / PgUp, Home / End | Next / previous set (compare mode), first / last |
| Wheel, + / − | Zoom at the cursor (or change image: Settings → Controls). Ctrl+wheel and touchpad pinch always zoom |
| Tilt wheel, two-finger swipe | Previous / next (pans when zoomed in) |
| Drag, double-click, Z / 0 | Pan, toggle 100 % / fit |
| R / Shift+R | Rotate the view (this session only, the file is not changed) |
| F, F11, Alt+Enter | Fullscreen |
| C / Shift+C | Compare 2 or 4 images (synced zoom) / compare the current burst, sharpest one marked |
| Tab, click | Pick the active image in compare mode |
| G | Grid of thumbnails (arrows, Enter, double-click; + / − change the size) |
| K | Focus peaking: sharp edges painted red |
| P / X / U, Ctrl+1 … 5 / Ctrl+0 | Pick / reject / clear, star rating / clear stars (session only) |
| Ctrl+F | Show: all, picks, rated, unmarked, rejects |
| Shift+P | Marks panel: counts, filter, batch move / delete / copy paths |
| Delete, 1 … 9 | Recycle Bin / move to destination 1 … 9, together with its RAW/XMP |
| Ctrl+Z | Undo the last delete or move, including whole batches (up to 50) |
| L | Slideshow |
| Ctrl+C / Ctrl+Shift+C | Copy the image / its path |
| Ctrl+E / E | Show in Explorer / open in your editor (or "Open with") |
| I / O / B / S / F3 / H | Info and histogram / overlay / sidebar / settings / stats / help |
| F5, Esc | Rescan; close panel, stop slideshow, leave grid, compare, filter or fullscreen, then quit |

Touch screens: pinch to zoom, drag to pan, swipe sideways for the next image, two-finger tap for 100 %. Animated GIFs play.

### Culling workflow

- **Mark as you go:** **P** picks, **X** rejects, **Ctrl+1–5** rates; the next image follows automatically (optional). Marks live in memory for this session only: nothing is written to your files. **Ctrl+F** narrows the view to picks, rated, unmarked or rejects; prefetching follows the filtered order.
- **Apply in one go:** **Shift+P** opens the marks panel. Move all picks to destination 2, send all rejects to the Recycle Bin, or copy their paths. A batch is one Windows operation and one **Ctrl+Z**.
- **Bursts:** shots from one camera within 2 s (adjustable) get a coloured bar in the sidebar and grid. **Shift+C** shows the burst side by side and marks the sharpest (a focus score of the decoded image). **K** shows focus peaking on any image.
- **Compare** any images with **C**. Zoom into the eyes on one and every tile follows.
- **RAW + JPEG pairs** (`IMG_1234.jpg` + `.CR2` + `.xmp`) show once and are deleted, moved and restored together (Settings → Sorting).
- **Destinations:** keys 1–9 can point anywhere (`D:\Keepers`, `..\Rejects`). Name clashes become `name (2).jpg`, or the move is refused if you prefer. Files are never overwritten.
- **Sort** by name, date taken, date modified, size or type, and **include subfolders** (their move destinations are left out).

The **stats panel (F3)** shows the cache map, texture and decode memory against their limits, the live Windows VRAM budget, free system RAM, process memory and how many files have been read from disk, so you can see the guarantees hold.

## Settings that matter

**Settings → Cache & memory**

- **Prefetch ahead** (default 4) and **Keep behind** (default 5). The window follows your direction of travel.
- **Memory budget:** *Automatic* picks a safe size for your hardware. Manual sliders only go up to what the hardware can handle:
  - VRAM: *n − 1* GB on cards with 2 GB or more, half the card below that, and never above the OS budget.
  - RAM: total minus max(2 GB, 25 %).
  - On integrated GPUs, textures live in system RAM, so a single shared budget is shown.
- **Resolution:** *Fit to screen* (recommended) or *Full resolution*. Zooming into huge images (panoramas, scans over 16384 px) decodes just the visible area at full resolution.
- **Keep images you have seen compressed:** history is stored as BC1 textures (8× smaller), so up to 48 images stay ready for going back.
- **Release cache when minimized**, and **Don't prefetch online-only files**.

**Settings → Display:** backdrop, colour management (embedded profiles → sRGB, or → your display's profile; HDR images are tone-mapped).
**Settings → Sorting:** sort order, subfolders, burst gap, grid size, RAW+JPEG pairing, rename-on-conflict, and the folder for each key 1–9.
**Settings → Controls:** wheel behaviour, slideshow speed, external editor, and every keyboard shortcut.

Settings are stored in `%LOCALAPPDATA%\FastImageViewer\settings.ini`. Nothing else is ever written.

## Installing

Download the installer or the portable zip from the GitHub release (built by CI on every `v*` tag; signed when the repository has a code-signing certificate). The installer works without administrator rights. The portable zip includes `install.ps1` for "Open with" registration.

## Building

See [GETTING_STARTED.md](GETTING_STARTED.md). In short, from a VS 2022+ x64 developer prompt with `VCPKG_ROOT` set:

```
cmake --preset release
cmake --build --preset release
```

The core logic (cache window, scheduler, budgets, EXIF, pixel ops, key map, marks, sorting, bursts, BC1, GIF compositing, settings) is platform independent and unit-tested on every push: `cmake --preset core-tests && cmake --build --preset core-tests && ctest --preset core-tests`.

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
