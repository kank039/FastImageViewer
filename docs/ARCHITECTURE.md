# Architecture

## Why v2 hurt the machine

v2 had five problems:

| Problem in v2 | Effect | v3 design |
|---|---|---|
| A new `std::async` thread for every cache miss. The RAM mode wanted about 100 images at once. | Dozens of simultaneous reads and decodes, CPU at 100 %, disk seeking | Fixed worker pool. A scheduler admits work only when it fits the budget. |
| Decoded size estimated as file size × 5 (real JPEG ratio is 10–20×). Images still loading were not counted. | The "1 GB" budget really allowed about 4 GB, so Windows paged | Header probe first. Exact byte plans. In-flight work is counted. |
| Every image decoded at full resolution | 50 MP = 200 MB of texture | Decoded at monitor resolution. Full resolution only for the zoomed image. |
| Any folder change flushed the whole cache. `FILE_NOTIFY_CHANGE_SIZE` fires repeatedly while a file is being copied. | Constant re-reading of the folder | Incremental list updates. Cache keyed by file version, not index. |
| Loads could not be cancelled. Uploads were synchronous on the UI thread. Rendering ran at 60 fps all the time. | Spikes, stutter, idle GPU load | Cooperative cancellation. Textures created on workers. Event-driven rendering. |

## Layers

```
src/core/       portable C++20, unit-tested on Linux and Windows
  CacheWindow   which folder indices to hold, in priority order
  DecodePlan    exact texture / transient byte sizes for a decode
  MemoryLimits  hardware caps (sliders) and live limits (right now)
  Scheduler     admission, eviction, dispatch, cancellation accounting
  Pixels        EXIF orientation parser, rotation, mip generation
  Settings      validation against caps, key=value serialization
src/platform/   Windows: Gpu (D3D11/DXGI/DComp), WicCodec, Jobs (pool, IoGate,
                background mode), Folder (model + watcher), System, FileStream
src/app/        App (window, loop, input, scheduler host), AppUi (ImGui)
```

## The cache window

`ComputeWindow` returns the slots in this priority order: current, +1, +2, −1, +3 … +ahead, −2 … −behind. "+" means the direction of travel, so pressing ← flips the window. The image you're about to see always comes first, and a single step back is next.

## From want to texture

Each event that could change what should be cached calls `App::UpdateCache()`. That covers navigation, a finished job, a budget change, a folder change and a settings change. `UpdateCache()` builds the list of `Want`s and calls `Scheduler::Pump()`, which works in four steps:

1. **Probe.** For an unknown file, read only the header: size, EXIF orientation, whether the codec can pre-scale, and the embedded thumbnail for the current image. That makes every later number exact.
2. **Admit.** Walk the wants in priority order, adding up texture bytes, and admit each want that fits the GPU pool. Anything already resident that no longer fits is evicted, lowest priority first. A lower-ranked want can never block a higher one.
3. **Dispatch.** Start admitted decodes while worker slots and the transient RAM pool allow:
   - Transient RAM covers the file buffer, decoder output, rotation scratch, the mip chain, the driver's upload copy and codec working memory.
   - The current image has an extra express slot and its own thread.
   - While you are skimming (less than 120 ms since the last key), only the current image starts new work.
4. **Cancel.** Work that falls out of the window is cancelled. Its bytes stay counted (as a "zombie") until the worker reports back, so cancelling never frees memory early. Workers check the cancel flag between 2 MB read chunks and between 256-row pixel bands.

`DecodeImage` follows the plan exactly, in this order:

1. Read the file into RAM (or stream it, for files over 64 MB).
2. Let the codec decode at 1/2, 1/4 or 1/8 scale where it can (JPEG).
3. Convert to BGRA and scale (high-quality cubic) to the planned size.
4. Rotate per EXIF.
5. Build the mip chain on the CPU.
6. Create an immutable texture on the worker thread, then free the CPU copy.

## Limits

### Caps (what the sliders allow)

- **VRAM:** n − 1 GB on cards with 2 GB or more, and n/2 below that. It is also capped at the startup DXGI budget minus 256 MB.
- **RAM:** total − max(2 GB, 25 %).
- **Integrated GPUs (UMA):** textures live in system RAM, so there is one shared pool.

### Live limits (checked once a second, and on DXGI budget-change events)

- **RAM:** the cache never takes the system's free memory below a headroom of 10 % of RAM (at least 1 GB, at most 2 GB). If Windows signals low memory, the cache halves on each check down to a floor.
- **VRAM:** the cache stays inside the DXGI budget Windows grants this process, minus 256 MB. Past that budget, Windows silently demotes textures to system memory.
- **Minimized for 10 s** (if enabled): everything but the current image is released.

Our own usage is added back into each limit, so the limits don't oscillate as the cache grows.

## Disk behaviour

- **Storage type:** detected per folder with the seek-penalty query. SSDs get 2 parallel readers; hard disks, network and removable drives, and unknown devices get 1.
- **Read priority:** the read gate serves readers by priority, and a long prefetch read yields to the current image between chunks.
- **Background mode:** prefetch runs in Windows "background mode" (`THREAD_MODE_BACKGROUND_BEGIN`). That gives it very low I/O priority, and low memory priority for the file pages it pulls into the OS cache. It also runs under EcoQoS, which prefers efficiency cores.
- **Sharing:** all opens use full share flags, so Explorer can always rename, move or delete a file being read.
- **Writes:** none, apart from `settings.ini`. It is written atomically, and only when its content actually changed. `imgui.ini` is disabled.
- **Online-only files** (OneDrive placeholders) are never prefetched. Reading one triggers a download to disk.

## Folder changes

`ReadDirectoryChangesW` reports which file changed, and changes are batched until 300 ms after the last one.

- Each changed name is re-stat'ed. Its id changes only if its size or modification time changed, so the rest of the cache is untouched.
- Files changed in the last 1.5 s are not decoded yet, because they may still be being written.
- If the change buffer overflows, the folder is rescanned. The rescan keeps the ids of files that didn't change, so cached images survive.

## Known limitations

- **Probes are not budgeted.** They read only a file's header, plus the embedded thumbnail for the current image. At most 3 run at once, which bounds them at roughly 36 MB.
- **Exit can wait on a stalled read.** A header probe, or a streamed decode of a file over 64 MB, only checks for cancellation between steps, not during a read. If a network share stalls or a OneDrive download hangs, closing the app waits for that read to finish. A graphics device reset doesn't wait: jobs carry a device generation number and their results are discarded.
- **Animated GIF/WebP** show their first frame only. **Colour management** (ICC profiles) is not applied.
