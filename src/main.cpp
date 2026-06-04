#define GLFW_EXPOSE_NATIVE_WIN32
#include <stdlib.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <future>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

#ifdef _WIN32
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2
#endif

// --- FORMAT & CODEC TRACKING ---
struct ImageFormat {
    std::string displayName;
    std::vector<std::string> extensions;
    bool enabled;
    bool requiresExtension;
    bool isInstalled;
    std::string storeUrl;
};

std::vector<ImageFormat> appFormats = {
    {"JPEG", {".jpg", ".jpeg"}, true, false, true, ""},
    {"PNG", {".png"}, true, false, true, ""},
    {"BMP", {".bmp"}, true, false, true, ""},
    {"GIF", {".gif"}, true, false, true, ""},
    {"TIFF", {".tiff", ".tif"}, true, false, true, ""},
    {"WebP", {".webp"}, true, true, false, "ms-windows-store://pdp/?ProductId=9PG2DK419DRG"},
    {"HEIC / iPhone", {".heic", ".heif"}, true, true, false, "ms-windows-store://pdp/?ProductId=9NMZLZ57R3T7"},
    {"AVIF", {".avif"}, true, true, false, "ms-windows-store://pdp/?ProductId=9MVZQVXJBQ9V"}
};

// --- SYSTEM STATE & SETTINGS ---
struct ImageFileInfo {
    fs::path path;
    std::string filename;
    std::string extension;
    float diskSizeMB = 0.0f;
};

std::vector<ImageFileInfo> imageFiles;
int currentIndex = 0;
fs::path targetDirectory; 

std::atomic<bool> dirNeedsRefresh{false};
std::atomic<bool> isWatching{true};
std::thread dirWatcherThread;

bool showOverlay = true;
bool showSettings = false;
int loopBehavior = 0; 
bool closeOnEmpty = false; 

bool enableSidebar = true;
float sidebarWidthMultiplier = 0.22f; 
float sidebarHeightMultiplier = 1.0f; 
int overlayPosition = 1; 

bool ovShowName = true; bool ovShowIndex = true; bool ovShowDim = true; bool ovShowSize = true; bool ovShowExt = true;
bool sbShowName = false; bool sbShowIndex = true; bool sbShowDim = true; bool sbShowSize = true; bool sbShowExt = false;

int cacheStrategy = 1; 
int cacheLimitCount = 5; 
int cacheLimitRAM = 1024; 
ImVec4 colorSuccess = ImVec4(0.2f, 0.8f, 0.2f, 1.0f); 
ImVec4 colorError = ImVec4(0.8f, 0.2f, 0.2f, 1.0f); 

// --- CACHE MANAGER ---
enum class CacheStatus { Loading, Ready, Error };
struct RawImageData { int index = -1; int width = 0; int height = 0; std::vector<BYTE> pixels; bool success = false; };
struct CacheEntry { GLuint textureID = 0; int width = 0; int height = 0; size_t sizeBytes = 0; CacheStatus status = CacheStatus::Loading; };

std::unordered_map<int, CacheEntry> textureCache;
std::vector<std::future<RawImageData>> loadingFutures;
size_t currentRamUsage = 0;

// --- DYNAMIC CODEC DETECTION ---
bool CheckExtensionSupport(IWICImagingFactory* pFactory, const std::wstring& searchExt) {
    ComPtr<IEnumUnknown> pEnum;
    if (FAILED(pFactory->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault, &pEnum))) return false;
    
    ULONG cbActual;
    ComPtr<IUnknown> pUnk;
    while (pEnum->Next(1, &pUnk, &cbActual) == S_OK) {
        ComPtr<IWICBitmapDecoderInfo> pInfo;
        if (SUCCEEDED(pUnk.As(&pInfo))) {
            UINT cch = 0;
            if (SUCCEEDED(pInfo->GetFileExtensions(0, nullptr, &cch)) && cch > 0) {
                std::vector<WCHAR> exts(cch);
                if (SUCCEEDED(pInfo->GetFileExtensions(cch, exts.data(), &cch))) {
                    std::wstring supportedExts(exts.data());
                    if (supportedExts.find(searchExt) != std::wstring::npos) return true;
                }
            }
        }
    }
    return false;
}

void DetectInstalledCodecs() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) return;
    
    { // <--- NEW SCOPE BLOCK: Forces ComPtrs to release before CoUninitialize
        ComPtr<IWICImagingFactory> pFactory;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory)))) {
            for (auto& fmt : appFormats) {
                if (fmt.requiresExtension) {
                    std::wstring wExt(fmt.extensions[0].begin(), fmt.extensions[0].end());
                    fmt.isInstalled = CheckExtensionSupport(pFactory.Get(), wExt);
                    if (!fmt.isInstalled) fmt.enabled = false;
                }
            }
        }
    } // <--- END SCOPE BLOCK
    
    CoUninitialize();
}

bool IsFormatEnabled(const std::string& ext) {
    for (const auto& fmt : appFormats) {
        if (fmt.enabled && std::find(fmt.extensions.begin(), fmt.extensions.end(), ext) != fmt.extensions.end()) {
            return true;
        }
    }
    return false;
}

// --- DIRECTORY KERNEL WATCHER ---
void WatchDirectoryBackground(std::wstring targetPath) {
    HANDLE hChange = FindFirstChangeNotificationW(targetPath.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE);
    if (hChange == INVALID_HANDLE_VALUE) return;
    while (isWatching) {
        DWORD waitStatus = WaitForSingleObject(hChange, 250); 
        if (waitStatus == WAIT_OBJECT_0) {
            dirNeedsRefresh = true;
            FindNextChangeNotification(hChange);
        }
    }
    FindCloseChangeNotification(hChange);
}

// --- CORE LOGIC ---
RawImageData DecodeImageBackground(int index, fs::path filePath) {
    RawImageData result;
    result.index = index;
    
    HANDLE hFile = CreateFileW(filePath.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return result;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart == 0) { CloseHandle(hFile); return result; }

    std::vector<BYTE> fileBuffer(fileSize.QuadPart);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, fileBuffer.data(), (DWORD)fileSize.QuadPart, &bytesRead, NULL) || bytesRead != fileSize.QuadPart) {
        CloseHandle(hFile); return result;
    }
    CloseHandle(hFile);

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) return result;

    { // <--- NEW SCOPE BLOCK
        ComPtr<IWICImagingFactory> pFactory;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory)))) {
            ComPtr<IWICStream> pStream;
            if (SUCCEEDED(pFactory->CreateStream(&pStream))) {
                if (SUCCEEDED(pStream->InitializeFromMemory(fileBuffer.data(), (DWORD)fileBuffer.size()))) {
                    ComPtr<IWICBitmapDecoder> pDecoder;
                    if (SUCCEEDED(pFactory->CreateDecoderFromStream(pStream.Get(), NULL, WICDecodeMetadataCacheOnDemand, &pDecoder))) {
                        ComPtr<IWICBitmapFrameDecode> pFrame;
                        if (SUCCEEDED(pDecoder->GetFrame(0, &pFrame))) {
                            ComPtr<IWICFormatConverter> pConverter;
                            if (SUCCEEDED(pFactory->CreateFormatConverter(&pConverter))) {
                                if (SUCCEEDED(pConverter->Initialize(pFrame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) {
                                    UINT width, height;
                                    pConverter->GetSize(&width, &height);
                                    result.pixels.resize(width * height * 4);
                                    if (SUCCEEDED(pConverter->CopyPixels(NULL, width * 4, (UINT)result.pixels.size(), result.pixels.data()))) {
                                        result.width = width;
                                        result.height = height;
                                        result.success = true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    } // <--- END SCOPE BLOCK
    
    CoUninitialize();
    return result;
}

void UpdateCacheManager() {
    if (imageFiles.empty()) return;

    std::vector<int> desiredIndices;
    desiredIndices.push_back(currentIndex); 

    int step = 1;
    size_t estimatedRam = 0;
    size_t ramBudget = (size_t)cacheLimitRAM * 1024 * 1024;

    while (true) {
        if (cacheStrategy == 0 && desiredIndices.size() >= cacheLimitCount) break;
        if (cacheStrategy == 1 && estimatedRam >= ramBudget) break;
        if (step > imageFiles.size()) break; 

        int forwardIndex = currentIndex + step;
        if (loopBehavior == 0) forwardIndex = forwardIndex % imageFiles.size();
        if (forwardIndex < imageFiles.size() && std::find(desiredIndices.begin(), desiredIndices.end(), forwardIndex) == desiredIndices.end()) {
            size_t dynamicEstimate = (size_t)(imageFiles[forwardIndex].diskSizeMB * 5.0f * 1024.0f * 1024.0f);
            if (dynamicEstimate < (10 * 1024 * 1024)) dynamicEstimate = 10 * 1024 * 1024;
            if (cacheStrategy == 1 && (estimatedRam + (textureCache.count(forwardIndex) ? textureCache[forwardIndex].sizeBytes : dynamicEstimate)) > ramBudget) break;
            desiredIndices.push_back(forwardIndex);
            estimatedRam += textureCache.count(forwardIndex) ? textureCache[forwardIndex].sizeBytes : dynamicEstimate;
        }

        int backwardIndex = currentIndex - step;
        if (loopBehavior == 0) backwardIndex = (backwardIndex + imageFiles.size()) % imageFiles.size();
        if (backwardIndex >= 0 && std::find(desiredIndices.begin(), desiredIndices.end(), backwardIndex) == desiredIndices.end()) {
            size_t dynamicEstimate = (size_t)(imageFiles[backwardIndex].diskSizeMB * 5.0f * 1024.0f * 1024.0f);
            if (dynamicEstimate < (10 * 1024 * 1024)) dynamicEstimate = 10 * 1024 * 1024;
            if (cacheStrategy == 1 && (estimatedRam + (textureCache.count(backwardIndex) ? textureCache[backwardIndex].sizeBytes : dynamicEstimate)) > ramBudget) break;
            desiredIndices.push_back(backwardIndex);
            estimatedRam += textureCache.count(backwardIndex) ? textureCache[backwardIndex].sizeBytes : dynamicEstimate;
        }
        step++;
    }

    std::vector<int> toDelete;
    for (auto const& [index, entry] : textureCache) {
        if (std::find(desiredIndices.begin(), desiredIndices.end(), index) == desiredIndices.end()) {
            if (entry.status == CacheStatus::Ready) glDeleteTextures(1, &entry.textureID);
            currentRamUsage -= entry.sizeBytes;
            toDelete.push_back(index);
        }
    }
    for (int index : toDelete) textureCache.erase(index);

    for (int idx : desiredIndices) {
        if (textureCache.find(idx) == textureCache.end()) {
            textureCache[idx] = {0, 0, 0, 0, CacheStatus::Loading};
            loadingFutures.push_back(std::async(std::launch::async, DecodeImageBackground, idx, imageFiles[idx].path));
        }
    }
}

void RefreshFileList() {
    std::string currentFilename = imageFiles.empty() ? "" : imageFiles[currentIndex].filename;
    imageFiles.clear();
    
    std::error_code ec;
    if (targetDirectory.empty() || !fs::exists(targetDirectory, ec) || !fs::is_directory(targetDirectory, ec)) return;

    for (const auto& entry : fs::directory_iterator(targetDirectory, ec)) {
        if (ec) continue; 
        
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        if (IsFormatEnabled(ext)) {
            ImageFileInfo info;
            info.path = entry.path();
            info.filename = entry.path().stem().string();
            info.extension = ext;
            auto sizeBytes = fs::file_size(entry.path(), ec);
            info.diskSizeMB = ec ? 0.0f : (float)sizeBytes / (1024.0f * 1024.0f);
            imageFiles.push_back(info);
        }
    }

    currentIndex = 0;
    if (!currentFilename.empty()) {
        for (size_t i = 0; i < imageFiles.size(); i++) {
            if (imageFiles[i].filename == currentFilename) {
                currentIndex = (int)i; break;
            }
        }
    }

    for (auto const& [index, entry] : textureCache) {
        if (entry.status == CacheStatus::Ready) glDeleteTextures(1, &entry.textureID);
    }
    textureCache.clear();
    currentRamUsage = 0;
    UpdateCacheManager();
}

void Navigate(int direction) {
    if (imageFiles.empty()) return;
    currentIndex += direction;
    if (currentIndex >= imageFiles.size()) currentIndex = (loopBehavior == 0) ? 0 : (int)imageFiles.size() - 1;
    else if (currentIndex < 0) currentIndex = (loopBehavior == 0) ? (int)imageFiles.size() - 1 : 0;
    UpdateCacheManager();
}

bool MoveToRecycleBin(const fs::path& filePath) {
    std::wstring pathStr = filePath.wstring();
    pathStr.push_back(L'\0'); 
    SHFILEOPSTRUCTW fileOp = {0};
    fileOp.wFunc = FO_DELETE;
    fileOp.pFrom = pathStr.c_str();
    fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&fileOp) == 0;
}

// THE NATIVE WINDOWS GUI ENTRY POINT
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 1. Force clear any garbage error codes left by the Windows OS boot sequence
    SetLastError(0); 

    // 2. Safely create a local mutex
    HANDLE hMutex = CreateMutexA(NULL, FALSE, "Local\\FastImageViewer_Mutex");
    if (hMutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0; // ONLY exit if we genuinely proved another instance owns this exact mutex
    }

    // 3. Resume normal OS error suppression
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    
    // --- THE MASTER SAFETY NET ---
    try {
        DetectInstalledCodecs();

        std::error_code ec;
        fs::path targetPath;
        
        // THE REAL FIX: KERNEL PARSER RETURNED (No more NULL pointers)
        int wargc;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        
        if (wargv != NULL && wargc > 1) {
            targetPath = fs::absolute(fs::path(wargv[1]), ec);
        } else {
            targetPath = fs::current_path(ec); 
        }
        
        if (wargv != NULL) LocalFree(wargv);

        // Safely resolve the working directory
        if (fs::is_directory(targetPath, ec)) {
            targetDirectory = targetPath;
        } else if (targetPath.has_parent_path()) {
            targetDirectory = targetPath.parent_path();
        } else {
            targetDirectory = fs::current_path(ec);
        }

        // Double check existence to prevent silent C++ aborts
        if (!fs::exists(targetDirectory, ec)) targetDirectory = fs::current_path(ec);

        RefreshFileList();
        
        // Hardware-level path matching
        if (!fs::is_directory(targetPath, ec)) {
            for (size_t i = 0; i < imageFiles.size(); i++) {
                if (fs::equivalent(imageFiles[i].path, targetPath, ec)) { 
                    currentIndex = (int)i; 
                    break; 
                }
            }
            UpdateCacheManager();
        }

        if (imageFiles.empty() && closeOnEmpty) return -1;

    } catch (const std::exception& e) {
        MessageBoxA(NULL, e.what(), "FATAL C++ EXCEPTION", MB_OK | MB_ICONERROR);
        return -1;
    } catch (...) {
        MessageBoxA(NULL, "An unknown error occurred during boot.", "FATAL C++ EXCEPTION", MB_OK | MB_ICONERROR);
        return -1;
    }

    if (!glfwInit()) {
        MessageBoxA(NULL, "GLFW Failed to Initialize! Check if glfw3.dll is in the folder.", "FATAL CRASH", MB_OK | MB_ICONERROR);
        return -1;
    }
    
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "FastImageViewer", NULL, NULL);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;

    HWND hwnd = glfwGetWin32Window(window);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWORD backdrop = DWMSBT_MAINWINDOW;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

    dirWatcherThread = std::thread(WatchDirectoryBackground, targetDirectory.wstring());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (dirNeedsRefresh) {
            dirNeedsRefresh = false;
            RefreshFileList();
        }

        for (auto it = loadingFutures.begin(); it != loadingFutures.end(); ) {
            if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                RawImageData data = it->get();
                if (textureCache.find(data.index) != textureCache.end()) {
                    if (data.success) {
                        GLuint tex;
                        glGenTextures(1, &tex);
                        glBindTexture(GL_TEXTURE_2D, tex);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, data.width, data.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data.pixels.data());
                        
                        size_t byteSize = data.width * data.height * 4;
                        textureCache[data.index] = {tex, data.width, data.height, byteSize, CacheStatus::Ready};
                        currentRamUsage += byteSize;
                    } else {
                        textureCache[data.index].status = CacheStatus::Error;
                    }
                }
                it = loadingFutures.erase(it);
            } else {
                ++it;
            }
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) glfwSetWindowShouldClose(window, GLFW_TRUE); 
        
        if (!imageFiles.empty()) {
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) Navigate(1);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) Navigate(-1);
            
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                if (MoveToRecycleBin(imageFiles[currentIndex].path)) {}
            }
            for (int key = ImGuiKey_1; key <= ImGuiKey_9; ++key) {
                if (ImGui::IsKeyPressed((ImGuiKey)key)) {
                    std::string folderName = std::to_string(key - ImGuiKey_1 + 1);
                    fs::path targetDir = imageFiles[currentIndex].path.parent_path() / folderName;
                    fs::create_directories(targetDir);
                    fs::rename(imageFiles[currentIndex].path, targetDir / imageFiles[currentIndex].path.filename());
                }
            }
        }
        
        if (ImGui::IsKeyPressed(ImGuiKey_O)) showOverlay = !showOverlay;
        if (ImGui::IsKeyPressed(ImGuiKey_S)) showSettings = !showSettings;
        if (ImGui::IsKeyPressed(ImGuiKey_B)) enableSidebar = !enableSidebar;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        float sidebarWidth = enableSidebar ? (io.DisplaySize.x * sidebarWidthMultiplier) : 0.0f;

        // 1. Main Image Window
        ImGui::SetNextWindowPos(ImVec2(sidebarWidth, 0));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x - sidebarWidth, io.DisplaySize.y));
        ImGuiWindowFlags imageFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus;
        
        ImGui::Begin("Image", nullptr, imageFlags);
        if (imageFiles.empty()) {
            ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - sidebarWidth)/2 - 120, io.DisplaySize.y/2));
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "NO IMAGES REMAIN IN FOLDER");
            ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - sidebarWidth)/2 - 90, io.DisplaySize.y/2 + 20));
            ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "Press 'S' for Settings");
        } 
        else if (textureCache.count(currentIndex) && textureCache[currentIndex].status == CacheStatus::Ready) {
            auto& img = textureCache[currentIndex];
            float aspect = (float)img.width / (float)img.height;
            float winAspect = (io.DisplaySize.x - sidebarWidth) / io.DisplaySize.y;
            ImVec2 drawSize = (aspect > winAspect) ? ImVec2(io.DisplaySize.x - sidebarWidth, (io.DisplaySize.x - sidebarWidth) / aspect) : ImVec2(io.DisplaySize.y * aspect, io.DisplaySize.y);
            
            ImVec2 cursor = ImGui::GetCursorPos();
            cursor.x += ((io.DisplaySize.x - sidebarWidth) - drawSize.x) * 0.5f;
            cursor.y += (io.DisplaySize.y - drawSize.y) * 0.5f;
            ImGui::SetCursorPos(cursor);
            ImGui::Image((ImTextureID)(intptr_t)img.textureID, drawSize);
        } else {
            ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - sidebarWidth)/2 - 50, io.DisplaySize.y/2));
            if (textureCache[currentIndex].status == CacheStatus::Error) ImGui::TextColored(colorError, "LOAD ERROR");
            else ImGui::TextColored(ImVec4(1,1,0,1), "DECODING...");
        }
        ImGui::End();

        // 2. Sidebar
        if (enableSidebar) {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(sidebarWidth, io.DisplaySize.y));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 0.65f));
            
            ImGui::Begin("Sidebar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
            ImGui::Text("RAM: %zu MB / %d MB", currentRamUsage / (1024*1024), cacheLimitRAM);
            ImGui::Separator();

            ImGui::BeginChild("SidebarScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_None);
            
            if (!imageFiles.empty()) {
                std::vector<int> cachedIndices;
                for (auto const& [key, val] : textureCache) cachedIndices.push_back(key);
                std::sort(cachedIndices.begin(), cachedIndices.end());

                for (int idx : cachedIndices) {
                    auto& entry = textureCache[idx];
                    auto& fileMeta = imageFiles[idx];
                    
                    ImU32 shadowColor = IM_COL32(0,0,0,0);
                    if (entry.status == CacheStatus::Ready) shadowColor = ImGui::ColorConvertFloat4ToU32(colorSuccess);
                    if (entry.status == CacheStatus::Error) shadowColor = ImGui::ColorConvertFloat4ToU32(colorError);
                    if (idx == currentIndex) shadowColor = IM_COL32(255, 255, 255, 255); 

                    ImGui::PushID(idx);
                    
                    ImVec2 p_min = ImGui::GetCursorScreenPos();
                    float itemHeight = 100.0f * sidebarHeightMultiplier;
                    float padding = 8.0f;
                    float availableWidth = ImGui::GetContentRegionAvail().x; 
                    ImVec2 p_max = ImVec2(p_min.x + availableWidth, p_min.y + itemHeight + (padding * 2));
                    
                    ImGui::GetWindowDrawList()->AddRectFilled(p_min, p_max, IM_COL32(30, 30, 30, 180));
                    ImGui::GetWindowDrawList()->AddRect(p_min, p_max, shadowColor, 0.0f, 0, idx == currentIndex ? 3.0f : 1.0f);

                    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + padding, ImGui::GetCursorPosY() + padding));
                    
                    if (entry.status == CacheStatus::Ready) {
                        float thumbAspect = (float)entry.width / (float)entry.height;
                        ImVec2 thumbSize = thumbAspect > 1.0f ? ImVec2(itemHeight, itemHeight/thumbAspect) : ImVec2(itemHeight*thumbAspect, itemHeight);
                        ImGui::Image((ImTextureID)(intptr_t)entry.textureID, thumbSize);
                    } else if (entry.status == CacheStatus::Error) {
                        ImGui::TextColored(colorError, "ERROR");
                    } else {
                        ImGui::Text("Loading...");
                    }

                    ImGui::SameLine(itemHeight + (padding * 2) + 10.0f);
                    ImGui::BeginGroup();
                    if (sbShowIndex)     ImGui::Text("Index: %d", idx + 1);
                    if (sbShowName)      ImGui::Text("Name: %s", fileMeta.filename.c_str());
                    if (sbShowExt)       ImGui::Text("Ext: %s", fileMeta.extension.c_str());
                    if (sbShowSize)      ImGui::Text("Disk: %.2f MB", fileMeta.diskSizeMB);
                    if (sbShowDim && entry.status == CacheStatus::Ready) {
                        ImGui::Text("Dim: %dx%d", entry.width, entry.height);
                    }
                    ImGui::EndGroup();
                    
                    if (idx == currentIndex) ImGui::SetScrollHereY(0.5f);

                    ImGui::SetCursorScreenPos(ImVec2(p_min.x, p_max.y + 6.0f)); 
                    ImGui::Dummy(ImVec2(0.0f, 0.0f)); 
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::End();
            ImGui::PopStyleColor();
        }

        // 3. Info Overlay
        if (showOverlay && !imageFiles.empty()) {
            const float PAD = 10.0f;
            ImVec2 window_pos;
            window_pos.x = (overlayPosition == 1 || overlayPosition == 3) ? (io.DisplaySize.x - PAD) : (sidebarWidth + PAD);
            window_pos.y = (overlayPosition == 2 || overlayPosition == 3) ? (io.DisplaySize.y - PAD) : PAD;
            ImVec2 window_pos_pivot = ImVec2((overlayPosition == 1 || overlayPosition == 3) ? 1.0f : 0.0f, (overlayPosition == 2 || overlayPosition == 3) ? 1.0f : 0.0f);

            ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);
            ImGui::SetNextWindowBgAlpha(0.6f);
            
            ImGui::Begin("Overlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
            
            auto& curFile = imageFiles[currentIndex];
            if (ovShowName)  ImGui::Text("File: %s", curFile.filename.c_str());
            if (ovShowExt)   ImGui::Text("Type: %s", curFile.extension.c_str());
            if (ovShowIndex) ImGui::Text("Index: %d / %zu", currentIndex + 1, imageFiles.size());
            if (ovShowSize)  ImGui::Text("Size: %.2f MB", curFile.diskSizeMB);
            
            if (ovShowDim && textureCache.count(currentIndex) && textureCache[currentIndex].status == CacheStatus::Ready) {
                ImGui::Text("Res: %dx%d", textureCache[currentIndex].width, textureCache[currentIndex].height);
            }
            ImGui::End();
        }

        // 4. Settings
        if (showSettings) {
            ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::Begin("Settings & Controls", &showSettings, ImGuiWindowFlags_AlwaysAutoResize);
            
            if (ImGui::CollapsingHeader("Supported File Formats", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool formatsChanged = false;
                for (auto& fmt : appFormats) {
                    if (!fmt.requiresExtension || fmt.isInstalled) {
                        if (ImGui::Checkbox(fmt.displayName.c_str(), &fmt.enabled)) formatsChanged = true;
                    } else {
                        ImGui::BeginDisabled();
                        bool dummy = false;
                        ImGui::Checkbox(fmt.displayName.c_str(), &dummy);
                        ImGui::EndDisabled();
                        
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "[Missing System Codec]");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            ImGui::SetTooltip("Click to download the official codec from the Microsoft Store");
                            if (ImGui::IsItemClicked()) {
                                ShellExecuteA(NULL, "open", fmt.storeUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
                            }
                        }
                    }
                }
                if (formatsChanged) RefreshFileList();
            }

            if (ImGui::CollapsingHeader("Interface Layout")) {
                ImGui::Checkbox("Close Application When Folder is Empty", &closeOnEmpty);
                ImGui::Separator();
                ImGui::Checkbox("Show Sidebar (Press 'B')", &enableSidebar);
                if (enableSidebar) {
                    ImGui::SliderFloat("Sidebar Width", &sidebarWidthMultiplier, 0.10f, 0.50f, "%.2f x");
                    ImGui::SliderFloat("Thumbnail Height", &sidebarHeightMultiplier, 0.50f, 2.00f, "%.2f x");
                }
                ImGui::Spacing();
                ImGui::Combo("Top Overlay Anchor", &overlayPosition, "Top-Left\0Top-Right\0Bottom-Left\0Bottom-Right\0");
            }

            if (ImGui::CollapsingHeader("Metadata Display")) {
                ImGui::Columns(3, "metaColumns", false);
                ImGui::SetColumnWidth(0, 120); ImGui::SetColumnWidth(1, 100); ImGui::SetColumnWidth(2, 100);
                ImGui::NextColumn(); ImGui::Text("Overlay"); ImGui::NextColumn(); ImGui::Text("Sidebar"); ImGui::Separator();
                ImGui::NextColumn(); ImGui::Text("File Name"); ImGui::NextColumn(); ImGui::Checkbox("##ovName", &ovShowName); ImGui::NextColumn(); ImGui::Checkbox("##sbName", &sbShowName);
                ImGui::NextColumn(); ImGui::Text("Extension"); ImGui::NextColumn(); ImGui::Checkbox("##ovExt", &ovShowExt); ImGui::NextColumn(); ImGui::Checkbox("##sbExt", &sbShowExt);
                ImGui::NextColumn(); ImGui::Text("Index"); ImGui::NextColumn(); ImGui::Checkbox("##ovIdx", &ovShowIndex); ImGui::NextColumn(); ImGui::Checkbox("##sbIdx", &sbShowIndex);
                ImGui::NextColumn(); ImGui::Text("Disk Size"); ImGui::NextColumn(); ImGui::Checkbox("##ovSize", &ovShowSize); ImGui::NextColumn(); ImGui::Checkbox("##sbSize", &sbShowSize);
                ImGui::NextColumn(); ImGui::Text("Dimensions"); ImGui::NextColumn(); ImGui::Checkbox("##ovDim", &ovShowDim); ImGui::NextColumn(); ImGui::Checkbox("##sbDim", &sbShowDim);
                ImGui::Columns(1);
            }
            
            if (ImGui::CollapsingHeader("Caching Limits")) {
                if (ImGui::RadioButton("By Image Count Loop Window", &cacheStrategy, 0)) UpdateCacheManager();
                if (ImGui::RadioButton("By Restrictive RAM Limit", &cacheStrategy, 1)) UpdateCacheManager();
                ImGui::Spacing();
                if (cacheStrategy == 0) {
                    if (ImGui::SliderInt("Total Images To Cache", &cacheLimitCount, 1, 20)) UpdateCacheManager();
                } else {
                    if (ImGui::SliderInt("Target Max RAM Budget (MB)", &cacheLimitRAM, 128, 8192)) UpdateCacheManager();
                }
            }
            ImGui::End();
        }

        ImGui::Render();
        int display_w = 0, display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    isWatching = false;
    if (dirWatcherThread.joinable()) dirWatcherThread.join();

    for (auto const& [index, entry] : textureCache) {
        if (entry.status == CacheStatus::Ready) glDeleteTextures(1, &entry.textureID);
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);

    return 0;
}