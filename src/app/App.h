#pragma once
#include <imgui.h>

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/CacheWindow.h"
#include "../core/Histogram.h"
#include "../core/Keys.h"
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
        std::shared_ptr<const core::Histogram> histogram;
    };
    struct Tile {
        ImVec2 min, max;  // screen rectangle
        int index = -1;   // folder index shown in it
        int slot = 0;     // 0 .. TileCount()-1
    };
    // Undo: where each file of an operation went (Recycle Bin item or new path).
    struct UndoFile {
        std::wstring original;
        std::wstring location;
        bool recycled = false;
        uint64_t size = 0, mtime = 0;  // moved file as we left it: undo refuses if it changed
    };
    struct UndoOp {
        std::vector<UndoFile> files;  // primary first
        std::string label;
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
    void Execute(core::Action a, bool repeat);
    void OnWheel(int delta, bool horizontal, bool ctrl, ImVec2 screen);
    void OnGesture(HGESTUREINFO h);

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
    void RecycleActive();
    void MoveActive(int key);
    void Undo();
    void PushUndo(UndoOp op);
    // All files of a pair move together: if only some succeeded, put those back.
    // Returns the files that could not be put back (they become an undo entry).
    std::vector<UndoFile> RollBack(const std::vector<UndoFile>& done);
    std::wstring DestinationFor(int key) const;
    std::string DestinationLabel(int key) const;
    std::wstring PickFolder();
    void ShowOpenDialog(bool pickFolder);
    void ToggleFullscreen();
    // View rotation (session only, files are never rewritten)
    int RotationOf(const FileEntry& f) const;  // quarter turns clockwise, 0..3
    void Rotate(int quarterTurns);
    // Slideshow
    void ToggleSlideshow();
    void StopSlideshow();
    // Clipboard, Explorer, editor
    void CopyActiveImage();
    void CopyActivePath();
    // Key bindings
    void RebuildKeyMap();
    void SetBindings(core::Action a, const std::vector<core::Chord>& chords);
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

    // View (single image or 2/4-up compare)
    void ViewArea(ImVec2& mn, ImVec2& mx) const;
    std::vector<Tile> Tiles() const;
    int TileCount() const;
    int VisibleTiles() const;  // tiles that actually show an image (fewer near the end without wrap-around)
    int TileIndex(int slot) const;  // folder index shown in a tile, -1 if none
    int TileAt(ImVec2 screen) const;
    int ActiveIndex() const;
    const FileEntry* Active() const;
    bool IsVisible(uint64_t item) const;
    ViewGeometry GeometryFor(int index, ImVec2 mn, ImVec2 mx) const;
    ViewGeometry Geometry() const;  // the active image
    core::Size SourceSize(const FileEntry& f) const;
    void ResetView();
    void ZoomAt(float factor, ImVec2 screen);
    void ToggleActualSize(ImVec2 screen);
    void PanBy(float dx, float dy);
    void SetCompare(int count);
    bool ZoomedIn() const;  // the active image is larger than its view (pan instead of swipe)

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
    void DrawInfo();
    void DrawHistogram(const core::Histogram& h, ImVec2 size);
    void DrawSortingTab();
    void DrawControlsTab();

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
    std::unordered_map<uint64_t, core::DecodePlan> fullPlans_;  // frozen per visible image

    std::mutex completionsMutex_;
    std::vector<std::function<void()>> completions_;
    std::atomic<bool> wakePosted_{false};

    enum class ZoomMode { Fit, Custom } zoomMode_ = ZoomMode::Fit;
    float zoom_ = 1.0f;
    ImVec2 center_{0, 0};
    bool dragging_ = false;
    bool dragMoved_ = false;
    POINT dragLast_{}, dragStart_{};

    int compareCount_ = 1;      // 1 (single), 2 or 4 images side by side
    int activeTile_ = 0;        // tile that Delete / 1-9 / info act on
    float cmpZoomRel_ = 1.0f;   // compare zoom relative to each tile's fit (synced across tiles)
    ImVec2 cmpCenter_{0.5f, 0.5f};  // compare centre in normalized image coordinates

    // Rotation chosen in this session, by lower-case path.
    std::unordered_map<std::wstring, int> rotation_;

    core::KeyMap keymap_;
    int recordingAction_ = -1;  // Settings > Controls is waiting for a key for this action

    bool slideshow_ = false;
    uint64_t slideShownAt_ = 0;  // when the current slide appeared (0 = still loading)

    bool copying_ = false;           // a clipboard copy is being prepared
    uint64_t copyJob_ = 0;
    uint64_t nextOwnJob_ = 1ull << 62;  // job ids outside the scheduler's range

    int wheelAccum_ = 0;             // touchpads send many small wheel steps
    int hwheelAccum_ = 0;
    uint64_t lastWheelNavMs_ = 0;
    POINT gestureLast_{};
    ULONGLONG gestureZoomLast_ = 0;
    int gestureSwipe_ = 0;           // horizontal travel of a pan gesture at fit zoom

    std::deque<UndoOp> undo_;   // newest at the back
    int pendingDestPick_ = 0;   // 1..9: folder picker for that key (run outside the ImGui frame)
    bool pendingEditorPick_ = false;

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
