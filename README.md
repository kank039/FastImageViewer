# Fast Image Viewer (Native Win32 / OpenGL)

A hyper-optimized, zero-dependency native Windows 11 image viewer built for speed. It bypasses heavy frameworks (like Qt or Electron) by hooking directly into the Windows OS layer, utilizing the Windows Imaging Component (WIC) for decoding and OpenGL/GLFW for hardware-accelerated rendering.

It features multithreaded background caching, dynamic RAM management, and native Windows 11 Desktop Window Manager (DWM) integration for hardware-accelerated "Mica" frosted glass interfaces.

## 🛠 Tech Stack

- **Language:** C++20
- **Build System:** CMake + Ninja
- **Windowing:** GLFW3 + Native Win32 API (`WinMain`)
- **Graphics:** OpenGL 3.3 (via GLAD)
- **UI:** Dear ImGui
- **Image Decoding:** Native WIC (Windows Imaging Component) + COM Interfaces

## 🛑 Contributor Guidelines: Do's and Don'ts

Native Windows development is a minefield of silent failures, memory access violations, and polluted OS memory. **Read these before modifying the core engine.**

### The Do's

- **DO wrap COM Smart Pointers in explicit scopes:** If you use `ComPtr<T>`, you MUST ensure it destructs _before_ calling `CoUninitialize()`. Wrap your COM logic in `{ }` blocks.
    
- **DO use Kernel Path Parsing:** Always use `CommandLineToArgvW(GetCommandLineW(), ...)` to parse incoming file paths. Standard C++ `argv` will corrupt Unicode characters and paths with spaces.
    
- **DO clear OS memory before checking Mutexes:** Windows often leaves garbage error codes (like `183`) in memory during boot. Always call `SetLastError(0)` before checking `GetLastError()` for your application Mutex.
    
- **DO use `std::error_code` for filesystem checks:** Never use throwing versions of `std::filesystem` functions. Wrap them in `std::error_code` to prevent the app from silently crashing on corrupted or protected directories.
    

### The Don'ts

- **DON'T use CMake Linker Hacks to hide the console:** Do not use `/ENTRY:mainCRTStartup` to force a graphical subsystem. It bypasses the Windows C-Runtime initialization. Write a proper `WinMain` entry point instead.
    
- **DON'T assume `__argc` and `__wargv` are populated:** When using `WinMain`, the C-Runtime does not guarantee these global variables are safely initialized, leading to instant `NULL` pointer Segfaults.
    
- **DON'T suppress errors while debugging:** If the app is silently failing, comment out `SetErrorMode(SEM_FAILCRITICALERRORS)` to allow Windows to show you missing DLL popups.
    

## 🐛 The "Silent Crash" Debugging Chronicle

During the transition from a console application to a native, silent DWM-integrated GUI, the application compiled perfectly but suffered from immediate, silent shutdowns.

Here is the exact ordered list of what we tried, and what ultimately fixed it:

1. **The CMake Linker Hack:** We initially tried linking `/SUBSYSTEM:WINDOWS` while using a standard `main()` function by overriding the entry point via CMake.
    
    - _Result:_ Failed. Bypassing the C-Runtime caused the OS to instantly terminate the app when requesting DWM window transparency.
        
2. **The `WinMain` Migration & MSVC Globals:** We switched to the correct `WinMain` entry point and tried reading arguments using the MSVC global variables `__wargv`.
    
    - _Result:_ Failed. `WinMain` left `__wargv` uninitialized (as a `NULL` pointer). Dereferencing it caused a fatal Memory Access Violation (Segfault) before the engine could boot.
        
3. **The Try-Catch & DLL Drop:** We implemented the Kernel `CommandLineToArgvW` parser, wrapped the boot sequence in a master `try-catch`, and manually ensured `glfw3.dll` was in the root directory.
    
    - _Result:_ Failed. The application still refused to open and threw zero C++ exceptions, meaning the OS was killing it.
        
4. **The Mutex Zombie Hunt:** We checked the OS exit code (`echo %ERRORLEVEL%`), which returned `0` (a clean, intentional exit). We assumed a background zombie process was holding the Mutex.
    
    - _Result:_ Failed. No zombie process existed. The Windows OS boot sequence was leaving garbage data in memory, causing `GetLastError()` to falsely trigger our "App Already Open" Mutex check. We patched it by adding `SetLastError(0)`.
        
5. **The Final Boss (What Actually Worked):**
    
    - After bypassing the Mutex, the app _still_ died silently. Using `MessageBoxA` tracers, we pinpointed the exact moment of death: immediately after `DetectInstalledCodecs()` finished.
        
    - **The Fix:** C++ Smart Pointers (`ComPtr`) used to query the Windows Registry do not destruct until they hit the bottom of the function. We were calling `CoUninitialize()` to shut down the Windows COM subsystem _while the pointer was still holding onto it_. This triggered an immediate, untraceable OS-level Segfault. We fixed it by wrapping all `ComPtr` initializations inside explicit `{ }` scope blocks, forcing them to safely destruct in RAM before the subsystem went offline.