#pragma once
#include <imgui.h>

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/CacheWindow.h"
#include "../core/Scheduler.h"
#include "../core/Settings.h"
#include "../platform/Folder.h"
#include "../platform/Formats.h"
#include "../platform/Gpu.h"
#include "../platform/Jobs.h"
#include "../platform/System.h"
#include "../platform/WicCodec.h"

namespace fiv {

constexpr wchar_t kWindowClass[] = L"FastImageViewer.Main";
constexpr ULONG_PTR kCopyDataOpen = 0x46495633;  // 'FIV3': another instance asks us to open a path

class App final : public core::SchedulerHost {
public:
    App();
    ~App() override;
    int Run(HINSTANCE inst, const std::wstring& initialPath);

    // core::SchedulerHost
    void StartProbe(uint64_t job, uint64_t item, int rank) override;
    void StartDecode(uint64_t job, const core::EntryKey& key, const core::DecodePlan& plan, int rank) override;
    void CancelJob(uint64_t job) override;
    void SetJobPriority(uint64_t job, int rank) override;
    void ReleaseEntry(const core::EntryKey& key) override;

private:
    struct Texture {
        ComPtr<ID3D11ShaderResourceView> srv;
        core::Size size;
    };
    struct ToastMsg {
        std::string text;
        uint64_t until = 0;
    };
    struct ViewGeometry {
        ImVec2 viewMin, viewMax;   // area available to the image
        core::Size source;         // oriented source size (0 if unknown)
        float fitZoom = 1, zoom = 1;
        ImVec2 center;             // source pixel at the view centre
    };

    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    bool HandleKey(WPARAM vk, LPARAM lp, bool sys);

    // Setup / teardown
    bool CreateMainWindow(HINSTANCE inst);
    bool InitGraphics(std::string& error);
    void InitImGui();
    void ApplyDpi(UINT dpi);
    void ApplyBackdropSetting();
    void RecreateDevice();
    void Shutdown();

    // Settings
    void LoadSettings();
    void SaveSettingsNow();
    void MarkSettingsDirty() { settingsDirtyAt_ = NowMs(); }
    void OnCacheSettingsChanged();

    // Folder & navigation
    void OpenPath(const std::wstring& path);
    void ReloadFolder();
    void ApplyDirChanges();
    void Navigate(int delta);
    void JumpTo(int index, int direction);
    void SyncCurrentIndex();
    void RecycleCurrent();
    void MoveCurrent(int folderNumber);
    void ShowOpenDialog(bool pickFolder);
    void ToggleFullscreen();
    void UpdateTitle();
    void UpdateMonitor();
    void CheckEmpty();

    // Cache
    void UpdateCache();
    void UpdateLimits();
    std::vector<core::Want> BuildWants();
    core::DecodePlan PlanFor(const FileEntry& f, core::Variant v, uint64_t maxBytes) const;
    uint64_t LiveCapForCurrent() const;
    uint64_t TransientFor(const FileEntry& f, const core::DecodePlan& plan) const;
    uint64_t PerImageCap() const;
    void Post(std::function<void()> fn);
    void DrainCompletions();
    void OnProbeResult(uint64_t job, uint64_t item, ProbeResult r, uint64_t gen);
    void OnDecodeResult(uint64_t job, core::EntryKey key, core::DecodePlan plan, DecodeResult r, uint64_t bytesRead,
                        uint64_t gen);
    const Texture* TextureFor(uint64_t item) const;  // best available for an item

    // View
    ViewGeometry Geometry() const;
    void ResetView();
    void ZoomAt(float factor, ImVec2 screen);
    void ToggleActualSize(ImVec2 screen);

    // Frame
    void Tick();
    void RenderFrame();
    void RequestFrames(int n = 3) { framesToRender_ = std::max(framesToRender_, n); }
    void Toast(std::string text);
    float SidebarWidthPx() const;
    const FileEntry* Current() const;

    // UI (AppUi.cpp)
    void DrawImage();
    void DrawEmpty();
    void DrawOverlay();
    void DrawToasts();
    void DrawSidebar();
    void DrawSettings();
    void DrawStats();
    void DrawHelp();
    void DrawCacheMap(float height);

    // --- state ---
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    bool running_ = true;
    bool inRender_ = false;
    bool suspended_ = false;  // device reset in progress
    Gpu gpu_;
    bool translucent_ = false;
    float dpiScale_ = 1.0f;
    ImGuiStyle baseStyle_;
    core::Size monitor_{1920, 1080};

    JobSystem jobs_;
    IoGate gate_;
    core::Scheduler sched_{*this};
    FolderModel folder_;
    DirWatcher watcher_;
    FormatRegistry formats_;
    StorageKind storage_ = StorageKind::Unknown;

    core::Settings settings_;
    core::MemoryCaps caps_;
    core::Limits limits_;
    core::LiveMemory live_;
    HANDLE lowMemory_ = nullptr;
    std::wstring settingsPath_;
    std::string lastSavedSettings_;
    uint64_t settingsDirtyAt_ = 0;

    int current_ = 0;
    std::wstring currentPath_;
    int direction_ = 1;
    uint64_t lastNavMs_ = 0;
    bool settleArmed_ = false;
    uint64_t recheckAt_ = 0;

    std::unordered_map<core::EntryKey, Texture, core::EntryKeyHash> textures_;
    // Textures released while a frame is being built may still be referenced by
    // its draw list: they are kept alive here until after Present().
    std::vector<ComPtr<ID3D11ShaderResourceView>> graveyard_;
    std::unordered_map<uint64_t, std::shared_ptr<std::atomic<int>>> livePriority_;  // decode job -> rank
    uint64_t deviceGen_ = 1;  // results from jobs started on an older device are discarded
    uint64_t previewItem_ = 0;
    Texture preview_;
    bool wantFull_ = false;
    uint64_t fullItem_ = 0;
    core::DecodePlan fullPlan_;

    std::mutex completionsMutex_;
    std::vector<std::function<void()>> completions_;
    std::atomic<bool> wakePosted_{false};

    enum class ZoomMode { Fit, Custom } zoomMode_ = ZoomMode::Fit;
    float zoom_ = 1.0f;
    ImVec2 center_{0, 0};
    bool dragging_ = false;
    POINT dragLast_{};

    int pendingOpen_ = 0;  // 1 file dialog, 2 folder dialog (run outside the ImGui frame)
    bool showSettings_ = false;
    bool showHelp_ = false;
    bool fullscreen_ = false;
    WINDOWPLACEMENT savedPlacement_{};
    LONG_PTR savedStyle_ = 0;
    bool scrollSidebarToCurrent_ = true;
    bool minimized_ = false;
    bool trimmed_ = false;
    uint64_t minimizedAt_ = 0;

    std::deque<ToastMsg> toasts_;
    int framesToRender_ = 3;
    uint64_t nextGovernorMs_ = 0;
    uint64_t wakeAt_ = 0;

    // Session statistics (stats panel)
    uint64_t decodes_ = 0, probes_ = 0, filesRead_ = 0, bytesRead_ = 0;
    double lastDecodeMs_ = 0;
};

}  // namespace fiv
