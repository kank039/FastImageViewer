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