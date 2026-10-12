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

Ideas for later
    [ ] Animated GIF / WebP playback
    [ ] ICC colour management
    [ ] Rotate / flip view commands
