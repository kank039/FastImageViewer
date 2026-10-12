The Master Checklist (v3 "revamped")

Environment
    [x] MSVC (VS 2022+), CMake, Ninja, vcpkg
    [x] Static vcpkg triplet: single self-contained .exe

Core engine
    [x] Fixed worker pool with an express lane for the image on screen
    [x] Exact per-image memory plans (header probe before any allocation)
    [x] Scheduler: priority admission, eviction, cancellation with zombie accounting
    [x] Direction-aware cache window (ahead / behind)
    [x] Hardware caps (VRAM n-1 GB, RAM minus reserve) and live limits (DXGI budget, RAM headroom, low-memory signal)
    [x] Screen-resolution decode, JPEG codec prescaling, full resolution only on zoom
    [x] Cooperative cancellation (read chunks, pixel bands)
    [x] Disk-type aware read gate (HDD / network: 1 reader), background I/O priority, EcoQoS
    [x] Incremental folder watching (no cache flushes), cloud placeholder protection

GPU rendering
    [x] Direct3D 11, immutable textures created on worker threads
    [x] CPU mip chains for clean downscaling
    [x] Event-driven rendering (no frames when idle)
    [x] DirectComposition + Mica with opaque fallback
    [x] Device-lost recovery

UI / UX
    [x] Zoom, pan, 100 % toggle, fullscreen, EXIF rotation, thumbnail preview while loading
    [x] Overlay, virtualized sidebar, stats panel, help, settings that persist and apply immediately
    [x] Drag & drop, Ctrl+O, single instance hands files to the running window
    [x] Recycle Bin with permanent-delete warning, move to subfolders 1-9

Quality
    [x] Portable core unit tests (sanitizers in CI)
    [x] GitHub Actions: MSVC Windows build + tests
    [ ] Hands-on test pass on real Windows hardware (HDD, network share, OneDrive folder, integrated GPU, HDR monitor)

Culling (v3.1)
    [x] Undo for delete / move (exact, via IFileOperation)
    [x] Compare 2 / 4 with synced zoom
    [x] RAW + JPEG + XMP pairing
    [x] Configurable destinations for 1-9, rename on conflict
    [x] Info panel: EXIF + histogram

Workflow (v3.2)
    [x] Rebindable keys, wheel modes, touchpad and touch gestures
    [x] View rotation, slideshow, clipboard (image / path), Show in Explorer, open in editor
    [x] Session-only picks / rejects / stars, filters, batch apply with one-step undo
    [x] Sort orders, subfolders, bursts with sharpest-shot marking, focus peaking, grid view
    [x] Colour management (ICC to sRGB or display), HDR tone mapping
    [x] Animated GIF, 1:1 detail for huge images, BC1 compressed history
    [x] CI installer, optional code signing, releases on tags

Ideas for later
    [ ] Animated WebP playback
    [ ] True HDR output (scRGB swap chain) on HDR monitors
