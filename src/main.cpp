#include <glad/glad.h>
#include <GLFW/glfw3.h>
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

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

#ifdef _WIN32
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

// --- SYSTEM STATE & SETTINGS ---
std::vector<fs::path> imageFiles;
int currentIndex = 0;
bool showOverlay = true;
bool showSettings = false;
int loopBehavior = 0; 

// Sidebar & Cache Settings
bool enableSidebar = true;
float sidebarMultiplier = 0.22f; // Defaults to 22% of screen width
int cacheStrategy = 1; // 0 = Image Count, 1 = RAM Budget
int cacheLimitCount = 5; 
int cacheLimitRAM = 1024; // MB
ImVec4 colorSuccess = ImVec4(0.2f, 0.8f, 0.2f, 1.0f); 
ImVec4 colorError = ImVec4(0.8f, 0.2f, 0.2f, 1.0f); 

// --- MULTI-THREADED CACHE MANAGER ---
enum class CacheStatus { Loading, Ready, Error };

struct RawImageData {
    int index = -1;
    int width = 0;
    int height = 0;
    std::vector<BYTE> pixels;
    bool success = false;
};

struct CacheEntry {
    GLuint textureID = 0;
    int width = 0;
    int height = 0;
    size_t sizeBytes = 0;
    CacheStatus status = CacheStatus::Loading;
};

std::unordered_map<int, CacheEntry> textureCache;
std::vector<std::future<RawImageData>> loadingFutures;
size_t currentRamUsage = 0;

bool MoveToRecycleBin(const fs::path& filePath) {
    std::wstring pathStr = filePath.wstring();
    pathStr.push_back(L'\0'); 
    SHFILEOPSTRUCTW fileOp = {0};
    fileOp.wFunc = FO_DELETE;
    fileOp.pFrom = pathStr.c_str();
    fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&fileOp) == 0;
}

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
    const size_t estimatedBytesPerImage = 30 * 1024 * 1024; 

    while (true) {
        if (cacheStrategy == 0 && desiredIndices.size() >= cacheLimitCount) break;
        if (cacheStrategy == 1 && (estimatedRam + estimatedBytesPerImage) >= ramBudget) break;
        if (step > imageFiles.size()) break; 

        int forwardIndex = currentIndex + step;
        if (loopBehavior == 0) forwardIndex = forwardIndex % imageFiles.size();
        if (forwardIndex < imageFiles.size() && std::find(desiredIndices.begin(), desiredIndices.end(), forwardIndex) == desiredIndices.end()) {
            desiredIndices.push_back(forwardIndex);
            estimatedRam += textureCache.count(forwardIndex) ? textureCache[forwardIndex].sizeBytes : estimatedBytesPerImage;
        }

        int backwardIndex = currentIndex - step;
        if (loopBehavior == 0) backwardIndex = (backwardIndex + imageFiles.size()) % imageFiles.size();
        if (backwardIndex >= 0 && std::find(desiredIndices.begin(), desiredIndices.end(), backwardIndex) == desiredIndices.end()) {
            desiredIndices.push_back(backwardIndex);
            estimatedRam += textureCache.count(backwardIndex) ? textureCache[backwardIndex].sizeBytes : estimatedBytesPerImage;
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
            loadingFutures.push_back(std::async(std::launch::async, DecodeImageBackground, idx, imageFiles[idx]));
        }
    }
}

void Navigate(int direction) {
    if (imageFiles.empty()) return;
    currentIndex += direction;
    if (currentIndex >= imageFiles.size()) currentIndex = (loopBehavior == 0) ? 0 : (int)imageFiles.size() - 1;
    else if (currentIndex < 0) currentIndex = (loopBehavior == 0) ? (int)imageFiles.size() - 1 : 0;
    UpdateCacheManager();
}

int main(int argc, char* argv[]) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    HANDLE hMutex = CreateMutexA(NULL, FALSE, "Global\\KILLSWITCH_FastImageViewer_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0; 

    fs::path targetPath;
    if (argc > 1) targetPath = fs::u8path(argv[1]);
    else { std::cerr << "Usage: FastImageViewer.exe <path_to_image>" << std::endl; return -1; }

    fs::path searchDir = fs::is_directory(targetPath) ? targetPath : targetPath.parent_path();
    for (const auto& entry : fs::directory_iterator(searchDir)) {
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp") {
            imageFiles.push_back(entry.path());
            if (entry.path() == targetPath) currentIndex = imageFiles.size() - 1;
        }
    }
    if (imageFiles.empty()) return -1;

    if (!glfwInit()) return -1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "FastImageViewer", NULL, NULL);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    UpdateCacheManager();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // --- UPLOAD FINISHED TEXTURES TO GPU ---
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

        // --- INPUT HANDLING ---
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) Navigate(1);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) Navigate(-1);
        
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            if (MoveToRecycleBin(imageFiles[currentIndex])) {
                imageFiles.erase(imageFiles.begin() + currentIndex);
                if (imageFiles.empty()) break;
                textureCache.clear(); currentRamUsage = 0; 
                Navigate(0);
            }
        }
        for (int key = ImGuiKey_1; key <= ImGuiKey_9; ++key) {
            if (ImGui::IsKeyPressed((ImGuiKey)key)) {
                std::string folderName = std::to_string(key - ImGuiKey_1 + 1);
                fs::path targetDir = imageFiles[currentIndex].parent_path() / folderName;
                fs::create_directories(targetDir);
                fs::rename(imageFiles[currentIndex], targetDir / imageFiles[currentIndex].filename());
                imageFiles.erase(imageFiles.begin() + currentIndex);
                if (imageFiles.empty()) break;
                textureCache.clear(); currentRamUsage = 0;
                Navigate(0);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_O)) showOverlay = !showOverlay;
        if (ImGui::IsKeyPressed(ImGuiKey_S)) showSettings = !showSettings;
        if (ImGui::IsKeyPressed(ImGuiKey_B)) enableSidebar = !enableSidebar;

        // --- RENDERING ---
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Dynamically calculate sidebar width based on the multiplier
        float sidebarWidth = enableSidebar ? (io.DisplaySize.x * sidebarMultiplier) : 0.0f;

        // 1. Main Image
        ImGui::SetNextWindowPos(ImVec2(sidebarWidth, 0));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x - sidebarWidth, io.DisplaySize.y));
        
        // ADDED: ImGuiWindowFlags_NoBringToFrontOnFocus prevents the image from hiding the overlay when clicked
        ImGuiWindowFlags imageFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | 
                                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus;
        
        ImGui::Begin("Image", nullptr, imageFlags);
        if (textureCache.count(currentIndex) && textureCache[currentIndex].status == CacheStatus::Ready) {
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

        // 2. Sidebar (Cache Visualizer)
        if (enableSidebar) {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(sidebarWidth, io.DisplaySize.y));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 1.0f));
            ImGui::Begin("Sidebar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
            
            ImGui::Text("RAM: %zu MB / %d MB", currentRamUsage / (1024*1024), cacheLimitRAM);
            ImGui::Separator();

            std::vector<int> cachedIndices;
            for (auto const& [key, val] : textureCache) cachedIndices.push_back(key);
            std::sort(cachedIndices.begin(), cachedIndices.end());

            for (int idx : cachedIndices) {
                auto& entry = textureCache[idx];
                
                ImU32 shadowColor = IM_COL32(0,0,0,0);
                if (entry.status == CacheStatus::Ready) shadowColor = ImGui::ColorConvertFloat4ToU32(colorSuccess);
                if (entry.status == CacheStatus::Error) shadowColor = ImGui::ColorConvertFloat4ToU32(colorError);
                if (idx == currentIndex) shadowColor = IM_COL32(255, 255, 255, 255); 

                ImGui::PushID(idx);
                
                // ADDED: Tighter layout calculations
                ImVec2 p_min = ImGui::GetCursorScreenPos();
                float itemHeight = 100.0f;
                float padding = 8.0f;
                ImVec2 p_max = ImVec2(p_min.x + sidebarWidth - 15.0f, p_min.y + itemHeight + (padding * 2));
                
                ImGui::GetWindowDrawList()->AddRectFilled(p_min, p_max, IM_COL32(30,30,30,255));
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

                // File info shifted slightly left to accommodate tighter width if user scales it down
                ImGui::SameLine(itemHeight + (padding * 2) + 10.0f);
                ImGui::BeginGroup();
                ImGui::Text("ID: %d", idx);
                ImGui::Text("%.1f MB", (float)entry.sizeBytes / (1024*1024));
                ImGui::EndGroup();
                
                if (idx == currentIndex) ImGui::SetScrollHereY(0.5f);

                // ADDED: Absolute cursor positioning for perfectly tight spacing
                ImGui::SetCursorScreenPos(ImVec2(p_min.x, p_max.y + 6.0f)); 
                ImGui::Dummy(ImVec2(0.0f, 0.0f)); // Forces ImGui to register the new scroll boundary
                ImGui::PopID();
            }
            ImGui::End();
            ImGui::PopStyleColor();
        }

        // 3. Overlays & Settings
        if (showOverlay) {
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 200, 10), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.5f);
            ImGui::Begin("Overlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::Text("%s", imageFiles[currentIndex].filename().string().c_str());
            ImGui::Text("File %d of %zu", currentIndex + 1, imageFiles.size());
            ImGui::End();
        }

        if (showSettings) {
            // ADDED: Auto-Centering logic based on exact screen size
            ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            
            // ADDED: AlwaysAutoResize forces the window to wrap tightly around its controls
            ImGui::Begin("Settings", &showSettings, ImGuiWindowFlags_AlwaysAutoResize);
            
            ImGui::Checkbox("Show Sidebar (Press 'B')", &enableSidebar);
            if (enableSidebar) {
                ImGui::SliderFloat("Sidebar Width", &sidebarMultiplier, 0.1f, 0.5f, "%.2f x");
            }
            
            ImGui::Separator();
            ImGui::Text("Caching Strategy");
            if (ImGui::RadioButton("By Image Count", &cacheStrategy, 0)) UpdateCacheManager();
            if (ImGui::RadioButton("By RAM Limit", &cacheStrategy, 1)) UpdateCacheManager();
            
            if (cacheStrategy == 0) {
                if (ImGui::SliderInt("Total Images", &cacheLimitCount, 1, 20)) UpdateCacheManager();
            } else {
                if (ImGui::SliderInt("RAM Limit (MB)", &cacheLimitRAM, 128, 8192)) UpdateCacheManager();
            }
            
            ImGui::Separator();
            ImGui::ColorEdit4("Success Border", (float*)&colorSuccess, ImGuiColorEditFlags_NoInputs);
            ImGui::ColorEdit4("Error Border", (float*)&colorError, ImGuiColorEditFlags_NoInputs);
            ImGui::End();
        }

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

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