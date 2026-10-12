Fast Image Viewer – v3.2 "workflow"

Culling
    Picks, rejects and 0–5 stars (P / X / U, Ctrl+0–5). Session only, by choice: they live in memory and are never written to the images. Optional auto-advance after marking.
    Filters (Ctrl+F): picks, rated, unmarked, rejects. Navigation, compare and prefetch follow the filtered list.
    Marks panel (Shift+P): counts, and batch move / Recycle Bin / copy paths for a selection, as one Windows operation and one undo step. Pairs stay together, and incomplete pairs are rolled back.
    Bursts: shots within N seconds from one camera are grouped (coloured bars). Shift+C compares the burst and marks the sharpest shot, using a focus score computed while decoding.
    Focus peaking (K), as a pixel shader: no extra memory.
    Grid view (G) with budgeted thumbnails. The sidebar now shows thumbnails for every visible row.
    Sorting by name, date taken (headers read in the background, lowest priority), date modified, size or type, ascending or descending. Optional subfolders (8 levels / 50,000 images; move destinations excluded).

Viewing
    Colour management: embedded ICC / Adobe RGB profiles are converted to sRGB or to the monitor's profile. HDR (floating-point) images are tone-mapped with a highlight roll-off instead of clipping.
    Animated GIFs play (all disposal modes), with their memory reserved from the budget.
    Huge images: zooming past what fits in one texture decodes the visible area at full resolution.
    Compressed history (optional): images behind you are kept as BC1 textures, 8x smaller, up to 48. They are transcoded from the GPU copy, with no second disk read.
    View rotation (R / Shift+R), slideshow (L, keeps the screen awake).

Controls & integration
    Every shortcut can be rebound (Settings > Controls); help is generated from the key map. Wheel can zoom or change images; tilt wheel and two-finger sideways swipes change images; touch: pinch, pan, swipe, two-finger tap.
    Ctrl+C copies the image (budgeted, on a worker), Ctrl+Shift+C the path, Ctrl+E shows it in Explorer, E opens it in your editor or the "Open with" dialog.

Build & release
    CI builds the Inno Setup installer (per-user, no admin) and a portable zip, signs both when a certificate secret is configured, and publishes a GitHub release for v* tags.

Fast Image Viewer – v3.1 "culling"

    Compare mode: C shows 2 or 4 consecutive images side by side with synced zoom and pan; Tab or a click picks the active one; PgUp/PgDn move by a whole set.
    Undo (Ctrl+Z, 50 levels) for deletes and moves. Windows reports where every file went, so moves are reversed and deletes are restored from the Recycle Bin exactly.
    RAW + JPEG (+ XMP) pairs are shown once and deleted, moved and restored together (Settings → Sorting).
    Keys 1–9 can point at any folder (relative or absolute, with Browse); name clashes become "name (2).jpg" or are refused; files are never overwritten.
    Info panel (I): camera, lens, exposure, focal length, date taken, size, and an RGB histogram with shadow/highlight clipping, at no extra disk or decode cost.
    I now toggles the info panel; the stats panel is F3.

Fast Image Viewer – v3.0 "revamped"

Rewritten from scratch around one rule: the cache may never hurt the machine.

Why: v2 started a thread per image, under-estimated decoded sizes by about 4x, ignored work still in flight, flushed the whole cache on every folder change, decoded everything at full resolution and rendered at 60 fps when idle. Together these exhausted RAM, pushed Windows into the page file and thrashed disks.

Cache & memory
    Exact budgeting: the header is probed first, every texture and every transient decode buffer is sized before allocation, and in-flight work is counted.
    Hardware caps: VRAM n-1 GB (n/2 below 2 GB) and RAM total minus max(2 GB, 25 %). Settings sliders cannot exceed them.
    Live limits: follows the DXGI VRAM budget (with change notifications), keeps system RAM headroom, reacts to Windows low-memory signals, and releases the cache when minimized.
    Direction-aware window: 4 ahead / 5 behind by default; the current image has its own express worker.
    Screen-resolution decoding (JPEG uses 1/2–1/8 codec scaling); full resolution only for the image you zoom into.
    Cancellation: images you skip past stop decoding within milliseconds.

Disk
    Fixed worker pool; 1 reader on HDD/network, 2 on SSD; prefetch at Windows background I/O priority with EcoQoS.
    No disk writes except settings.ini, and only when it changes. imgui.ini is gone.
    OneDrive online-only files are never prefetched (reading them would download them).
    Folder changes are applied incrementally and no longer flush the cache.

Display & UI
    Direct3D 11 + DirectComposition (Mica), mipmapped textures created on worker threads, event-driven rendering (0 frames when idle).
    Zoom (wheel, + / -, double-click for 100 %), pan, fullscreen, EXIF rotation, embedded-thumbnail preview while loading.
    Whole-folder virtualized sidebar, overlay, stats panel (F3), help (F1), drag & drop, Ctrl+O, and a second launch now opens the file in the running window.
    Settings persist (%LOCALAPPDATA%\FastImageViewer\settings.ini) and all of them apply immediately.

Fixes
    Unicode filenames no longer crash; filesystem calls no longer throw; the "DECODING..." screen no longer sticks forever; files are sorted the way Explorer sorts them; Delete warns before a permanent delete on drives with no Recycle Bin; moving to 1–9 never overwrites.

Build
    Single static .exe (x64-windows-static): no DLL drop. CMake presets and a GitHub Actions workflow (MSVC build plus sanitizer unit tests).

Fast Image Viewer – v1.0 Changelog
Core Engine & Architecture

    WIC Integration: Removed fragile third-party dependencies (stb_image, libjpeg-turbo) and wired the engine directly into the native Windows Imaging Component (WIC) for zero-dependency, OS-level image decoding.

    Asynchronous Pipeline: Decoupled image decoding from the main render thread using <future> and std::async. Heavy image math now happens invisibly in the background, keeping the UI at a locked framerate without hanging.

    Compiler Optimization: Migrated from standard MSBuild to the high-performance Ninja build system. Forced the Native x64 Microsoft Visual C++ toolchain to completely eliminate 32-bit compiler memory exhaustion (C1060).

    Zero-I/O Decoder: Refactored file reading to bypass WIC's internal disk streams. The engine now uses raw CreateFileW to pull entire files into RAM first, silently catching fatal HDD hardware errors or corrupted sectors without crashing the Windows COM APIs.

    Mechanical Drive Sympathy: Injected the FILE_FLAG_SEQUENTIAL_SCAN kernel hint to prevent older spinning hard drives from thrashing their read heads during massive folder ingests.

Memory & Cache Management

    Dynamic VRAM Pooling: Replaced the single-image buffer with an unordered map texture cache (textureCache) that dynamically allocates and evicts OpenGL textures.

    RAM-Budgeted Caching: The engine continuously estimates the uncompressed memory weight of surrounding files (using a diskSizeMB * 5.0 heuristic) and pre-loads them bi-directionally until it hits a strict, user-defined memory ceiling (e.g., 1024 MB).

    Resource Cleanup: Implemented strict cache flushing and VRAM destruction (glDeleteTextures) during folder navigation or when file lists change to prevent memory leaks.

User Interface & Layout

    Interactive Sidebar: Built a togglable (B) left-hand panel that visualizes the active RAM pool in real-time. It displays dynamically scaled thumbnails of all pre-loaded images, automatically scrolling to keep the active image centered.

    Status Indicators: Thumbnails are wrapped in configurable colored borders to instantly indicate their background thread status (Green for loaded, Red for corrupted/missing).

    Dynamic Data Matrix: Added a granular configuration matrix (S) allowing users to independently toggle metadata properties (Filename, Extension, Index, Disk Size, Pixel Dimensions) on both the Sidebar and the floating Overlay.

    Corner-Aware Overlay: The floating metadata overlay mathematically pivots its anchor point, allowing users to snap it perfectly into any of the four corners of the window.

    Empty State Handling: If a folder is emptied (via deletion or moving files), the UI drops into a safe "Blank Canvas" state instead of crashing, with an option to automatically close the application when the queue hits zero.

    Layout Scaling: Added real-time slider multipliers to allow users to dynamically adjust the width of the sidebar and the vertical height of the thumbnails inside it.

Windows OS Integration

    Live Directory Sync: Spawned a background thread utilizing the Windows Kernel FindFirstChangeNotificationW. The application now listens for external file drops or deletions in Windows Explorer and instantly rebuilds its cache without user input.

    Windows 11 DWM "Mica": Ripped out the solid black background, set the OpenGL framebuffer to transparent, and injected the DWMSBT_MAINWINDOW attribute to enable native, hardware-accelerated frosted glass behind the ImGui UI.

    Smart Codec Detection: Expanded file support to include WebP, AVIF, HEIC, TIFF, and GIF. The engine actively queries the Windows Registry at boot to check if modern codecs are physically installed.

    Store Deep-Links: If a user is missing a modern codec (like Apple's HEIC), the settings menu disables the toggle and generates a native OS deep-link (ms-windows-store://) to jump them straight to the official Microsoft Store download page.

    Silent Execution: Re-linked the executable using /SUBSYSTEM:WINDOWS and /ENTRY:mainCRTStartup to completely suppress the rogue black console terminal from appearing behind the graphical window.

Deployment & Distribution

    Automated Installer: Authored an Inno Setup (.iss) script that compresses the .exe and dynamically linked vcpkg .dll files into a single Setup_v1.0.exe wizard.

    Custom Application Icon: Integrated user-provided .ico files directly into the installer executable, the start menu shortcuts, and the uninstaller registry.

    Registry Injection (ProgID): Hand-wrote strict Windows Registry hooks into the installer so the application registers as a certified Image File handler, allowing users to natively right-click and set it as their default Windows 11 photo viewer.