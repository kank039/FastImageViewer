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
#include "../core/Marks.h"
#include "../core/Organize.h"
#include "../core/Scheduler.h"
#include "../core/Settings.h"
#include "../platform/GifPlayer.h"
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
        bool hdr = false, colorConverted = false;
        core::Rect region;  // Detail textures: the part of the image they hold
    };
    // Animated GIF of the image on screen: one job at a time composites the
    // next frame; its RAM and VRAM are reserved from the budget while it plays.
    struct AnimState {
        GifAnimator gif;
        bool opened = false;
        bool released = false;
        uint64_t ram = 0, gpu = 0;  // reserved
    };
    struct Animation {
        uint64_t item = 0;  // file id this is for (also set when it turned out not to animate)
        std::shared_ptr<AnimState> state;
        ComPtr<ID3D11Texture2D> tex;
        bool busy = false;
        bool playing = false;
        uint64_t nextAt = 0;
        uint32_t frame = 0, frames = 0;
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
    void FlushCache();  // drop every decoded image (colour settings changed)
    // Animation
    void UpdateAnimation();
    void StopAnimation();
    void StepAnimation();
    void ReleaseAnimation(AnimState& s);
    void OnAnimationFrame(std::shared_ptr<AnimState> s, HRESULT hr, uint32_t delayMs, uint32_t index);

    // Folder & navigation
    void OpenPath(const std::wstring& path);
    void ReloadFolder();
    void ApplyDirChanges();
    void Navigate(int delta);
    void JumpTo(int index, int direction);
    void SyncCurrentIndex();  // also rebuilds the view
    // The view: folder indices in display order (filtered, sorted). Navigation,
    // tiles and the prefetch window all work on positions in this list.
    void RebuildView();
    int ViewCount() const { return int(view_.size()); }
    FileEntry& FileAt(int pos) { return folder_.Files()[size_t(view_[size_t(pos)])]; }
    const FileEntry& FileAt(int pos) const { return folder_.Files()[size_t(view_[size_t(pos)])]; }
    int PosOfPath(const std::wstring& path) const;  // -1 if not in the view
    // Session-only marks
    core::Mark MarkOf(const FileEntry& f) const;
    void SetMark(const FileEntry& f, core::Mark m);
    void EditActiveMark(core::Mark (*edit)(core::Mark, int), int arg, const char* what);
    void SetFilter(core::Filter f);
    struct BatchRequest {
        core::Selection selection = core::Selection::Picks;
        int destKey = 0;  // 1..9 destination, 0 Recycle Bin
    };
    void RunBatch(const BatchRequest& req);
    std::vector<int> SelectionIndices(core::Selection s) const;  // folder indices
    void RecycleActive();
    void MoveActive(int key);
    void Undo();
    void PushUndo(UndoOp op);
    // All files of a pair move together: if only some succeeded, put those back.
    // Returns the files that could not be put back (they become an undo entry).
    std::vector<UndoFile> RollBack(const std::vector<UndoFile>& done);
    std::wstring DestinationFor(int key) const { return DestinationIn(key, folder_.Folder()); }
    std::wstring DestinationIn(int key, const std::wstring& base) const;
    std::vector<std::wstring> DestinationDirs(const std::wstring& base) const;  // excluded from subfolder scans
    std::wstring RelToFolder(const std::wstring& path) const;  // "" if not shown from the open folder
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
    const Texture* TextureFor(uint64_t item) const;  // best available for an item (Full, then Fit)
    const Texture* ThumbFor(uint64_t item) const;    // smallest available (Thumb, then Fit, then Full)
    uint32_t ThumbBox() const;                       // thumbnail size in pixels

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
    // Organise
    void ToggleGrid();
    void UpdateBursts();
    void CompareBurst();
    void TryCompareBurst();
    void ApplyFolderOptions();  // subfolders on/off: rescan and re-watch
    void OnSortChanged();
    bool NeedsScan() const { return settings_.sortMode == int(core::SortKey::Taken); }
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
    void DrawMarksPanel();
    void DrawGrid();
    // Pick/reject flag and stars at `pos` (top-left), `size` high. Returns the width used.
    static float DrawMarkBadge(ImDrawList* dl, ImVec2 pos, float size, const core::Mark& m);
    static float MarkBadgeWidth(float size, const core::Mark& m);

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
    std::wstring displayProfile_;  // ICC profile of the current monitor

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

    bool grid_ = false;
    bool scrollGridToCurrent_ = true;
    int gridCols_ = 1;
    int gridFirst_ = 0, gridLast_ = -1;        // view positions drawn in the grid
    int sidebarFirst_ = 0, sidebarLast_ = -1;  // view positions drawn in the sidebar
    bool peaking_ = false;
    Animation anim_;
    // Detail for huge images: requested once the view has been still briefly.
    uint64_t viewChangedAt_ = 0;
    uint64_t detailCheckAt_ = 0;
    uint64_t detailItem_ = 0;
    core::DecodePlan detailPlan_;
    void ViewChanged() {
        viewChangedAt_ = NowMs();
        detailCheckAt_ = viewChangedAt_ + 200;
    }
    Texture animTex_;  // the animation's current frame, drawn instead of the still image
    std::vector<int> burst_;                   // burst id per view position, -1 none
    int burstWaitStart_ = -1;                  // Shift+C is waiting for capture times around here
    uint64_t burstWaitUntil_ = 0;
    bool sortPending_ = false;                 // date sort re-sorts once every header is read
    int scanLeft_ = 0;                         // headers still to read for the date sort

    std::vector<int> view_;   // folder indices in display order
    std::vector<int> posOf_;  // folder index -> view position (-1 hidden)
    std::unordered_map<std::wstring, core::Mark> marks_;  // by lower-case path, RAM only
    core::Filter filter_ = core::Filter::All;
    bool showMarks_ = false;
    int marksSelection_ = 0;   // core::Selection for the batch action
    int marksAction_ = 0;      // 0 Recycle Bin, 1..9 destination, 10 copy paths
    bool confirmBatch_ = false;
    bool confirmClearMarks_ = false;
    bool pendingBatchSet_ = false;
    BatchRequest pendingBatch_;

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
