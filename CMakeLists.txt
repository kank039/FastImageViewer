cmake_minimum_required(VERSION 3.21)

# Static MSVC runtime: one self-contained .exe, no DLLs to copy next to it.
# Must be set before project() so vcpkg's static triplet and our code agree.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(FastImageViewer VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FIV_BUILD_TESTS "Build the core unit tests" ON)

# ---- Portable core: cache policy, budgets, pixel ops, settings (no Windows headers)
add_library(fiv_core STATIC
    src/core/CacheWindow.cpp
    src/core/DecodePlan.cpp
    src/core/Exif.cpp
    src/core/FileGroups.cpp
    src/core/Histogram.cpp
    src/core/MemoryLimits.cpp
    src/core/Pixels.cpp
    src/core/Scheduler.cpp
    src/core/Settings.cpp
)
target_include_directories(fiv_core PUBLIC src)
if(MSVC)
    target_compile_options(fiv_core PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(fiv_core PRIVATE -Wall -Wextra)
endif()

if(FIV_BUILD_TESTS)
    enable_testing()
    add_executable(core_tests tests/core_tests.cpp)
    target_link_libraries(core_tests PRIVATE fiv_core)
    add_test(NAME core_tests COMMAND core_tests)
endif()

# ---- The Windows application
if(WIN32)
    find_package(imgui CONFIG REQUIRED)

    configure_file(src/app/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)

    add_executable(FastImageViewer WIN32
        src/main.cpp
        src/app/App.cpp
        src/app/AppUi.cpp
        src/platform/FileOps.cpp
        src/platform/FileStream.cpp
        src/platform/Folder.cpp
        src/platform/Formats.cpp
        src/platform/Gpu.cpp
        src/platform/Jobs.cpp
        src/platform/System.cpp
        src/platform/WicCodec.cpp
        src/platform/Win.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/app.rc
    )
    target_compile_definitions(FastImageViewer PRIVATE
        UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    target_link_libraries(FastImageViewer PRIVATE
        fiv_core imgui::imgui
        d3d11 dxgi dcomp dwmapi windowscodecs shlwapi shell32 ole32 uuid)

    if(MSVC)
        # The manifest (DPI awareness, long paths, UTF-8) is merged by the linker.
        target_sources(FastImageViewer PRIVATE src/app/app.manifest)
        target_compile_options(FastImageViewer PRIVATE /W4 /permissive- /utf-8 /wd4100)
        set_property(TARGET FastImageViewer PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    else()
        # MinGW: wWinMain entry point; the manifest is embedded through app.rc.
        target_compile_definitions(FastImageViewer PRIVATE FIV_RC_MANIFEST)
        set_property(SOURCE ${CMAKE_CURRENT_BINARY_DIR}/app.rc APPEND PROPERTY COMPILE_DEFINITIONS FIV_RC_MANIFEST)
        target_link_options(FastImageViewer PRIVATE -municode -static)
        target_compile_options(FastImageViewer PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    endif()
endif()
