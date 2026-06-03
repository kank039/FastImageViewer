The Master Checklist
Phase 1: Environment Setup (Windows 11) <- Current Step

    [x] Install the Microsoft Visual C++ (MSVC) compiler via Visual Studio Build Tools.

    [x] Install CMake (the industry-standard C++ build configuration tool).

    [x] Install Git (if not already present) and set up vcpkg (Microsoft's C++ package manager).

Phase 2: Project Blueprint & Window Lifecycle

    [x] Create the project directory structure and configure the master CMakeLists.txt file.

    [x] Fetch and link dependencies via vcpkg (sdl2, imgui, gl3w or glad).

    [x] Write the boilerplate code to spawn a native Windows window using SDL2 and open a hardware-accelerated OpenGL or DirectX 12 context.

    [x] Hook Dear ImGui into the render loop to draw a basic "Hello World" overlay.

Phase 3: The Core Performance Pipeline (The Hardware Engine)

    [x] Implement Win32 Memory-Mapped Files (CreateFileMapping, MapViewOfFile) to bypass traditional slow disk I/O.

    [ ] Integrate SIMD-accelerated decoding libraries: libjpeg-turbo for JPEGs and libspng for PNGs.

    [ ] Create a dedicated background worker thread pool to handle asynchronous decoding.

    [ ] Build a multi-threaded ring buffer cache to pre-fetch the next and previous images in a directory before you even click them.

Phase 4: Zero-Copy GPU Rendering

    [ ] Implement asynchronous pixel transfers to the GPU using Pixel Buffer Objects (PBOs) or staging buffers to prevent micro-stutters during loading.

    [ ] Write custom GPU vertex/fragment shaders to handle panning, zooming, and color clamping instantly at your monitor's exact refresh rate.

    [ ] Generate hardware mipmaps on the fly so zooming out of massive 50MP files doesn't cause aliasing or performance drops.

Phase 5: UI & UX Polish

    [ ] Strip the standard Windows window borders to make a modern, frameless, minimalist UI.

    [ ] Design the immediate-mode ImGui control layout (only visible when hovering or triggered).

    [ ] Map low-latency keyboard shortcuts for instant navigation, rotation, and file deletion.

Phase 6: Production Hardening & Stripping

    [ ] Enable aggressive compiler flags: Profile-Guided Optimization (PGO) and Link-Time Optimization (LTO/LTCG).

    [ ] Strip debug symbols and compile into a standalone, single executable under 10MB that boots under 10 milliseconds.