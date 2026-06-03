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
#include <atomic>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

#ifdef _WIN32
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

// A structure to pass raw pixels from the background thread to the main thread
struct RawImageData {
    int width = 0;
    int height = 0;
    std::vector<BYTE> pixels;
    bool success = false;
};

struct LoadedImage {
    GLuint textureID = 0;
    int width = 0;
    int height = 0;
    bool loaded = false;
};

// Application State
std::vector<fs::path> imageFiles;
int currentIndex = 0;
LoadedImage currentImage;
bool showOverlay = true;
bool showSettings = false;
int cacheAhead = 3;
int loopBehavior = 0; 

// Multi-threading state
std::future<RawImageData> futureImage;
std::atomic<bool> isDecoding{false};

bool MoveToRecycleBin(const fs::path& filePath) {
    std::wstring pathStr = filePath.wstring();
    pathStr.push_back(L'\0'); 
    SHFILEOPSTRUCTW fileOp = {0};
    fileOp.wFunc = FO_DELETE;
    fileOp.pFrom = pathStr.c_str();
    fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&fileOp) == 0;
}

void UnloadCurrentTexture() {
    if (currentImage.loaded) {
        glDeleteTextures(1, &currentImage.textureID);
        currentImage.loaded = false;
    }
}

// --- BACKGROUND THREAD WORKER ---
// This runs entirely separate from the UI, so it can never freeze the window
RawImageData DecodeImageBackground(fs::path filePath) {
    RawImageData result;
    
    // Initialize COM strictly for this background thread
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) return result;

    ComPtr<IWICImagingFactory> pFactory;
    hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (SUCCEEDED(hr)) {
        ComPtr<IWICBitmapDecoder> pDecoder;
        hr = pFactory->CreateDecoderFromFilename(filePath.wstring().c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder);
        if (SUCCEEDED(hr)) {
            ComPtr<IWICBitmapFrameDecode> pFrame;
            hr = pDecoder->GetFrame(0, &pFrame);
            if (SUCCEEDED(hr)) {
                ComPtr<IWICFormatConverter> pConverter;
                hr = pFactory->CreateFormatConverter(&pConverter);
                if (SUCCEEDED(hr)) {
                    hr = pConverter->Initialize(pFrame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
                    if (SUCCEEDED(hr)) {
                        UINT width, height;
                        pConverter->GetSize(&width, &height);
                        result.pixels.resize(width * height * 4);
                        hr = pConverter->CopyPixels(NULL, width * 4, result.pixels.size(), result.pixels.data());
                        if (SUCCEEDED(hr)) {
                            result.width = width;
                            result.height = height;
                            result.success = true;
                        }
                    }
                }
            }
        }
    }
    CoUninitialize();
    return result;
}

// Triggers the background worker
void RequestImageLoad(const fs::path& filePath) {
    if (isDecoding) return; // Prevent spamming thread creation
    isDecoding = true;
    futureImage = std::async(std::launch::async, DecodeImageBackground, filePath);
}

void Navigate(int direction) {
    if (imageFiles.empty() || isDecoding) return; // Lock navigation until decode finishes to prevent race conditions
    currentIndex += direction;
    if (currentIndex >= imageFiles.size()) currentIndex = (loopBehavior == 0) ? 0 : (int)imageFiles.size() - 1;
    else if (currentIndex < 0) currentIndex = (loopBehavior == 0) ? (int)imageFiles.size() - 1 : 0;
    
    RequestImageLoad(imageFiles[currentIndex]);
}

int main(int argc, char* argv[]) {
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

    // Start loading the first image asynchronously
    RequestImageLoad(imageFiles[currentIndex]);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // --- CHECK BACKGROUND THREAD ---
        // If the background thread is finished, upload the pixels to the GPU!
        if (isDecoding && futureImage.valid() && futureImage.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            RawImageData data = futureImage.get();
            if (data.success) {
                UnloadCurrentTexture();
                glGenTextures(1, &currentImage.textureID);
                glBindTexture(GL_TEXTURE_2D, currentImage.textureID);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, data.width, data.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data.pixels.data());
                
                currentImage.width = data.width;
                currentImage.height = data.height;
                currentImage.loaded = true;
            }
            isDecoding = false;
        }

        // --- INPUT HANDLING ---
        if (!isDecoding) { // Only allow navigation if we aren't currently loading
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) Navigate(1);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) Navigate(-1);
            
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                if (MoveToRecycleBin(imageFiles[currentIndex])) {
                    imageFiles.erase(imageFiles.begin() + currentIndex);
                    if (imageFiles.empty()) break;
                    Navigate(0);
                }
            }

            for (int key = ImGuiKey_1; key <= ImGuiKey_9; ++key) {
                if (ImGui::IsKeyPressed((ImGuiKey)key)) {
                    std::string folderName = std::to_string(key - ImGuiKey_1 + 1);
                    fs::path targetDir = imageFiles[currentIndex].parent_path() / folderName;
                    fs::create_directories(targetDir);
                    fs::path newFilePath = targetDir / imageFiles[currentIndex].filename();
                    fs::rename(imageFiles[currentIndex], newFilePath);
                    
                    imageFiles.erase(imageFiles.begin() + currentIndex);
                    if (imageFiles.empty()) break;
                    Navigate(0);
                }
            }
        }

        if (ImGui::IsKeyPressed(ImGuiKey_O)) showOverlay = !showOverlay;
        if (ImGui::IsKeyPressed(ImGuiKey_S)) showSettings = !showSettings;

        // --- RENDERING ---
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Image", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        
        if (currentImage.loaded) {
            float aspect = (float)currentImage.width / (float)currentImage.height;
            float winAspect = io.DisplaySize.x / io.DisplaySize.y;
            ImVec2 drawSize = (aspect > winAspect) ? ImVec2(io.DisplaySize.x, io.DisplaySize.x / aspect) : ImVec2(io.DisplaySize.y * aspect, io.DisplaySize.y);
            
            ImVec2 cursor = ImGui::GetCursorPos();
            cursor.x += (io.DisplaySize.x - drawSize.x) * 0.5f;
            cursor.y += (io.DisplaySize.y - drawSize.y) * 0.5f;
            ImGui::SetCursorPos(cursor);
            
            // Dim the image if we are currently loading the next one
            ImVec4 tintColor = isDecoding ? ImVec4(0.3f, 0.3f, 0.3f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            ImGui::Image((ImTextureID)(intptr_t)currentImage.textureID, drawSize, ImVec2(0, 0), ImVec2(1, 1), tintColor, ImVec4(0, 0, 0, 0));
        }
        ImGui::End();

        // Loading Spinner/Text
        if (isDecoding) {
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2 - 50, io.DisplaySize.y / 2 - 20));
            ImGui::Begin("Loading", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground);
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "DECODING...");
            ImGui::End();
        }

        if (showOverlay) {
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 200, 10), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.5f);
            ImGui::Begin("Overlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::Text("%s", imageFiles[currentIndex].filename().string().c_str());
            ImGui::Text("File %d of %zu", currentIndex + 1, imageFiles.size());
            ImGui::End();
        }

        if (showSettings) {
            ImGui::Begin("Settings", &showSettings);
            MEMORYSTATUSEX memInfo;
            memInfo.dwLength = sizeof(MEMORYSTATUSEX);
            GlobalMemoryStatusEx(&memInfo);
            DWORDLONG totalPhysMem = memInfo.ullTotalPhys / (1024 * 1024);
            ImGui::Text("System RAM: %llu MB", totalPhysMem);
            ImGui::Separator();
            ImGui::SliderInt("Cache Ahead", &cacheAhead, 0, 10);
            ImGui::RadioButton("Loop Folder", &loopBehavior, 0); ImGui::SameLine();
            ImGui::RadioButton("Stop at End", &loopBehavior, 1);
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

    UnloadCurrentTexture();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);

    return 0;
}