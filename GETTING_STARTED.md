# Getting Started: Fast Image Viewer Contributor Guide

Welcome to the **Fast Image Viewer** project! This document outlines the exact steps required to set up your local development environment, compile the C++ engine, and successfully test it without hitting the silent OS-level traps inherent to native Windows development.

## 1. Prerequisites
Before you clone the repository, ensure your Windows 11 workstation has the following tools installed:

* **Visual Studio 2022** (with the "Desktop development with C++" workload installed)
* **Windows 11 SDK** (Required for the DWM Mica integration)
* **CMake** (v3.15 or higher)
* **Ninja Build System** (Highly recommended for parallel compilation)
* **vcpkg** (C++ Package Manager)

## 2. Setting Up Dependencies
We utilize `vcpkg` to manage our third-party libraries (GLFW, GLAD, and Dear ImGui). 

1. Open your terminal and navigate to your `vcpkg` installation directory.
2. Install the required packages for the x64-windows triplet:
   `vcpkg install glfw3 glad imgui[glfw-binding,opengl3-binding] --triplet=x64-windows`

## 3. Configuring and Compiling the Project
We use the **x64 Native Tools Command Prompt for VS 2022** to ensure the compiler uses the correct 64-bit architecture and prevents memory exhaustion errors.

1. Open the **x64 Native Tools Command Prompt**.
2. Navigate to the root of the cloned directory.
3. Generate the build environment using CMake and the vcpkg toolchain (replace `[path_to_vcpkg]` with your actual installation path):
   `cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE="[path_to_vcpkg]\scripts\buildsystems\vcpkg.cmake"`
4. Compile the engine:
   `cmake --build build`

## 4. ⚠️ CRITICAL: The DLL Drop
If you attempt to run the compiled `.exe` right now, **it will silently fail and instantly close** with no error messages. The Windows OS loader cannot find the dynamically linked `glfw3.dll` and aborts the process before our internal C++ error handlers can engage.

**You must manually copy the DLLs into the build folder:**
1. Open Windows File Explorer.
2. Navigate to `build\vcpkg_installed\x64-windows\bin\`.
3. Copy `glfw3.dll` (and any other `.dll` files present).
4. Paste them directly into your `build\` directory right next to `FastImageViewer.exe`.

## 5. Testing with a Sample Image
Run the executable from the command line, passing an absolute path to a sample image. (Test with an image name containing spaces to verify the UTF-16 kernel parser).

`.\build\FastImageViewer.exe "C:\Users\Public\Pictures\Sample Image.jpg"`