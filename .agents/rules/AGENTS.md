# AI Agent Context & Guidelines

**Target Audience:** Autonomous AI Agents, LLM Coding Assistants, and Automated Refactoring Tools.

## Project Overview
This repository contains the **Fast Image Viewer**, a hyper-optimized, zero-dependency, native Windows 11 application. 

**Core Directives for Agents:**
* **Do NOT introduce heavy frameworks** (Qt, Electron).
* **Do NOT introduce third-party image decoding libraries** (stb_image, libjpeg). 
* All image decoding MUST be done using the native Windows Imaging Component (WIC) via COM interfaces.
* The application utilizes Windows 11 DWM (`DWMSBT_MAINWINDOW` / Mica) for transparent frosted glass effects.

## 🛑 CRITICAL DIRECTIVES & KNOWN PITFALLS
During the development of this engine, several fatal, "silent-crash" traps were uncovered. **You must strictly adhere to the following rules to prevent regression.**

### 1. COM Smart Pointer Scope Violations (The Silent Assassin)
If you write or modify code using Windows COM interfaces (WIC), you MUST wrap the instantiation of `ComPtr<T>` objects inside an explicit scope block `{ }` if `CoUninitialize()` is called in the same function.
* **Why:** `ComPtr` objects destruct at the end of their scope. If `CoUninitialize()` executes *before* the `ComPtr` destructs, the OS will trigger an immediate, untraceable Memory Access Violation (Segfault).

### 2. The Mutex "Garbage Memory" Bug
Do not check for `ERROR_ALREADY_EXISTS` immediately after `CreateMutexA`. 
* **Why:** Windows often leaves garbage error codes (frequently `183`) in the thread's memory during OS boot sequence. Reading it immediately will falsely trigger the singleton-lock and cause the app to instantly close itself.
* **Rule:** You MUST manually clear the error state using `SetLastError(0);` *before* creating the Mutex.

### 3. Subsystem Entry Points & Unicode Arguments
This application is compiled with the `/SUBSYSTEM:WINDOWS` flag to hide the console terminal. Therefore, the entry point is `WinMain`, not `main`.
* **Why:** In a `WinMain` environment, the standard MSVC global `__wargv` array is NOT guaranteed to be initialized and may be a `NULL` pointer. Dereferencing it will crash the app instantly.
* **Rule:** ALWAYS use the native kernel parser `CommandLineToArgvW(GetCommandLineW(), &wargc)` to extract file paths. 

### 4. `std::filesystem` Crashing
Never use the standard, throwing variants of `std::filesystem` methods (e.g., `fs::exists(path)`).
* **Why:** Windows users often have hidden, system-protected, or corrupted files in their image directories. A throwing filesystem call will bypass standard logic and crash the background thread.
* **Rule:** ALWAYS pass `std::error_code` to every filesystem operation.

### 5. Debugging Silent OS Kills
Because the console is hidden, missing DLLs (like `glfw3.dll`) will cause the Windows OS Loader to kill the executable in 1 millisecond with zero visual feedback.
* **Rule:** If you are instructed to debug a "silent crash on launch", your first diagnostic step should be to instruct the user to comment out `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);` so the OS can display missing-DLL warning boxes.