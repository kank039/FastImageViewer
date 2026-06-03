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

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace fs = std::filesystem;

// --- Win32 High Performance GPU Request ---
#ifdef _WIN32
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

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

// Settings State
int cacheAhead = 3;
int loopBehavior = 0; // 0 = Loop back, 1 = Stop

// Function to safely delete to Windows Recycle Bin
bool MoveToRecycleBin(const fs::path& filePath) {
    std::wstring pathStr = filePath.wstring();
    pathStr.push_back(L'\0'); // Double null-terminated string required by Win32 API

    SHFILEOPSTRUCTW fileOp = {0};
    fileOp.wFunc = FO_DELETE;
    fileOp.pFrom = pathStr.c_str();
    fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    
    return SHFileOperationW(&fileOp) == 0;
}

// Memory and Texture Management
void UnloadCurrentTexture() {
    if (currentImage.loaded) {
        glDeleteTextures(1, &currentImage.textureID);
        currentImage.loaded = false;
    }
}

bool LoadTexture(const fs::path& filePath) {
    UnloadCurrentTexture();
    std::string pathStr = filePath.string();
    
    int channels = 0;
    unsigned char* data = stbi_load(pathStr.c_str(), &currentImage.width, &currentImage.height, &channels, 4);
    if (!data) return false;

    glGenTextures(1, &currentImage.textureID);
    glBindTexture(GL_TEXTURE_2D, currentImage.textureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, currentImage.width, currentImage.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);

    currentImage.loaded = true;
    return true;
}

// Navigation Logic
void Navigate(int direction) {
    if (imageFiles.empty()) return;
    currentIndex += direction;
    
    if (currentIndex >= imageFiles.size()) {
        currentIndex = (loopBehavior == 0) ? 0 : imageFiles.size() - 1;
    } else if (currentIndex < 0) {
        currentIndex = (loopBehavior == 0) ? imageFiles.size() - 1 : 0;
    }
    LoadTexture(imageFiles[currentIndex]);
}

int main(int argc, char* argv[]) {
    // 1. Single Instance Lock
    HANDLE hMutex = CreateMutexA(NULL, FALSE, "Global\\KILLSWITCH_FastImageViewer_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // App is already running. 
        // Future Implementation: Send argv[1] to existing process via IPC.
        return 0; 
    }

    // 2. Parse Input (File or Folder)
    fs::path targetPath;
    if (argc > 1) {
        targetPath = fs::u8path(argv[1]);
    } else {
        std::cerr << "Usage: FastImageViewer.exe <path_to_image_or_folder>" << std::endl;
        return -1;
    }

    fs::path searchDir = fs::is_directory(targetPath) ? targetPath : targetPath.parent_path();

    // Scan Directory
    for (const auto& entry : fs::directory_iterator(searchDir)) {
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png") {
            imageFiles.push_back(entry.path());
            if (entry.path() == targetPath) {
                currentIndex = imageFiles.size() - 1;
            }
        }
    }

    if (imageFiles.empty()) return -1;

    // 3. Initialize Window System
    if (!glfwInit()) return -1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "FastImageViewer", NULL, NULL);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;

    // 4. Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    LoadTexture(imageFiles[currentIndex]);

    // 5. Render Loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // --- Keyboard Input Handling ---
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) Navigate(1);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) Navigate(-1);
        
        // Delete File
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            if (MoveToRecycleBin(imageFiles[currentIndex])) {
                imageFiles.erase(imageFiles.begin() + currentIndex);
                if (imageFiles.empty()) break; // Close if folder is empty
                Navigate(0); // Reload current index
            }
        }

        // Quick Move (Keys 1-9)
        for (int key = ImGuiKey_1; key <= ImGuiKey_9; ++key) {
            if (ImGui::IsKeyPressed((ImGuiKey)key)) {
                std::string folderName = std::to_string(key - ImGuiKey_1 + 1);
                fs::path targetDir = imageFiles[currentIndex].parent_path() / folderName;
                
                fs::create_directories(targetDir); // Creates if it doesn't exist
                fs::path newFilePath = targetDir / imageFiles[currentIndex].filename();
                
                fs::rename(imageFiles[currentIndex], newFilePath);
                
                imageFiles.erase(imageFiles.begin() + currentIndex);
                if (imageFiles.empty()) break;
                Navigate(0);
            }
        }

        if (ImGui::IsKeyPressed(ImGuiKey_O)) showOverlay = !showOverlay;
        if (ImGui::IsKeyPressed(ImGuiKey_S)) showSettings = !showSettings;

        // --- Rendering ---
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Draw Image
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
            ImGui::Image((void*)(intptr_t)currentImage.textureID, drawSize);
        }
        ImGui::End();

        // Corner Overlay
        if (showOverlay) {
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 200, 10), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.5f);
            ImGui::Begin("Overlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::Text("%s", imageFiles[currentIndex].filename().string().c_str());
            ImGui::Text("File %d of %zu", currentIndex + 1, imageFiles.size());
            ImGui::End();
        }

        // Settings Panel
        if (showSettings) {
            ImGui::Begin("Settings", &showSettings);
            MEMORYSTATUSEX memInfo;
            memInfo.dwLength = sizeof(MEMORYSTATUSEX);
            GlobalMemoryStatusEx(&memInfo);
            DWORDLONG totalPhysMem = memInfo.ullTotalPhys / (1024 * 1024);
            
            ImGui::Text("System RAM: %llu MB", totalPhysMem);
            ImGui::Separator();
            
            ImGui::SliderInt("Cache Ahead", &cacheAhead, 0, 10);
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Recommended: %d images", totalPhysMem > 16000 ? 5 : 2);
            
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