# History: the v1/v2 "silent crash" chronicle

Kept for reference. These lessons still apply and are encoded in AGENTS.md; the v3 code follows all of them.


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