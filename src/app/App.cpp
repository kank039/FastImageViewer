#include "App.h"

#include <imgui_impl_dx11.h>
#include <shlwapi.h>
#include <imgui_impl_win32.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../core/Bc1.h"
#include "../platform/FileOps.h"
#include "../platform/Shell.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fiv {

namespace {

constexpr UINT WM_APP_WAKE = WM_APP + 1;        // worker results are waiting
constexpr UINT WM_APP_DIRCHANGED = WM_APP + 2;  // folder contents changed

constexpr uint64_t kSettleMs = 120;        // prefetch resumes this long after the last navigation
constexpr uint64_t kStableMs = 1500;       // a file changed this recently may still be being written
constexpr uint64_t kRetryMs = 2000;        // transient failures (locked file, low memory)
constexpr uint64_t kTrimAfterMs = 10000;   // release cache this long after minimizing
constexpr uint64_t kGovernorMs = 1000;     // live memory check while images are open
constexpr uint64_t kSaveDelayMs = 1500;    // settings are written once, after changes settle

bool IsTransientError(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION) ||
           hr == E_OUTOFMEMORY || hr == HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
}

}  // namespace

App::App() = default;

App::~App() { Shutdown(); }

// ---- Lifecycle ----------------------------------------------------------------

int App::Run(HINSTANCE inst, const std::wstring& initialPath) {
    inst_ = inst;
    LoadSettings();
    formats_.Detect();
    formats_.ApplyDisabled(settings_.disabledFormats);

    if (!CreateMainWindow(inst)) {
        MessageBoxW(nullptr, L"Could not create the main window.", L"FastImageViewer", MB_ICONERROR);
        return 1;
    }
    std::string error;
    if (!InitGraphics(error)) {
        MessageBoxW(hwnd_, FromUtf8(error).c_str(), L"FastImageViewer", MB_ICONERROR);
        return 1;
    }
    InitImGui();

    // One express thread for the image on screen + general workers sized to the CPU.
    const int cores = LogicalCores();
    const int general = cores >= 8 ? 3 : cores >= 4 ? 2 : 1;
    jobs_.Start(general + 1);
    sched_.SetConfig({general, 2});
    lowMemory_ = CreateMemoryResourceNotification(LowMemoryResourceNotification);

    // Restore placement (only if it is still on a connected monitor).
    WINDOWPLACEMENT wp{};
    wp.length = sizeof wp;
    GetWindowPlacement(hwnd_, &wp);
    RECT saved{settings_.winX, settings_.winY, settings_.winX + settings_.winW, settings_.winY + settings_.winH};
    if (settings_.winW > 200 && settings_.winH > 150 && MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) {
        wp.rcNormalPosition = saved;
        wp.showCmd = settings_.winMaximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        SetWindowPlacement(hwnd_, &wp);
    } else {
        ShowWindow(hwnd_, SW_SHOWNORMAL);
    }
    UpdateWindow(hwnd_);
    UpdateMonitor();
    UpdateLimits();

    if (!initialPath.empty()) OpenPath(initialPath);
    UpdateTitle();

    while (running_) {
        if (framesToRender_ <= 0 || minimized_) {
            HANDLE handles[1];
            DWORD count = 0;
            if (gpu_.BudgetEvent()) handles[count++] = gpu_.BudgetEvent();
            const uint64_t now = NowMs();
            const DWORD timeout = wakeAt_ == 0 ? INFINITE : wakeAt_ <= now ? 0 : DWORD(std::min<uint64_t>(wakeAt_ - now, 60000));
            const DWORD r = MsgWaitForMultipleObjectsEx(count, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (count && r == WAIT_OBJECT_0) nextGovernorMs_ = 0;  // the OS changed our VRAM budget
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running_ = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running_) break;
        Tick();
        if (framesToRender_ > 0 && !minimized_) {
            RenderFrame();
            --framesToRender_;
        }
    }
    Shutdown();
    return 0;
}

void App::Shutdown() {
    if (!hwnd_ && !gpu_.Alive()) return;
    StopSlideshow();
    if (settingsDirtyAt_ || hwnd_) SaveSettingsNow();
    watcher_.Stop();
    jobs_.Stop();
    {
        std::lock_guard lk(completionsMutex_);
        completions_.clear();  // drops any textures produced after shutdown began
    }
    textures_.clear();
    readbacks_.clear();
    preview_ = {};
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    gpu_.Destroy();
    if (lowMemory_) CloseHandle(lowMemory_);
    lowMemory_ = nullptr;
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(h);
    }
}

bool App::CreateMainWindow(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) return false;

    // WS_EX_NOREDIRECTIONBITMAP: no GDI surface; DirectComposition owns the client area.
    const int w = MulDiv(1280, int(GetDpiForSystem()), 96), h = MulDiv(800, int(GetDpiForSystem()), 96);
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_ACCEPTFILES, kWindowClass, L"FastImageViewer",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, inst, this);
    return hwnd_ != nullptr;
}

bool App::InitGraphics(std::string& error) {
    if (!gpu_.Create(hwnd_, error)) return false;
    caps_ = core::ComputeCaps(GatherMemoryFacts(gpu_));
    core::Validate(settings_, caps_);
    ApplyBackdropSetting();
    return true;
}

void App::InitImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini written behind the user's back
    io.LogFilename = nullptr;
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;  // arrow keys belong to navigation
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 8;
    s.FrameRounding = 4;
    s.GrabRounding = 4;
    s.WindowBorderSize = 0;
    s.Colors[ImGuiCol_WindowBg].w = 0.92f;
    baseStyle_ = s;
    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(gpu_.Device(), gpu_.Context());
    ApplyDpi(GetDpiForWindow(hwnd_));
}

void App::ApplyDpi(UINT dpi) {
    dpiScale_ = dpi ? float(dpi) / 96.0f : 1.0f;
    ImGuiStyle& style = ImGui::GetStyle();
    style = baseStyle_;
    style.ScaleAllSizes(dpiScale_);
    ImGuiIO& io = ImGui::GetIO();
    const char* font = "C:\\Windows\\Fonts\\segoeui.ttf";
    const bool haveFont = GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES;
#if IMGUI_VERSION_NUM >= 19200
    // Dynamic fonts: load once, scale per DPI.
    if (io.Fonts->Fonts.empty()) {
        if (!haveFont || !io.Fonts->AddFontFromFileTTF(font, 16.0f)) io.Fonts->AddFontDefault();
    }
    style.FontScaleDpi = dpiScale_;
#else
    io.Fonts->Clear();
    if (!haveFont || !io.Fonts->AddFontFromFileTTF(font, std::round(16.0f * dpiScale_))) {
        ImFontConfig cfg;
        cfg.SizePixels = std::round(13.0f * dpiScale_);
        io.Fonts->AddFontDefault(&cfg);
    }
    ImGui_ImplDX11_InvalidateDeviceObjects();  // font atlas is rebuilt on the next frame
#endif
    RequestFrames();
}

void App::ApplyBackdropSetting() {
    translucent_ = ApplyBackdrop(hwnd_, settings_.backdrop == int(core::Backdrop::Mica), gpu_.Composited());
    RequestFrames();
}

void App::RecreateDevice() {
    StopAnimation();
    suspended_ = true;
    ++deviceGen_;  // anything still running finishes on the old device and is discarded
    for (const Readback& rb : readbacks_) {
        const uint64_t job = rb.job, gen = rb.gen;
        const core::EntryKey key = rb.key;
        const core::DecodePlan plan = rb.plan;
        Post([this, job, key, plan, gen] {
            DecodeResult r;
            r.hr = E_ABORT;
            OnDecodeResult(job, key, plan, r, 0, gen);
        });
    }
    readbacks_.clear();
    jobs_.CancelAll();
    DrainCompletions();
    sched_.Clear();
    textures_.clear();
    preview_ = {};
    previewItem_ = 0;
    ImGui_ImplDX11_Shutdown();
    gpu_.Destroy();
    std::string error;
    if (!gpu_.Create(hwnd_, error)) {
        MessageBoxW(hwnd_, FromUtf8(error).c_str(), L"FastImageViewer", MB_ICONERROR);
        running_ = false;
        return;
    }
    ImGui_ImplDX11_Init(gpu_.Device(), gpu_.Context());
    caps_ = core::ComputeCaps(GatherMemoryFacts(gpu_));
    core::Validate(settings_, caps_);
    ApplyBackdropSetting();
    suspended_ = false;
    nextGovernorMs_ = 0;
    UpdateLimits();
    UpdateCache();
    Toast("Graphics device was reset");
}

// ---- Settings -------------------------------------------------------------------

void App::LoadSettings() {
    settingsPath_ = SettingsFilePath();
    std::string text;
    if (!settingsPath_.empty() && ReadTextFile(settingsPath_, text)) {
        core::Deserialize(text, settings_);
        lastSavedSettings_ = text;
    }
    RebuildKeyMap();
}

void App::RebuildKeyMap() { keymap_.Build(settings_.keyBindings); }

void App::SetBindings(core::Action a, const std::vector<core::Chord>& chords) {
    const core::ActionInfo& info = core::InfoFor(a);
    std::vector<core::Chord> defaults;
    core::ParseChords(info.defaults, defaults);
    if (chords == defaults)
        settings_.keyBindings.erase(info.id);
    else
        settings_.keyBindings[info.id] = core::ChordsToString(chords);
    RebuildKeyMap();
    MarkSettingsDirty();
}

void App::SaveSettingsNow() {
    settingsDirtyAt_ = 0;
    settings_.disabledFormats = formats_.DisabledNames();
    if (hwnd_ && !fullscreen_) {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof wp;
        if (GetWindowPlacement(hwnd_, &wp)) {
            settings_.winX = wp.rcNormalPosition.left;
            settings_.winY = wp.rcNormalPosition.top;
            settings_.winW = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
            settings_.winH = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
            settings_.winMaximized = wp.showCmd == SW_SHOWMAXIMIZED;
        }
    }
    const std::string text = core::Serialize(settings_);
    if (text == lastSavedSettings_ || settingsPath_.empty()) return;  // never rewrite an identical file
    if (WriteTextFileAtomic(settingsPath_, text)) lastSavedSettings_ = text;
}

void App::FlushCache() {
    sched_.Clear();
    fullPlans_.clear();
    if (inRender_ && preview_.srv) graveyard_.push_back(preview_.srv);  // may be in this frame's draw list
    preview_ = {};
    previewItem_ = 0;
    UpdateCache();
    RequestFrames();
}

// ---- Animation -------------------------------------------------------------------------

void App::ReleaseAnimation(AnimState& st) {
    if (st.released) return;
    st.released = true;
    sched_.ReleaseTransient(st.ram);
    sched_.ReleaseGpu(st.gpu);
    UpdateCache();
}

void App::StopAnimation() {
    if (anim_.state && !anim_.busy) ReleaseAnimation(*anim_.state);  // a running job releases when it reports back
    if (inRender_ && animTex_.srv) graveyard_.push_back(animTex_.srv);
    animTex_ = {};
    anim_ = {};
}

void App::UpdateAnimation() {
    const FileEntry* f = Current();
    uint64_t want = 0;
    if (f && !grid_ && compareCount_ == 1 && f->ext == L".gif" && f->probed && SUCCEEDED(f->probeHr) &&
        TextureFor(f->id))
        want = f->id;
    if (anim_.item == want) return;
    StopAnimation();
    if (!want) return;
    anim_.item = want;
    // Reserve what playing needs (file, canvas, saved canvas, frame; one texture)
    // before reading anything. Too big for the budget: it stays a still image.
    const uint64_t canvas = uint64_t(f->stored.w) * f->stored.h * 4;
    auto st = std::make_shared<AnimState>();
    st->ram = f->size + canvas * 3;
    st->gpu = canvas;
    if (st->ram + st->gpu > PerImageCap() || st->ram > sched_.TransientRoom(limits_)) return;
    sched_.ReserveTransient(st->ram);
    sched_.ReserveGpu(st->gpu);
    UpdateCache();  // makes room: lower-ranked prefetch is evicted first
    anim_.state = st;
    StepAnimation();
}

void App::StepAnimation() {
    if (!anim_.state || anim_.busy) return;
    anim_.busy = true;
    auto st = anim_.state;
    const FileEntry* f = folder_.FindById(anim_.item);
    const std::wstring path = f ? f->path : L"";
    jobs_.Submit(nextOwnJob_++, 1, [this, st, path](WorkerContext& ctx, const CancelFlag& cancel) {
        HRESULT hr = E_ABORT;
        uint32_t delay = 100, index = 0;
        if (!cancel) {
            if (!st->opened) {
                IoGateLock io(gate_, 1, cancel);
                hr = io.held() ? st->gif.Open(ctx.wic, path, st->ram) : E_ABORT;
                st->opened = true;
                if (hr == S_OK && st->gif.WorkingBytes() > st->ram) hr = E_OUTOFMEMORY;  // larger canvas than the header said
            } else {
                hr = S_OK;
            }
            if (hr == S_OK) hr = st->gif.Next(ctx.wic, delay, index);
        }
        Post([this, st, hr, delay, index] { OnAnimationFrame(st, hr, delay, index); });
    });
}

void App::OnAnimationFrame(std::shared_ptr<AnimState> st, HRESULT hr, uint32_t delayMs, uint32_t index) {
    if (st != anim_.state) {  // stopped while the job ran
        ReleaseAnimation(*st);
        return;
    }
    anim_.busy = false;
    if (hr != S_OK) {  // a single frame, too big, or broken: keep showing the still image
        ReleaseAnimation(*st);
        anim_.state.reset();
        anim_.playing = false;
        return;
    }
    const core::Size cs = st->gif.Canvas();
    if (uint64_t(cs.w) * cs.h * 4 > st->gpu) {
        StopAnimation();
        anim_.item = Current() ? Current()->id : 0;  // do not retry this file
        return;
    }
    if (!anim_.tex) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = cs.w;
        d.Height = cs.h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(gpu_.Device()->CreateTexture2D(&d, nullptr, &anim_.tex)) ||
            FAILED(gpu_.Device()->CreateShaderResourceView(anim_.tex.Get(), nullptr, &srv))) {
            anim_.tex.Reset();
            ReleaseAnimation(*st);
            anim_.state.reset();
            return;
        }
        animTex_ = {srv, cs, nullptr};
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(gpu_.Context()->Map(anim_.tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        const uint32_t* src = st->gif.Pixels();
        for (uint32_t y = 0; y < cs.h; ++y)
            std::memcpy(static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, src + size_t(y) * cs.w, size_t(cs.w) * 4);
        gpu_.Context()->Unmap(anim_.tex.Get(), 0);
    }
    anim_.playing = true;
    anim_.frame = index;
    anim_.frames = st->gif.Frames();
    anim_.nextAt = NowMs() + delayMs;
    RequestFrames(1);
}

void App::OnCacheSettingsChanged() {
    core::Validate(settings_, caps_);
    MarkSettingsDirty();
    fullPlans_.clear();  // plans depend on the budget
    nextGovernorMs_ = 0;
    UpdateLimits();
    UpdateCache();
}

// ---- Folder & navigation ----------------------------------------------------------

const FileEntry* App::Current() const {
    if (current_ < 0 || current_ >= ViewCount()) return nullptr;
    return &FileAt(current_);
}

void App::RebuildView() {
    const int n = folder_.Count();
    view_.clear();
    for (int i = 0; i < n; ++i)
        if (filter_ == core::Filter::All || core::Matches(filter_, MarkOf(folder_.Files()[size_t(i)]))) view_.push_back(i);
    const auto key = core::SortKey(settings_.sortMode);
    if (key != core::SortKey::Name || settings_.sortDescending) {
        std::vector<core::SortItem> items;
        items.reserve(view_.size());
        for (int i : view_) {
            const FileEntry& f = folder_.Files()[size_t(i)];
            items.push_back({f.takenSec, core::FileTimeToUnix(f.mtime), f.size, f.ext});
        }
        std::vector<int> sorted;
        sorted.reserve(view_.size());
        for (int o : core::SortOrder(items, key, settings_.sortDescending)) sorted.push_back(view_[size_t(o)]);
        view_.swap(sorted);
    }
    posOf_.assign(size_t(n), -1);
    for (size_t p = 0; p < view_.size(); ++p) posOf_[size_t(view_[p])] = int(p);
    UpdateBursts();
}

void App::UpdateBursts() {
    std::vector<core::BurstInput> in;
    in.reserve(view_.size());
    for (int i = 0; i < ViewCount(); ++i) {
        const FileEntry& f = FileAt(i);
        in.push_back({f.takenSec, f.photo.make + "|" + f.photo.model});
    }
    burst_ = core::FindBursts(in, settings_.burstGapSeconds);
}

void App::OnSortChanged() {
    core::Validate(settings_, caps_);
    MarkSettingsDirty();
    sortPending_ = NeedsScan();
    SyncCurrentIndex();
    UpdateCache();
    scrollSidebarToCurrent_ = scrollGridToCurrent_ = true;
    if (sortPending_ && scanLeft_ > 0) Toast("Reading capture dates of " + std::to_string(scanLeft_) + " images...");
    RequestFrames();
}

void App::ApplyFolderOptions() {
    if (folder_.Folder().empty()) return;
    watcher_.Stop();
    folder_.SetRecursive(settings_.includeSubfolders, DestinationDirs(folder_.Folder()));
    ReloadFolder();
    watcher_.Start(folder_.Folder(), hwnd_, WM_APP_DIRCHANGED, settings_.includeSubfolders);
    if (folder_.Truncated()) Toast("Stopped after " + std::to_string(FolderModel::kMaxFiles) + " images");
}

std::wstring App::RelToFolder(const std::wstring& path) const {
    const std::wstring& root = folder_.Folder();
    if (root.empty() || path.size() <= root.size() + 1) return {};
    const size_t cut = root.back() == L'\\' ? root.size() : root.size() + 1;
    if (CompareStringOrdinal(path.c_str(), int(root.size()), root.c_str(), int(root.size()), TRUE) != CSTR_EQUAL ||
        (cut > root.size() && path[root.size()] != L'\\'))
        return {};
    std::wstring rel = path.substr(cut);
    if (!settings_.includeSubfolders && rel.find(L'\\') != std::wstring::npos) return {};
    return rel;
}

int App::PosOfPath(const std::wstring& path) const {
    const int i = folder_.IndexOfPath(path);
    return i >= 0 && size_t(i) < posOf_.size() ? posOf_[size_t(i)] : -1;
}

void App::OpenPath(const std::wstring& raw) {
    std::wstring path = raw;
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') path = path.substr(1, path.size() - 2);
    const std::wstring full = FullPath(path);
    std::wstring folder, select;
    if (IsDirectory(full)) {
        folder = full;
    } else {
        folder = ParentFolder(full);
        select = full;
    }
    if (folder.empty() || !IsDirectory(folder)) {
        Toast("Cannot open " + ToUtf8(path));
        return;
    }
    const bool same = CompareStringOrdinal(folder.c_str(), -1, folder_.Folder().c_str(), -1, TRUE) == CSTR_EQUAL;
    if (!same) {
        sched_.Clear();
        textures_.clear();
        preview_ = {};
        previewItem_ = 0;
        watcher_.Stop();
    }
    folder_.SetPairing(settings_.pairRawJpeg);
    folder_.SetRecursive(settings_.includeSubfolders, DestinationDirs(folder));
    if (!folder_.Load(folder, formats_)) Toast("Could not read the folder");
    if (folder_.Truncated()) Toast("Stopped after " + std::to_string(FolderModel::kMaxFiles) + " images");
    filter_ = core::Filter::All;
    sortPending_ = NeedsScan();  // whatever was asked for must be visible
    RebuildView();
    storage_ = DetectStorage(folder);
    gate_.SetSlots(ReadersFor(storage_));
    if (!same) watcher_.Start(folder, hwnd_, WM_APP_DIRCHANGED, settings_.includeSubfolders);

    int index = 0;
    if (!select.empty()) {
        index = PosOfPath(select);
        if (index < 0) {
            index = 0;
            if (GetFileAttributesW(select.c_str()) != INVALID_FILE_ATTRIBUTES)
                Toast("That file type is turned off or not supported (see Settings > Formats)");
        }
    }
    current_ = index;
    currentPath_ = ViewCount() ? FileAt(index).path : L"";
    direction_ = 1;
    activeTile_ = 0;
    lastNavMs_ = 0;
    scrollSidebarToCurrent_ = true;
    ResetView();
    UpdateTitle();
    UpdateCache();
    RequestFrames();
}

void App::ReloadFolder() {
    if (folder_.Folder().empty()) return;
    folder_.SetPairing(settings_.pairRawJpeg);
    folder_.Load(folder_.Folder(), formats_);
    SyncCurrentIndex();
    UpdateCache();
    RequestFrames();
    CheckEmpty();
}

void App::ApplyDirChanges() {
    bool overflow = false;
    const auto names = watcher_.Take(overflow);
    if (overflow) {
        ReloadFolder();
        return;
    }
    bool changed = false;
    const uint64_t now = NowMs();
    bool reload = false;
    for (const auto& n : names) changed |= folder_.Refresh(n, formats_, now, &reload);
    if (reload) {
        ReloadFolder();
        return;
    }
    if (!changed) return;
    SyncCurrentIndex();
    UpdateCache();
    RequestFrames();
    CheckEmpty();
}

void App::CheckEmpty() {
    if (folder_.Count() == 0 && settings_.closeWhenEmpty && !folder_.Folder().empty()) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void App::SyncCurrentIndex() {
    RebuildView();
    const int n = ViewCount();
    const int i = PosOfPath(currentPath_);
    if (i >= 0) {
        current_ = i;
    } else {
        // Current file vanished (or was filtered out): whatever now sits at its position (the next file) takes over.
        current_ = n ? std::clamp(current_, 0, n - 1) : 0;
        currentPath_ = n ? FileAt(current_).path : L"";
        if (compareCount_ == 1) ResetView();
    }
    activeTile_ = std::clamp(activeTile_, 0, std::max(0, VisibleTiles() - 1));
    UpdateTitle();
}

void App::Navigate(int delta) {
    const int n = ViewCount();
    if (n == 0) return;
    int next = current_ + delta;
    if (settings_.wrapAround)
        next = ((next % n) + n) % n;
    else
        next = std::clamp(next, 0, n - 1);
    if (next != current_) JumpTo(next, delta > 0 ? 1 : -1);
}

void App::JumpTo(int index, int direction) {
    if (index < 0 || index >= ViewCount()) return;
    current_ = index;
    currentPath_ = FileAt(index).path;
    direction_ = direction < 0 ? -1 : 1;
    lastNavMs_ = NowMs();
    settleArmed_ = true;
    scrollSidebarToCurrent_ = true;
    slideShownAt_ = 0;  // a slide's time starts when it appears
    scrollGridToCurrent_ = true;
    activeTile_ = std::clamp(activeTile_, 0, std::max(0, VisibleTiles() - 1));
    if (compareCount_ == 1)
        ResetView();
    else
        wantFull_ = false;  // compare keeps its synced zoom while sliding through a burst
    UpdateTitle();
    UpdateCache();
    RequestFrames();
}

namespace {

std::string NameWithCompanions(const FileEntry& f) {
    std::string s = ToUtf8(f.name);
    bool raw = false, xmp = false;
    for (const auto& c : f.companions) {
        const size_t dot = c.find_last_of(L'.');
        const std::wstring ext = dot == std::wstring::npos ? L"" : ToLower(c.substr(dot));
        (ext == L".xmp" ? xmp : raw) = true;
    }
    if (raw) s += " +RAW";
    if (xmp) s += " +XMP";
    return s;
}

std::wstring LeafOf(const std::wstring& p) {
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

}  // namespace

void App::PushUndo(UndoOp op) {
    if (op.files.empty()) return;
    undo_.push_back(std::move(op));
    while (undo_.size() > 50) undo_.pop_front();
}

namespace {

bool StatFile(const std::wstring& path, uint64_t& size, uint64_t& mtime) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    mtime = (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    return true;
}

}  // namespace

std::vector<App::UndoFile> App::RollBack(const std::vector<UndoFile>& done) {
    std::vector<UndoFile> stuck;
    for (const auto& f : done) {
        const HRESULT hr = f.recycled ? RestoreRecycled(hwnd_, f.location, f.original) : MoveBack(hwnd_, f.location, f.original);
        if (FAILED(hr)) stuck.push_back(f);
    }
    return stuck;
}

void App::RecycleActive() {
    const FileEntry* f = Active();
    if (!f) return;
    std::vector<std::wstring> paths{f->path};
    paths.insert(paths.end(), f->companions.begin(), f->companions.end());
    const std::string label = NameWithCompanions(*f);
    const FileOpResult r = RecycleFiles(hwnd_, paths);

    std::vector<UndoFile> done;
    bool permanent = false, untracked = false, allOk = !r.files.empty();
    HRESULT firstError = S_OK;
    for (const auto& file : r.files) {
        if (FAILED(file.hr)) {
            allOk = false;
            if (SUCCEEDED(firstError)) firstError = file.hr;
            continue;
        }
        if (file.untracked)
            untracked = true;  // in the Recycle Bin, but we do not know which item
        else if (file.to.empty())
            permanent = true;  // the shell deleted it outright (after asking)
        else
            done.push_back({file.from, file.to, true});
    }
    if (!allOk) {
        // Keep pairs together: whatever did go to the bin comes back.
        UndoOp op;
        op.label = "delete " + label;
        op.files = RollBack(done);
        const bool split = !op.files.empty() || untracked || permanent;
        PushUndo(std::move(op));
        if (split) ReloadFolder();
        Toast(r.aborted ? "Delete cancelled"
                        : "Could not delete " + label + ": " + DescribeError(firstError) +
                              (split ? "  (some files of the pair were deleted)" : "  (nothing was deleted)"));
        RequestFrames();
        return;
    }
    UndoOp op;
    op.label = "delete " + label;
    op.files = std::move(done);
    PushUndo(std::move(op));
    folder_.RemovePath(paths[0]);
    SyncCurrentIndex();
    UpdateCache();
    Toast("Deleted " + label +
          (permanent ? "  (permanently)" : untracked ? "  (restore it from the Recycle Bin)" : "  \xC2\xB7  Ctrl+Z to undo"));
    CheckEmpty();
    RequestFrames();
}

// ---- Session marks ------------------------------------------------------------------

core::Mark App::MarkOf(const FileEntry& f) const {
    if (marks_.empty()) return {};
    auto it = marks_.find(ToLower(f.path));
    return it == marks_.end() ? core::Mark{} : it->second;
}

void App::SetMark(const FileEntry& f, core::Mark m) {
    if (m.empty())
        marks_.erase(ToLower(f.path));
    else
        marks_[ToLower(f.path)] = m;
}

void App::EditActiveMark(core::Mark (*edit)(core::Mark, int), int arg, const char* what) {
    const FileEntry* f = Active();
    if (!f) return;
    const std::wstring path = f->path;
    const core::Mark before = MarkOf(*f), after = edit(before, arg);
    SetMark(*f, after);
    std::string text = what;
    if (after.stars) text += "  " + std::string(size_t(after.stars), '*');
    Toast(text);
    RequestFrames();
    if (filter_ != core::Filter::All) {
        SyncCurrentIndex();  // it may have left the filtered view: the next image takes its place
        UpdateCache();
        if (PosOfPath(path) < 0) return;
    }
    // Culling flow: mark, see the next one. Not in compare mode, where the set stays put.
    if (settings_.autoAdvance && compareCount_ == 1 && !(after == before)) Navigate(+1);
}

void App::SetFilter(core::Filter f) {
    if (f == filter_) return;
    filter_ = f;
    const int before = current_;
    SyncCurrentIndex();
    if (current_ != before || PosOfPath(currentPath_) >= 0) ResetView();
    UpdateCache();
    scrollSidebarToCurrent_ = true;
    Toast(std::string("Showing: ") + core::FilterName(f) + "  (" + std::to_string(ViewCount()) + ")");
    RequestFrames();
}

std::vector<int> App::SelectionIndices(core::Selection sel) const {
    std::vector<int> out;
    if (marks_.empty() && sel != core::Selection::Unflagged) return out;
    for (int i = 0; i < folder_.Count(); ++i)
        if (core::Matches(sel, MarkOf(folder_.Files()[size_t(i)]))) out.push_back(i);
    return out;
}

void App::RunBatch(const BatchRequest& req) {
    std::vector<std::vector<std::wstring>> groups;
    for (int i : SelectionIndices(req.selection)) {
        const FileEntry& f = folder_.Files()[size_t(i)];
        std::vector<std::wstring> g{f.path};
        g.insert(g.end(), f.companions.begin(), f.companions.end());
        groups.push_back(std::move(g));
    }
    if (groups.empty()) {
        Toast("No images are marked that way");
        return;
    }
    std::wstring dest;
    std::string where;
    if (req.destKey) {
        dest = DestinationFor(req.destKey);
        where = DestinationLabel(req.destKey);
        if (dest.empty() || CompareStringOrdinal(dest.c_str(), -1, folder_.Folder().c_str(), -1, TRUE) == CSTR_EQUAL) {
            Toast("Destination " + std::to_string(req.destKey) + " is not usable from this folder");
            return;
        }
    }
    FileOpResult r;
    if (req.destKey) {
        r = MoveGroups(hwnd_, groups, dest, settings_.renameOnConflict);
    } else {
        std::vector<std::wstring> flat;
        for (const auto& g : groups) flat.insert(flat.end(), g.begin(), g.end());
        r = RecycleFiles(hwnd_, flat);
    }

    // Per group: all of it or none of it (a JPEG never leaves without its RAW).
    UndoOp op;
    const std::string count = std::to_string(groups.size()) + (groups.size() == 1 ? " image" : " images");
    op.label = req.destKey ? "move " + count + " to " + where : "delete " + count;
    int moved = 0, failed = 0;
    bool split = false, permanent = false, untracked = false;
    HRESULT firstError = S_OK;
    std::vector<std::wstring> removed;
    size_t base = 0;
    for (const auto& g : groups) {
        std::vector<UndoFile> done;
        bool ok = true;
        for (size_t k = 0; k < g.size(); ++k) {
            if (base + k >= r.files.size()) {
                ok = false;
                continue;
            }
            const OpFile& file = r.files[base + k];
            if (FAILED(file.hr)) {
                ok = false;
                if (SUCCEEDED(firstError)) firstError = file.hr;
            } else if (req.destKey) {
                UndoFile u{file.from, file.to, false};
                StatFile(file.to, u.size, u.mtime);
                done.push_back(u);
            } else if (file.untracked) {
                untracked = true;  // in the Recycle Bin, but we do not know which item
            } else if (file.to.empty()) {
                permanent = true;  // the shell deleted it outright (after asking)
            } else {
                done.push_back({file.from, file.to, true});
            }
        }
        if (ok) {
            ++moved;
            removed.push_back(g[0]);
            op.files.insert(op.files.end(), done.begin(), done.end());
        } else {
            ++failed;
            const auto stuck = RollBack(done);
            split |= !stuck.empty();
            op.files.insert(op.files.end(), stuck.begin(), stuck.end());
        }
        base += g.size();
    }
    PushUndo(std::move(op));
    for (const auto& p : removed) folder_.RemovePath(p);
    if (split || untracked || permanent) {
        ReloadFolder();
    } else {
        SyncCurrentIndex();
        UpdateCache();
    }
    std::string msg = req.destKey ? "Moved " + std::to_string(moved) + " to " + where
                                  : "Deleted " + std::to_string(moved);
    msg += moved == 1 ? " image" : " images";
    if (failed) msg += ", " + std::to_string(failed) + " failed (" + DescribeError(firstError) + ")";
    if (r.aborted && moved == 0) msg = "Cancelled";
    else if (moved) msg += permanent ? "  (some permanently)" : untracked ? "  (restore from the Recycle Bin)" : "  \xC2\xB7  Ctrl+Z undoes all of it";
    Toast(msg);
    CheckEmpty();
    RequestFrames();
}

std::vector<std::wstring> App::DestinationDirs(const std::wstring& base) const {
    std::vector<std::wstring> out;
    for (int k = 1; k <= 9; ++k)
        if (std::wstring d = DestinationIn(k, base); !d.empty()) out.push_back(std::move(d));
    return out;
}

std::wstring App::DestinationIn(int key, const std::wstring& base) const {
    const std::string& s = settings_.destinations[size_t(key - 1)];
    std::wstring d = s.empty() ? std::to_wstring(key) : FromUtf8(s);
    const DWORD need = ExpandEnvironmentStringsW(d.c_str(), nullptr, 0);
    if (need > 0) {
        std::wstring expanded(need, L'\0');
        const DWORD got = ExpandEnvironmentStringsW(d.c_str(), expanded.data(), need);
        if (got == 0 || got > need) return {};  // never act on a truncated path
        expanded.resize(got - 1);
        d = expanded;
    }
    if (PathIsRelativeW(d.c_str())) {
        if (base.empty()) return {};
        d = base + (base.back() == L'\\' ? L"" : L"\\") + d;
    }
    return FullPath(d);
}

std::string App::DestinationLabel(int key) const {
    if (settings_.destinations[size_t(key - 1)].empty()) return "folder " + std::to_string(key);
    const std::wstring leaf = LeafOf(DestinationFor(key));
    return ToUtf8(leaf.empty() ? DestinationFor(key) : leaf);
}

void App::MoveActive(int key) {
    const FileEntry* f = Active();
    if (!f) return;
    const std::wstring dest = DestinationFor(key);
    const std::string where = DestinationLabel(key);
    if (dest.empty()) {
        Toast("The folder for key " + std::to_string(key) + " is not a valid path");
        return;
    }
    if (CompareStringOrdinal(dest.c_str(), -1, folder_.Folder().c_str(), -1, TRUE) == CSTR_EQUAL) {
        Toast("Key " + std::to_string(key) + " points at this folder");
        return;
    }
    std::vector<std::wstring> paths{f->path};
    paths.insert(paths.end(), f->companions.begin(), f->companions.end());
    const std::string label = NameWithCompanions(*f);
    const std::wstring name = f->name;
    const FileOpResult r = MoveFiles(hwnd_, paths, dest, settings_.renameOnConflict);

    std::vector<UndoFile> done;
    bool allOk = !r.files.empty();
    HRESULT firstError = S_OK;
    for (const auto& file : r.files) {
        if (FAILED(file.hr)) {
            allOk = false;
            if (SUCCEEDED(firstError)) firstError = file.hr;
            continue;
        }
        UndoFile u{file.from, file.to, false};
        StatFile(file.to, u.size, u.mtime);
        done.push_back(u);
    }
    if (!allOk) {
        // Keep pairs together: whatever did move goes back.
        UndoOp op;
        op.label = "move " + label + " to " + where;
        op.files = RollBack(done);
        const bool split = !op.files.empty();
        PushUndo(std::move(op));
        if (split) ReloadFolder();
        if (r.aborted)
            Toast("Move cancelled");
        else if (firstError == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS))
            Toast(where + " already has a file named " + ToUtf8(name) + (split ? "  (part of the pair was moved)" : ""));
        else
            Toast("Could not move " + label + ": " + DescribeError(firstError) +
                  (split ? "  (part of the pair was moved; Ctrl+Z puts it back)" : "  (nothing was moved)"));
        RequestFrames();
        return;
    }
    std::string renamed;
    if (!r.files[0].to.empty() && CompareStringOrdinal(LeafOf(r.files[0].to).c_str(), -1, name.c_str(), -1, TRUE) != CSTR_EQUAL)
        renamed = " as " + ToUtf8(LeafOf(r.files[0].to));
    UndoOp op;
    op.label = "move " + label + " to " + where;
    op.files = std::move(done);
    PushUndo(std::move(op));
    folder_.RemovePath(paths[0]);
    SyncCurrentIndex();
    UpdateCache();
    Toast("Moved " + label + " to " + where + renamed);
    CheckEmpty();
    RequestFrames();
}

void App::Undo() {
    if (undo_.empty()) {
        Toast("Nothing to undo");
        return;
    }
    UndoOp op = std::move(undo_.back());
    undo_.pop_back();
    int restored = 0;
    HRESULT firstError = S_OK;
    bool changed = false;
    std::vector<UndoFile> retry;  // blocked only because the name is taken again: worth another try
    for (const auto& f : op.files) {
        HRESULT hr;
        uint64_t size = 0, mtime = 0;
        if (!f.recycled && (!StatFile(f.location, size, mtime) || size != f.size || mtime != f.mtime)) {
            changed = true;  // never move back a file that is not the one we moved
            hr = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        } else {
            hr = f.recycled ? RestoreRecycled(hwnd_, f.location, f.original) : MoveBack(hwnd_, f.location, f.original);
        }
        if (SUCCEEDED(hr)) {
            ++restored;
            continue;
        }
        if (SUCCEEDED(firstError)) firstError = hr;
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS)) retry.push_back(f);
    }
    if (restored == 0 && !retry.empty()) undo_.push_back({retry, op.label});
    // Show the restored image right away (the folder watcher would also catch it).
    const std::wstring& primary = op.files.front().original;
    if (!RelToFolder(primary).empty()) {
        for (const auto& f : op.files)
            if (const std::wstring rel = RelToFolder(f.original); !rel.empty()) folder_.Refresh(rel, formats_, 0);
        RebuildView();
        const int i = PosOfPath(primary);
        if (i >= 0) {
            current_ = std::max(0, i - activeTile_);
            JumpTo(current_, direction_);
        } else {
            SyncCurrentIndex();
            UpdateCache();
        }
    }
    if (restored == int(op.files.size()))
        Toast("Undone: " + op.label);
    else if (restored > 0)
        Toast("Partly undone: " + op.label + " (" + DescribeError(firstError) + ")");
    else
        Toast("Could not undo " + op.label + ": " +
              (changed ? std::string("the moved file was changed or moved again since")
               : firstError == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS)
                   ? std::string("a file with that name is back in place (still undoable once it is gone)")
                   : DescribeError(firstError)));
    RequestFrames();
}

std::wstring App::PickFolder() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS);
    std::wstring out;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (SUCCEEDED(dlg->Show(hwnd_)) && SUCCEEDED(dlg->GetResult(&item)) &&
        SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        out = path;
    CoTaskMemFree(path);
    return out;
}

void App::ShowOpenDialog(bool pickFolder) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (pickFolder ? DWORD(FOS_PICKFOLDERS) : 0u));
    std::wstring spec;
    if (!pickFolder) {
        for (const auto& fmt : formats_.All()) {
            if (!fmt.enabled) continue;
            for (const auto& e : fmt.extensions) spec += (spec.empty() ? L"*" : L";*") + e;
        }
        COMDLG_FILTERSPEC filters[] = {{L"Images", spec.c_str()}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(spec.empty() ? 0 : 2, filters + (spec.empty() ? 1 : 0));
        if (spec.empty()) dlg->SetFileTypes(1, filters + 1);
    }
    if (FAILED(dlg->Show(hwnd_))) return;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        OpenPath(path);
        CoTaskMemFree(path);
    }
}

void App::ToggleFullscreen() {
    if (!fullscreen_) {
        savedPlacement_.length = sizeof savedPlacement_;
        GetWindowPlacement(hwnd_, &savedPlacement_);
        savedStyle_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (savedStyle_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        fullscreen_ = true;
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, savedStyle_);
        SetWindowPlacement(hwnd_, &savedPlacement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = false;
    }
    ResetView();
    RequestFrames();
}

// ---- Rotation, slideshow, clipboard ------------------------------------------------

int App::RotationOf(const FileEntry& f) const {
    if (rotation_.empty()) return 0;
    auto it = rotation_.find(ToLower(f.path));
    return it == rotation_.end() ? 0 : it->second;
}

void App::Rotate(int quarterTurns) {
    const FileEntry* f = Active();
    if (!f) return;
    const int r = (RotationOf(*f) + quarterTurns) & 3;
    if (r == 0)
        rotation_.erase(ToLower(f->path));
    else
        rotation_[ToLower(f->path)] = r;
    if (compareCount_ == 1) ResetView();
    static const char* kAngles[] = {"0\xC2\xB0", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0"};
    Toast(std::string("Rotated to ") + kAngles[r] + "  \xC2\xB7  view only, the file is not changed");
    RequestFrames();
}

void App::ToggleSlideshow() {
    if (slideshow_) {
        StopSlideshow();
        Toast("Slideshow stopped");
        return;
    }
    if (ViewCount() < 2) return;
    if (compareCount_ > 1) SetCompare(1);
    slideshow_ = true;
    slideShownAt_ = 0;
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);  // no screen saver while it runs
    Toast("Slideshow: " + std::to_string(settings_.slideshowSeconds) + " s per image  \xC2\xB7  L or Esc stops");
}

void App::StopSlideshow() {
    if (!slideshow_) return;
    slideshow_ = false;
    SetThreadExecutionState(ES_CONTINUOUS);
    RequestFrames();
}

void App::CopyActivePath() {
    const FileEntry* f = Active();
    if (!f) return;
    Toast(CopyTextToClipboard(hwnd_, f->path) ? "Copied the file path" : "Could not open the clipboard");
}

void App::CopyActiveImage() {
    const FileEntry* f = Active();
    if (!f) return;
    if (copying_) {
        Toast("Still copying the previous image...");
        return;
    }
    // Three buffers of the image exist at once (decoded, rotated, clipboard copy):
    // all of it is reserved from the decode budget first, so the cache makes room.
    // Larger images are scaled down to fit (at most 256 MB, a 64-megapixel image).
    const uint64_t need = f->probed && SUCCEEDED(f->probeHr) ? uint64_t(f->stored.w) * f->stored.h * 4 : 0;
    uint64_t perBuffer = std::min<uint64_t>(sched_.TransientRoom(limits_) / 3, 256 * core::kMiB);
    if (need) perBuffer = std::min(perBuffer, need);
    if (perBuffer < std::min<uint64_t>(need ? need : 4 * core::kMiB, 4 * core::kMiB)) {
        Toast("Not enough free memory to copy the image right now");
        return;
    }
    const uint64_t reserve = perBuffer * 3;
    sched_.ReserveTransient(reserve);
    copying_ = true;
    const uint64_t job = nextOwnJob_++;
    copyJob_ = job;
    const std::wstring path = f->path;
    const int orientation = f->orientation, rotation = RotationOf(*f);
    const std::string name = ToUtf8(f->name);
    jobs_.Submit(job, 2, [this, path, orientation, rotation, perBuffer, reserve, name](WorkerContext& ctx,
                                                                                         const CancelFlag& cancel) {
        ClipboardImage img;
        img.hr = E_ABORT;
        if (!cancel) {
            IoGateLock io(gate_, 2, cancel);
            if (io.held()) img = BuildClipboardImage(ctx.wic, path, orientation, rotation, perBuffer);
        }
        Post([this, img, reserve, name]() mutable {
            sched_.ReleaseTransient(reserve);
            copying_ = false;
            if (SUCCEEDED(img.hr)) {
                const uint32_t w = img.width, h = img.height;
                const bool scaled = img.scaled;
                if (PutClipboardImage(hwnd_, img))
                    Toast("Copied " + name + " (" + std::to_string(w) + " x " + std::to_string(h) +
                          (scaled ? ", reduced to fit in memory)" : ")"));
                else
                    Toast("Could not open the clipboard");
            } else {
                FreeClipboardImage(img);
                if (img.hr != E_ABORT) Toast("Could not copy " + name + ": " + DescribeError(img.hr));
            }
            UpdateCache();
        });
    });
}

void App::UpdateTitle() {
    std::wstring title = L"FastImageViewer";
    if (const FileEntry* f = Active())
        title = f->name + L"  \u2014  " + std::to_wstring(ActiveIndex() + 1) + L" / " + std::to_wstring(ViewCount()) +
                L"  \u2014  FastImageViewer";
    SetWindowTextW(hwnd_, title.c_str());
}

void App::UpdateMonitor() {
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) return;
    const core::Size m{uint32_t(mi.rcMonitor.right - mi.rcMonitor.left), uint32_t(mi.rcMonitor.bottom - mi.rcMonitor.top)};
    if (m != monitor_ && !m.empty()) {
        monitor_ = m;  // decode size follows the monitor, not the window: resizing never re-decodes
        fullPlans_.clear();
        UpdateCache();
    }
    const std::wstring profile = DisplayProfilePath(hwnd_);
    if (profile != displayProfile_) {
        displayProfile_ = profile;
        if (settings_.colorMode == 2) FlushCache();  // colours were converted for the other screen
    }
}

// ---- Cache --------------------------------------------------------------------------

uint64_t App::PerImageCap() const {
    const uint64_t ram = core::EffectiveRamSetting(settings_, caps_);
    if (caps_.unified) return ram / 4;
    // Decoding needs about twice the texture size in RAM, so one image may use
    // at most a third of the decode pool and never more than the texture pool.
    return std::min(core::EffectiveVramSetting(settings_, caps_), ram / 3);
}

// The image on screen must always be admissible, even when the live pool has
// shrunk below the configured budget (low memory, a game holding the VRAM).
uint64_t App::LiveCapForCurrent() const {
    const uint64_t pool = limits_.unified ? limits_.ram / 2 : limits_.gpu;
    return pool ? std::min(PerImageCap(), pool) : PerImageCap();
}

uint32_t App::ThumbBox() const {
    // One size for the grid and the sidebar, so switching never re-decodes.
    const float px = float(std::max(settings_.gridSize, settings_.thumbHeight)) * dpiScale_;
    return std::max(64u, (uint32_t(px) + 31) / 32 * 32);
}

core::DecodePlan App::PlanFor(const FileEntry& f, core::Variant v, uint64_t maxBytes) const {
    core::PlanRequest r;
    r.source = f.Oriented();
    const bool fit = v == core::Variant::Fit && settings_.decodeMode == int(core::DecodeMode::FitScreen);
    if (fit) r.target = monitor_;
    if (v == core::Variant::Thumb) r.target = {ThumbBox(), ThumbBox()};
    r.maxTextureDim = gpu_.MaxTextureDim();
    r.maxGpuBytes = maxBytes;
    return core::PlanDecode(r);
}

uint64_t App::TransientFor(const FileEntry& f, const core::DecodePlan& plan) const {
    core::TransientRequest t;
    t.fileBytes = f.size;
    t.readIntoMemory = f.size <= core::kMaxInMemoryFile;
    t.stored = f.stored;
    t.canPrescale = f.canPrescale && plan.region.empty();
    t.reorients = f.orientation != 1;
    t.progressive = f.progressive;
    t.plan = plan;
    return core::TransientBytes(t);
}

std::vector<core::Want> App::BuildWants() {
    std::vector<core::Want> out;
    const int n = ViewCount();
    if (n == 0) return out;
    const bool trim = minimized_ && trimmed_;

    // Priority: the visible images (1, 2 or 4 tiles) first, then their
    // full-resolution versions when zoomed in, then the prefetch window.
    const int tiles = TileCount();
    std::vector<int> visible;
    for (int t = 0; t < tiles; ++t)
        if (TileIndex(t) >= 0) visible.push_back(TileIndex(t));

    core::WindowParams p;
    p.count = n;
    p.current = current_;
    p.direction = direction_;
    p.ahead = trim ? 0 : settings_.ahead + tiles - 1;
    p.behind = trim ? 0 : settings_.behind + (tiles > 1 ? tiles - 1 : 0);
    p.wrap = settings_.wrapAround;

    const uint64_t now = NowMs();
    uint64_t recheck = 0;
    auto later = [&](uint64_t t) { recheck = recheck ? std::min(recheck, t) : t; };

    auto addWant = [&](int index, int rank, bool isVisible, int tileSlot) {
        FileEntry& f = FileAt(index);
        const bool stable = f.changedAt == 0 || now >= f.changedAt + kStableMs;
        const bool waiting = f.retryAt > now;
        // Reading an online-only file makes OneDrive download it to disk: only on request.
        const bool cloudBlocked = !isVisible && settings_.skipCloudFiles && f.CloudOnly();
        if (!stable) later(f.changedAt + kStableMs);
        if (waiting) later(f.retryAt);

        core::Want w;
        w.key = {f.id, core::Variant::Fit};
        w.rank = rank;
        w.sizeKnown = f.probed && SUCCEEDED(f.probeHr);
        w.mayProbe = stable && !waiting && !cloudBlocked && !(f.probed && FAILED(f.probeHr));
        w.mayLoad = stable && !waiting && !cloudBlocked;
        if (w.sizeKnown) {
            w.plan = PlanFor(f, core::Variant::Fit, isVisible ? LiveCapForCurrent() : PerImageCap());
            if (!w.plan.valid()) return;
            w.transientBytes = TransientFor(f, w.plan);
        }
        out.push_back(w);

        if (isVisible && wantFull_ && w.sizeKnown) {
            auto it = fullPlans_.find(f.id);
            if (it == fullPlans_.end())  // frozen per image so budget jitter never causes re-decodes
                it = fullPlans_.emplace(f.id, PlanFor(f, core::Variant::Full, LiveCapForCurrent() / 2)).first;
            if (it->second.valid() && it->second.gpuBytes > w.plan.gpuBytes) {
                core::Want full = w;
                full.key.variant = core::Variant::Full;
                full.rank = tiles + tileSlot;
                full.plan = it->second;
                full.transientBytes = TransientFor(f, it->second);
                out.push_back(full);
            }
        }
    };

    auto addThumb = [&](int index, int rank) {
        FileEntry& f = FileAt(index);
        const bool stable = f.changedAt == 0 || now >= f.changedAt + kStableMs;
        const bool waiting = f.retryAt > now;
        const bool cloudBlocked = settings_.skipCloudFiles && f.CloudOnly();
        if (!stable) later(f.changedAt + kStableMs);
        if (waiting) later(f.retryAt);
        core::Want w;
        w.key = {f.id, core::Variant::Thumb};
        w.rank = rank;
        w.sizeKnown = f.probed && SUCCEEDED(f.probeHr);
        w.mayProbe = stable && !waiting && !cloudBlocked && !(f.probed && FAILED(f.probeHr));
        w.mayLoad = stable && !waiting && !cloudBlocked;
        if (w.sizeKnown) {
            w.plan = PlanFor(f, core::Variant::Thumb, PerImageCap());
            if (!w.plan.valid()) return;
            w.transientBytes = TransientFor(f, w.plan);
        }
        out.push_back(w);
    };
    std::vector<bool> covered(size_t(n), false);  // has a screen-sized want already
    int nextRank = 0;
    if (grid_) {
        // Grid: the cells on screen, the current image (so opening it is
        // instant), then one screen of cells above and below.
        const int first = std::clamp(gridFirst_, 0, n - 1), last = std::min(gridLast_, n - 1);
        for (int i = first; i <= last; ++i) addThumb(i, nextRank++);
        if (!trim) {
            addWant(current_, nextRank++, false, 0);
            covered[size_t(current_)] = true;
            const int span = std::max(1, last - first + 1);
            for (int k = 1; k <= span; ++k) {
                if (last + k < n) addThumb(last + k, nextRank++);
                if (first - k >= 0) addThumb(first - k, nextRank++);
            }
        }
    } else {
        for (size_t t = 0; t < visible.size(); ++t) {
            addWant(visible[t], int(t), true, int(t));
            covered[size_t(visible[t])] = true;
        }
        // Compressed history: images behind you are kept as BC1 copies (an eighth
        // of the memory). The full copy stays until the compressed one exists, so
        // it is transcoded on the GPU's copy instead of being read again.
        auto addCompact = [&](int index, int rank, bool keepOnly) {
            FileEntry& f = FileAt(index);
            const core::EntryKey ck{f.id, core::Variant::Compact}, fk{f.id, core::Variant::Fit};
            if (!(f.probed && SUCCEEDED(f.probeHr)) || sched_.IsFailed(ck)) return false;
            if (keepOnly && !sched_.IsResident(ck)) return false;
            const core::DecodePlan plan = core::PlanCompact(PlanFor(f, core::Variant::Fit, PerImageCap()));
            if (!plan.valid()) return false;
            core::Want w;
            w.key = ck;
            w.rank = rank;
            w.sizeKnown = true;
            w.plan = plan;
            w.transientBytes = TransientFor(f, plan);
            w.mayLoad = f.retryAt <= now && !(settings_.skipCloudFiles && f.CloudOnly());
            out.push_back(w);
            if (!keepOnly && !sched_.IsResident(ck) && (sched_.IsResident(fk) || sched_.IsDecoding(fk)))
                addWant(index, rank, false, 0);
            return true;
        };
        if (settings_.compactHistory)  // the image on screen keeps its compressed copy until the full one is back
            for (size_t t = 0; t < visible.size(); ++t)
                if (!sched_.IsResident({FileAt(visible[t]).id, core::Variant::Fit})) addCompact(visible[t], tiles, true);
        for (const core::WindowSlot& slot : core::ComputeWindow(p)) {
            if (covered[size_t(slot.index)]) continue;
            covered[size_t(slot.index)] = true;
            const bool behind = settings_.compactHistory && slot.offset * direction_ < 0;
            if (!behind || !addCompact(slot.index, 2 * tiles + slot.rank, false)) addWant(slot.index, 2 * tiles + slot.rank, false, 0);
        }
        nextRank = 2 * tiles + p.ahead + p.behind + 1;
        // Sidebar rows outside the prefetch window get small thumbnails, after everything else.
        if (settings_.showSidebar && !trim)
            for (int i = std::max(0, sidebarFirst_); i <= std::min(sidebarLast_, n - 1); ++i)
                if (!covered[size_t(i)]) addThumb(i, nextRank++);
    }
    // Detail: zoomed past what the best texture can show on a huge image, the
    // visible area (plus a margin for panning) is decoded at 1:1.
    if (!grid_ && compareCount_ == 1 && !trim && current_ < n) {
        FileEntry& f = FileAt(current_);
        const Texture* best = TextureFor(f.id);
        const ViewGeometry g = Geometry();
        bool need = best && f.probed && SUCCEEDED(f.probeHr) && RotationOf(f) == 0 && !g.source.empty() &&
                    g.zoom > float(best->size.w) / float(g.source.w) * 1.05f;
        auto full = fullPlans_.find(f.id);
        if (need && full != fullPlans_.end() && full->second.valid() && full->second.size.w >= g.source.w)
            need = false;  // the full-resolution texture will cover it
        if (!need) {
            detailItem_ = 0;
        } else if (now < viewChangedAt_ + 200 && detailItem_ != f.id) {
            later(viewChangedAt_ + 200);  // still moving: wait until the view settles
        } else {
            const float vw = (g.viewMax.x - g.viewMin.x) / g.zoom, vh = (g.viewMax.y - g.viewMin.y) / g.zoom;
            const float x0 = std::max(0.0f, g.center.x - vw * 0.5f), y0 = std::max(0.0f, g.center.y - vh * 0.5f);
            const core::Rect vis{uint32_t(x0), uint32_t(y0), uint32_t(std::ceil(vw)) + 1, uint32_t(std::ceil(vh)) + 1};
            const bool settled = now >= viewChangedAt_ + 200;
            if (detailItem_ != f.id || (settled && !detailPlan_.region.Contains(vis))) {
                const uint32_t side = std::min(gpu_.MaxTextureDim(), std::max(monitor_.w, monitor_.h) * 2);
                detailPlan_ = core::PlanRegion(g.source, vis, 0.25f, side);
                detailItem_ = detailPlan_.valid() && detailPlan_.gpuBytes <= LiveCapForCurrent() / 2 ? f.id : 0;
            }
            if (detailItem_ == f.id) {
                core::Want w;
                w.key = {f.id, core::Variant::Detail};
                w.rank = tiles;  // with the full-resolution version, before any prefetch
                w.sizeKnown = true;
                w.plan = detailPlan_;
                w.transientBytes = TransientFor(f, detailPlan_);
                out.push_back(w);
            }
        }
    }
    // Header-only reads: capture dates for the date sort, and the neighbours
    // of the image Shift+C is waiting on. Lowest priority, never decoded.
    scanLeft_ = 0;
    const bool burstWait = burstWaitStart_ >= 0 && now < burstWaitUntil_;
    for (int i = 0; i < n && !trim; ++i) {
        FileEntry& f = FileAt(i);
        if (f.probed || f.CloudOnly()) continue;
        const bool forBurst = burstWait && std::abs(i - burstWaitStart_) <= 8;
        if (!NeedsScan() && !forBurst) continue;
        if (NeedsScan()) ++scanLeft_;
        if (covered[size_t(i)]) continue;
        core::Want w;
        w.key = {f.id, core::Variant::Fit};
        w.rank = (forBurst ? 50000 : 100000) + i;
        w.mayProbe = f.retryAt <= now && (f.changedAt == 0 || now >= f.changedAt + kStableMs);
        w.mayLoad = false;
        out.push_back(w);
    }
    // Forget frozen full-size plans of images that are no longer on screen.
    for (auto it = fullPlans_.begin(); it != fullPlans_.end();) {
        const bool shown = std::any_of(visible.begin(), visible.end(),
                                       [&](int i) { return FileAt(i).id == it->first; });
        it = shown ? std::next(it) : fullPlans_.erase(it);
    }
    std::stable_sort(out.begin(), out.end(), [](const core::Want& a, const core::Want& b) { return a.rank < b.rank; });
    recheckAt_ = recheck;
    return out;
}

void App::UpdateCache() {
    if (suspended_ || !gpu_.Alive()) return;
    const bool settled = NowMs() >= lastNavMs_ + kSettleMs;
    sched_.Pump(BuildWants(), limits_, settled);
    if (!inRender_ && gpu_.Context()) gpu_.Context()->Flush();  // released textures are freed now, not at the next frame
    if (settings_.showStats || settings_.showSidebar) RequestFrames(1);
}

void App::UpdateLimits() {
    live_ = QueryLiveMemory(gpu_, lowMemory_);
    core::OurUsage ours;
    ours.gpu = sched_.ResidentGpu();
    ours.ram = sched_.TransientInUse() + (caps_.unified ? ours.gpu : 0);
    const core::Limits l = core::ComputeLimits(core::EffectiveRamSetting(settings_, caps_),
                                               core::EffectiveVramSetting(settings_, caps_), caps_, live_, ours);
    if (l.ram != limits_.ram || l.gpu != limits_.gpu || l.unified != limits_.unified) {
        limits_ = l;
        UpdateCache();
    }
    if (settings_.showStats) RequestFrames(1);
}

void App::Post(std::function<void()> fn) {
    {
        std::lock_guard lk(completionsMutex_);
        completions_.push_back(std::move(fn));
    }
    if (!wakePosted_.exchange(true)) PostMessageW(hwnd_, WM_APP_WAKE, 0, 0);
}

void App::DrainCompletions() {
    wakePosted_ = false;
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard lk(completionsMutex_);
        batch.swap(completions_);
    }
    for (auto& fn : batch) fn();
}

void App::StartProbe(uint64_t job, uint64_t item, int rank) {
    const FileEntry* f = folder_.FindById(item);
    if (!f) {
        Post([this, job, item] { OnProbeResult(job, item, ProbeResult{E_ABORT}, deviceGen_); });
        return;
    }
    const ImageFormat* fmt = formats_.ForExtension(f->ext);
    const bool honorExif = fmt ? fmt->honorsExif : true;
    // EXIF lives at the start of JPEG and TIFF-based files (incl. most RAW): parse it ourselves.
    const bool headExif = (fmt && (fmt->name == "JPEG" || fmt->name == "TIFF")) || (formats_.IsRaw(f->ext) && f->ext != L".cr3");
    // The embedded thumbnail is only worth reading for the image on screen.
    ComPtr<ID3D11Device> thumbDevice;
    if (rank == 0 && !TextureFor(item)) thumbDevice = gpu_.Device();
    std::wstring path = f->path;
    const uint64_t gen = deviceGen_;
    jobs_.Submit(job, rank, [this, job, item, path, honorExif, headExif, thumbDevice, rank, gen](WorkerContext& ctx, const CancelFlag& cancel) {
        ProbeResult r;
        r.hr = E_ABORT;
        if (!cancel) {
            IoGateLock io(gate_, rank, cancel);
            if (io.held()) {
                BackgroundMode bg(rank > 1);
                r = ProbeImage(ctx, thumbDevice.Get(), path, honorExif, headExif);
            }
        }
        Post([this, job, item, r, gen] { OnProbeResult(job, item, r, gen); });
    });
}

void App::OnProbeResult(uint64_t job, uint64_t item, ProbeResult r, uint64_t gen) {
    sched_.OnProbeDone(job);
    if (gen != deviceGen_) r.thumb.Reset();  // texture belongs to a replaced device
    FileEntry* f = folder_.FindById(item);
    if (f && r.hr != E_ABORT) {
        ++probes_;
        if (SUCCEEDED(r.hr)) {
            f->probed = true;
            f->probeHr = S_OK;
            f->stored = r.stored;
            f->orientation = r.orientation;
            f->canPrescale = r.canPrescale;
            f->progressive = r.progressive;
            f->photo = r.photo;
            f->takenSec = core::ParseExifTime(r.photo.dateTaken);
            f->error.clear();
        } else if (IsTransientError(r.hr)) {
            f->retryAt = NowMs() + kRetryMs;
            f->error = DescribeError(r.hr);
        } else {
            f->probed = true;
            f->probeHr = r.hr;
            f->error = DescribeError(r.hr);
        }
        if (r.thumb && Current() && Current()->id == item && !TextureFor(item)) {
            preview_ = {r.thumb, r.thumbSize};
            previewItem_ = item;
        }
        if (IsVisible(item)) RequestFrames();
        UpdateBursts();
        if (settings_.showSidebar || grid_) RequestFrames(1);
    }
    UpdateCache();  // also counts what the date sort still has to read
    if (sortPending_ && scanLeft_ == 0) {
        sortPending_ = false;
        SyncCurrentIndex();
        scrollSidebarToCurrent_ = scrollGridToCurrent_ = true;
        UpdateCache();
        RequestFrames();
    }
    if (burstWaitStart_ >= 0) TryCompareBurst();
}

void App::StartDecode(uint64_t job, const core::EntryKey& key, const core::DecodePlan& plan, int rank) {
    if (key.variant == core::Variant::Compact) {
        auto fit = textures_.find({key.item, core::Variant::Fit});
        if (fit != textures_.end() && fit->second.size == plan.size && fit->second.srv) {
            StartTranscode(job, key, plan, fit->second, rank);
            return;
        }
    }
    const FileEntry* f = folder_.FindById(key.item);
    if (!f) {
        Post([this, job, key, plan] {
            DecodeResult r;
            r.hr = E_ABORT;
            OnDecodeResult(job, key, plan, r, 0, deviceGen_);
        });
        return;
    }
    DecodeRequest req;
    req.path = f->path;
    req.fileBytes = f->size;
    req.readIntoMemory = f->size <= core::kMaxInMemoryFile;
    req.stored = f->stored;
    req.orientation = f->orientation;
    req.canPrescale = f->canPrescale;
    req.plan = plan;
    req.wantSharpness = key.variant == core::Variant::Fit;
    req.colorMode = settings_.colorMode;
    req.displayProfile = displayProfile_;
    req.priority->store(rank);  // everything except the image on screen runs at background priority
    livePriority_[job] = req.priority;
    ComPtr<ID3D11Device> device = gpu_.Device();
    const uint64_t gen = deviceGen_;
    jobs_.Submit(job, rank, [this, job, key, plan, req, device, gen](WorkerContext& ctx, const CancelFlag& cancel) {
        DecodeResult r = DecodeImage(ctx, device.Get(), gate_, req, cancel);
        const uint64_t bytes = SUCCEEDED(r.hr) && req.readIntoMemory ? req.fileBytes : 0;
        Post([this, job, key, plan, r, bytes, gen] { OnDecodeResult(job, key, plan, r, bytes, gen); });
    });
}

void App::OnDecodeResult(uint64_t job, core::EntryKey key, core::DecodePlan plan, DecodeResult r, uint64_t bytesRead,
                         uint64_t gen) {
    livePriority_.erase(job);
    if (gen != deviceGen_) {  // started on a device that has since been replaced
        sched_.OnDecodeDone(job, core::Outcome::Cancelled);
        return;
    }
    core::Outcome outcome = core::Outcome::Ok;
    if (FAILED(r.hr)) {
        if (r.hr == E_ABORT)
            outcome = core::Outcome::Cancelled;
        else if (r.retry || Gpu::IsDeviceLost(r.hr))
            outcome = core::Outcome::Retry;
        else
            outcome = core::Outcome::Failed;
    }
    const bool keep = sched_.OnDecodeDone(job, outcome);
    const uint64_t curId = Current() ? Current()->id : 0;
    if (keep) {
        ImVec2 uvMax(1, 1);
        if (plan.compressed) {
            const core::Size pad = core::BlockPadded(plan.size);
            uvMax = ImVec2(float(plan.size.w) / float(pad.w), float(plan.size.h) / float(pad.h));
        }
        textures_[key] = {r.srv, plan.size, r.histogram, r.hdr, r.colorConverted, plan.region, uvMax};
        ++decodes_;
        lastDecodeMs_ = r.ms;
        if (bytesRead) {
            ++filesRead_;
            bytesRead_ += bytesRead;
        }
        if (curId == key.item) {
            preview_ = {};
            previewItem_ = 0;
        }
        if (IsVisible(key.item) || key.variant == core::Variant::Thumb) RequestFrames();
    }
    if (FileEntry* f = folder_.FindById(key.item)) {
        if (keep && r.sharpness >= 0) f->sharpness = r.sharpness;
        if (outcome == core::Outcome::Failed && key.variant != core::Variant::Compact) f->error = DescribeError(r.hr);
        if (outcome == core::Outcome::Retry) {
            f->retryAt = NowMs() + kRetryMs;
            if (r.hr == HRESULT_FROM_WIN32(ERROR_FILE_INVALID)) {
                const std::wstring name = f->name;  // Refresh may move or erase *f
                folder_.Refresh(name, formats_, NowMs());  // file changed under us: re-stat it
                SyncCurrentIndex();
            }
        }
        if (IsVisible(key.item)) RequestFrames();
    }
    if (Gpu::IsDeviceLost(r.hr) && !suspended_) {
        RecreateDevice();
        return;
    }
    UpdateCache();
    // Let the driver finish the upload now, even if no frame follows, so the
    // staging copy is freed as soon as its accounting is.
    if (gpu_.Context()) gpu_.Context()->Flush();
}

void App::CancelJob(uint64_t job) {
    jobs_.Cancel(job);
    for (auto& rb : readbacks_)
        if (rb.job == job) rb.cancelled = true;
}

void App::StartTranscode(uint64_t job, const core::EntryKey& key, const core::DecodePlan& plan, const Texture& from,
                         int rank) {
    auto fail = [&](HRESULT hr) {
        const uint64_t gen = deviceGen_;
        Post([this, job, key, plan, hr, gen] {
            DecodeResult r;
            r.hr = hr;
            r.retry = hr == E_OUTOFMEMORY;
            OnDecodeResult(job, key, plan, r, 0, gen);
        });
    };
    ComPtr<ID3D11Resource> res;
    from.srv->GetResource(&res);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = plan.size.w;
    d.Height = plan.size.h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (!res || FAILED(gpu_.Device()->CreateTexture2D(&d, nullptr, &staging))) {
        fail(E_OUTOFMEMORY);
        return;
    }
    // Level 0 only: the mips are rebuilt for the padded size anyway.
    gpu_.Context()->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, res.Get(), 0, nullptr);
    gpu_.Context()->Flush();
    readbacks_.push_back({job, key, plan, staging, from.histogram, deviceGen_, rank, false});
}

// Copies finished GPU readbacks to RAM (never waiting on the GPU) and hands
// them to a worker for BC1 compression.
void App::PumpReadbacks() {
    for (auto it = readbacks_.begin(); it != readbacks_.end();) {
        Readback& rb = *it;
        HRESULT hr = rb.cancelled ? E_ABORT : S_OK;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(hr)) {
            hr = gpu_.Context()->Map(rb.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                ++it;
                continue;
            }
        }
        std::shared_ptr<uint8_t[]> pixels;
        if (SUCCEEDED(hr)) {
            const uint32_t w = rb.plan.size.w, h = rb.plan.size.h;
            pixels.reset(new (std::nothrow) uint8_t[size_t(w) * h * 4]);
            if (pixels)
                for (uint32_t y = 0; y < h; ++y)
                    std::memcpy(pixels.get() + size_t(y) * w * 4, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch,
                                size_t(w) * 4);
            else
                hr = E_OUTOFMEMORY;
            gpu_.Context()->Unmap(rb.staging.Get(), 0);
        }
        const uint64_t job = rb.job, gen = rb.gen;
        const core::EntryKey key = rb.key;
        const core::DecodePlan plan = rb.plan;
        auto histogram = rb.histogram;
        if (FAILED(hr)) {
            Post([this, job, key, plan, hr, gen] {
                DecodeResult r;
                r.hr = hr;
                r.retry = hr == E_OUTOFMEMORY;
                OnDecodeResult(job, key, plan, r, 0, gen);
            });
        } else {
            ComPtr<ID3D11Device> device = gpu_.Device();
            jobs_.Submit(job, rb.rank, [this, job, key, plan, pixels, histogram, device, gen](WorkerContext&, const CancelFlag& cancel) {
                BackgroundMode bg(true);
                DecodeResult r = CompressImage(device.Get(), pixels.get(), plan, cancel);
                if (histogram) r.histogram = histogram;
                Post([this, job, key, plan, r, gen] { OnDecodeResult(job, key, plan, r, 0, gen); });
            });
        }
        it = readbacks_.erase(it);
    }
}

void App::SetJobPriority(uint64_t job, int rank) {
    jobs_.SetPriority(job, rank);
    auto it = livePriority_.find(job);
    if (it != livePriority_.end()) it->second->store(rank);
}

void App::ReleaseEntry(const core::EntryKey& key) {
    auto it = textures_.find(key);
    if (it == textures_.end()) return;
    if (inRender_) graveyard_.push_back(std::move(it->second.srv));
    textures_.erase(it);
}

const App::Texture* App::ThumbFor(uint64_t item) const {
    for (core::Variant v : {core::Variant::Thumb, core::Variant::Fit, core::Variant::Full, core::Variant::Compact}) {
        auto it = textures_.find({item, v});
        if (it != textures_.end()) return &it->second;
    }
    return nullptr;
}

const App::Texture* App::TextureFor(uint64_t item) const {
    auto full = textures_.find({item, core::Variant::Full});
    if (full != textures_.end()) return &full->second;
    auto fit = textures_.find({item, core::Variant::Fit});
    if (fit != textures_.end()) return &fit->second;
    auto compact = textures_.find({item, core::Variant::Compact});
    if (compact != textures_.end()) return &compact->second;
    return nullptr;
}

// ---- View -----------------------------------------------------------------------------

float App::SidebarWidthPx() const {
    return settings_.showSidebar ? std::round(float(settings_.sidebarWidth) * dpiScale_) : 0.0f;
}

void App::ViewArea(ImVec2& mn, ImVec2& mx) const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    mn = ImVec2(SidebarWidthPx(), 0);
    mx = ImVec2(float(rc.right), float(rc.bottom));
}

int App::TileCount() const { return std::max(0, std::min(compareCount_, ViewCount())); }

int App::VisibleTiles() const {
    int n = 0;
    while (n < TileCount() && TileIndex(n) >= 0) ++n;  // tiles fill from the first one
    return n;
}

int App::TileIndex(int slot) const {
    const int n = ViewCount();
    if (n == 0 || slot < 0 || slot >= TileCount()) return -1;
    int i = current_ + slot;
    if (i >= n) {
        if (!settings_.wrapAround) return -1;
        i %= n;
    }
    return i;
}

std::vector<App::Tile> App::Tiles() const {
    ImVec2 mn, mx;
    ViewArea(mn, mx);
    const int count = std::max(1, TileCount());
    const float gap = count > 1 ? std::round(4 * dpiScale_) : 0.0f;
    const float w = mx.x - mn.x, h = mx.y - mn.y;
    int cols = 1, rows = 1;
    if (count == 2) (w >= h ? cols : rows) = 2;
    if (count >= 3) cols = rows = 2;
    const float tw = (w - gap * float(cols - 1)) / float(cols), th = (h - gap * float(rows - 1)) / float(rows);
    std::vector<Tile> out;
    for (int t = 0; t < count; ++t) {
        Tile tile;
        tile.min = ImVec2(std::round(mn.x + float(t % cols) * (tw + gap)), std::round(mn.y + float(t / cols) * (th + gap)));
        tile.max = ImVec2(std::round(tile.min.x + tw), std::round(tile.min.y + th));
        tile.slot = t;
        tile.index = TileIndex(t);
        out.push_back(tile);
    }
    return out;
}

int App::TileAt(ImVec2 p) const {
    for (const Tile& t : Tiles())
        if (p.x >= t.min.x && p.x < t.max.x && p.y >= t.min.y && p.y < t.max.y) return t.slot;
    return -1;
}

int App::ActiveIndex() const {
    if (ViewCount() == 0) return -1;
    if (compareCount_ <= 1) return current_;
    return TileIndex(std::clamp(activeTile_, 0, std::max(0, VisibleTiles() - 1)));
}

const FileEntry* App::Active() const {
    const int i = ActiveIndex();
    return i >= 0 && i < ViewCount() ? &FileAt(i) : nullptr;
}

bool App::IsVisible(uint64_t item) const {
    for (int t = 0; t < TileCount(); ++t) {
        const int i = TileIndex(t);
        if (i >= 0 && FileAt(i).id == item) return true;
    }
    return false;
}

core::Size App::SourceSize(const FileEntry& f) const {
    if (f.probed && SUCCEEDED(f.probeHr)) return f.Oriented();
    if (const Texture* t = TextureFor(f.id)) return t->size;
    if (previewItem_ == f.id && preview_.srv) return preview_.size;
    return {};
}

App::ViewGeometry App::GeometryFor(int index, ImVec2 mn, ImVec2 mx) const {
    ViewGeometry g;
    g.viewMin = mn;
    g.viewMax = mx;
    if (index < 0 || index >= ViewCount()) return g;
    const FileEntry& file = FileAt(index);
    g.source = SourceSize(file);
    if (RotationOf(file) & 1) std::swap(g.source.w, g.source.h);  // view rotation
    if (g.source.empty()) return g;
    const float vw = std::max(1.0f, mx.x - mn.x), vh = std::max(1.0f, mx.y - mn.y);
    const float sw = float(g.source.w), sh = float(g.source.h);
    g.fitZoom = std::min(vw / sw, vh / sh);
    if (!settings_.upscaleSmall) g.fitZoom = std::min(g.fitZoom, 1.0f);
    ImVec2 c;
    if (compareCount_ > 1) {  // synced: same zoom relative to fit, same relative position
        g.zoom = g.fitZoom * cmpZoomRel_;
        c = ImVec2(cmpCenter_.x * sw, cmpCenter_.y * sh);
    } else {
        g.zoom = zoomMode_ == ZoomMode::Fit ? g.fitZoom : zoom_;
        c = zoomMode_ == ZoomMode::Fit ? ImVec2(sw * 0.5f, sh * 0.5f) : center_;
    }
    // Keep the image inside the view: centred when smaller, edges clamped when larger.
    const float hw = vw * 0.5f / g.zoom, hh = vh * 0.5f / g.zoom;
    c.x = sw * g.zoom <= vw ? sw * 0.5f : std::clamp(c.x, hw, sw - hw);
    c.y = sh * g.zoom <= vh ? sh * 0.5f : std::clamp(c.y, hh, sh - hh);
    g.center = c;
    return g;
}

App::ViewGeometry App::Geometry() const {
    if (compareCount_ > 1) {
        const auto tiles = Tiles();
        const Tile& t = tiles[size_t(std::clamp(activeTile_, 0, int(tiles.size()) - 1))];
        return GeometryFor(t.index, t.min, t.max);
    }
    ImVec2 mn, mx;
    ViewArea(mn, mx);
    return GeometryFor(ViewCount() ? current_ : -1, mn, mx);
}

void App::ResetView() {
    ViewChanged();
    zoomMode_ = ZoomMode::Fit;
    zoom_ = 1.0f;
    cmpZoomRel_ = 1.0f;
    cmpCenter_ = ImVec2(0.5f, 0.5f);
    wantFull_ = false;
    dragging_ = false;
}

void App::SetCompare(int count) {
    compareCount_ = count;
    activeTile_ = 0;
    ResetView();
    UpdateTitle();
    UpdateCache();
    if (count > 1)
        Toast("Compare " + std::to_string(count) + " images  \xC2\xB7  Tab or click picks the active one  \xC2\xB7  C to change");
    RequestFrames();
}

void App::ToggleGrid() {
    grid_ = !grid_;
    if (grid_) {
        StopSlideshow();
        if (compareCount_ > 1) SetCompare(1);
        scrollGridToCurrent_ = true;
        gridFirst_ = current_;
        gridLast_ = current_;  // the real range is known after the first frame
    } else {
        ResetView();
    }
    UpdateTitle();
    UpdateCache();
    RequestFrames();
}

void App::CompareBurst() {
    const int pos = ActiveIndex();
    if (pos < 0) return;
    if (grid_) ToggleGrid();
    burstWaitStart_ = pos;
    burstWaitUntil_ = NowMs() + 5000;
    TryCompareBurst();
    if (burstWaitStart_ >= 0) {
        Toast("Reading capture times...");
        UpdateCache();  // probes the neighbours
    }
}

void App::TryCompareBurst() {
    const int pos = burstWaitStart_;
    const int n = ViewCount();
    if (pos < 0 || pos >= n) {
        burstWaitStart_ = -1;
        return;
    }
    UpdateBursts();
    auto known = [&](int i) { return i < 0 || i >= n || FileAt(i).probed || FileAt(i).CloudOnly(); };
    const int id = burst_[size_t(pos)];
    if (id < 0) {
        if (!known(pos) || !known(pos - 1) || !known(pos + 1)) return;  // still reading
        burstWaitStart_ = -1;
        Toast("Not part of a burst (no other shot within " + std::to_string(settings_.burstGapSeconds) + " s)");
        return;
    }
    int start = pos, end = pos;
    while (start > 0 && burst_[size_t(start - 1)] == id) --start;
    while (end + 1 < n && burst_[size_t(end + 1)] == id) ++end;
    if (!known(start - 1) || (end - start + 1 < 4 && !known(end + 1))) return;  // it may go on
    burstWaitStart_ = -1;
    const int len = end - start + 1;
    JumpTo(start, 1);
    SetCompare(std::min(len, 4));
    activeTile_ = std::clamp(pos - start, 0, std::min(len, 4) - 1);
    UpdateTitle();
    Toast("Burst of " + std::to_string(len) + (len > 4 ? "  \xC2\xB7  PgDn for the next shots" : "") +
          "  \xC2\xB7  the sharpest is marked");
}

void App::ZoomAt(float factor, ImVec2 screen) {
    ViewChanged();
    if (compareCount_ > 1) {
        const auto tiles = Tiles();
        int s = TileAt(screen);
        if (s < 0) s = std::clamp(activeTile_, 0, int(tiles.size()) - 1);
        const Tile& t = tiles[size_t(s)];
        const ViewGeometry g = GeometryFor(t.index, t.min, t.max);
        if (g.source.empty()) return;
        const ImVec2 tc((t.min.x + t.max.x) * 0.5f, (t.min.y + t.max.y) * 0.5f);
        const ImVec2 under(g.center.x + (screen.x - tc.x) / g.zoom, g.center.y + (screen.y - tc.y) / g.zoom);
        const float rel = std::clamp(cmpZoomRel_ * factor, 1.0f, std::max(1.0f, 64.0f / g.fitZoom));
        if (rel <= 1.001f) {
            cmpZoomRel_ = 1.0f;
            cmpCenter_ = ImVec2(0.5f, 0.5f);
        } else {
            const float z = g.fitZoom * rel;
            cmpCenter_ = ImVec2((under.x - (screen.x - tc.x) / z) / float(g.source.w),
                                (under.y - (screen.y - tc.y) / z) / float(g.source.h));
            cmpZoomRel_ = rel;
        }
        RequestFrames();
        return;
    }
    ViewGeometry g = Geometry();
    if (g.source.empty()) return;
    const ImVec2 vc((g.viewMin.x + g.viewMax.x) * 0.5f, (g.viewMin.y + g.viewMax.y) * 0.5f);
    const ImVec2 under(g.center.x + (screen.x - vc.x) / g.zoom, g.center.y + (screen.y - vc.y) / g.zoom);
    const float minZoom = std::min(g.fitZoom, 1.0f) * 0.25f;
    float z = std::clamp(g.zoom * factor, minZoom, 64.0f);
    if (factor < 1 && g.zoom > g.fitZoom && z <= g.fitZoom * 1.01f) {  // zooming out snaps to fit once
        zoomMode_ = ZoomMode::Fit;
        RequestFrames();
        return;
    }
    zoom_ = z;
    center_ = ImVec2(under.x - (screen.x - vc.x) / z, under.y - (screen.y - vc.y) / z);
    zoomMode_ = ZoomMode::Custom;
    RequestFrames();
}

void App::ToggleActualSize(ImVec2 screen) {
    if (compareCount_ > 1) {
        const auto tiles = Tiles();
        int s = TileAt(screen);
        if (s < 0) s = std::clamp(activeTile_, 0, int(tiles.size()) - 1);
        const ViewGeometry g = GeometryFor(tiles[size_t(s)].index, tiles[size_t(s)].min, tiles[size_t(s)].max);
        if (g.source.empty()) return;
        if (cmpZoomRel_ > 1.001f && std::fabs(g.zoom - 1.0f) < 0.01f) {
            cmpZoomRel_ = 1.0f;
            cmpCenter_ = ImVec2(0.5f, 0.5f);
            RequestFrames();
        } else {
            ZoomAt(1.0f / g.zoom, screen);
        }
        return;
    }
    ViewGeometry g = Geometry();
    if (g.source.empty()) return;
    if (zoomMode_ == ZoomMode::Custom && std::fabs(g.zoom - 1.0f) < 0.001f) {
        zoomMode_ = ZoomMode::Fit;
        RequestFrames();
        return;
    }
    ZoomAt(1.0f / g.zoom, screen);
    zoomMode_ = ZoomMode::Custom;
    zoom_ = 1.0f;
}

void App::PanBy(float dx, float dy) {
    ViewChanged();
    if (compareCount_ > 1) {
        const auto tiles = Tiles();
        int s = TileAt(ImVec2(float(dragStart_.x), float(dragStart_.y)));
        if (s < 0) s = std::clamp(activeTile_, 0, int(tiles.size()) - 1);
        const ViewGeometry g = GeometryFor(tiles[size_t(s)].index, tiles[size_t(s)].min, tiles[size_t(s)].max);
        if (g.source.empty() || cmpZoomRel_ <= 1.001f) return;
        cmpCenter_ = ImVec2((g.center.x - dx / g.zoom) / float(g.source.w), (g.center.y - dy / g.zoom) / float(g.source.h));
        RequestFrames();
        return;
    }
    const ViewGeometry g = Geometry();
    const bool larger = float(g.source.w) * g.zoom > g.viewMax.x - g.viewMin.x + 0.5f ||
                        float(g.source.h) * g.zoom > g.viewMax.y - g.viewMin.y + 0.5f;
    if (g.source.empty() || !larger) return;  // a fitted image stays fitted (and re-fits on resize)
    center_ = ImVec2(g.center.x - dx / g.zoom, g.center.y - dy / g.zoom);
    zoom_ = g.zoom;
    zoomMode_ = ZoomMode::Custom;
    RequestFrames();
}

// ---- Frame ------------------------------------------------------------------------------

void App::Toast(std::string text) {
    toasts_.push_back({std::move(text), NowMs() + 3000});
    while (toasts_.size() > 4) toasts_.pop_front();
    RequestFrames();
}

void App::Tick() {
    if (pendingOpen_) {
        const bool folder = pendingOpen_ == 2;
        pendingOpen_ = 0;
        ShowOpenDialog(folder);
    }
    if (pendingBatchSet_) {  // shell file operations pump messages: never inside a frame
        pendingBatchSet_ = false;
        RunBatch(pendingBatch_);
    }
    if (pendingEditorPick_) {
        pendingEditorPick_ = false;
        ComPtr<IFileOpenDialog> dlg;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
            COMDLG_FILTERSPEC filters[] = {{L"Programs", L"*.exe"}};
            dlg->SetFileTypes(1, filters);
            ComPtr<IShellItem> item;
            PWSTR path = nullptr;
            if (SUCCEEDED(dlg->Show(hwnd_)) && SUCCEEDED(dlg->GetResult(&item)) &&
                SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                settings_.editorPath = ToUtf8(path);
                MarkSettingsDirty();
            }
            CoTaskMemFree(path);
            RequestFrames();
        }
    }
    if (pendingDestPick_) {
        const int key = pendingDestPick_;
        pendingDestPick_ = 0;
        const std::wstring picked = PickFolder();
        if (!picked.empty()) {
            settings_.destinations[size_t(key - 1)] = ToUtf8(picked);
            MarkSettingsDirty();
            RequestFrames();
        }
    }
    const uint64_t now = NowMs();
    if (folder_.Count() && now >= nextGovernorMs_) {
        nextGovernorMs_ = now + kGovernorMs;
        UpdateLimits();
    }
    if (settleArmed_ && now >= lastNavMs_ + kSettleMs) {
        settleArmed_ = false;
        UpdateCache();
    }
    if (recheckAt_ && now >= recheckAt_) {
        recheckAt_ = 0;
        UpdateCache();
    }
    if (minimized_ && settings_.releaseWhenMinimized && !trimmed_ && now >= minimizedAt_ + kTrimAfterMs) {
        trimmed_ = true;
        UpdateCache();
    }
    if (!readbacks_.empty() && gpu_.Alive() && !suspended_) PumpReadbacks();
    if (detailCheckAt_ && now >= detailCheckAt_) {
        detailCheckAt_ = 0;
        UpdateCache();  // the view settled: maybe a sharper detail is due
    }
    UpdateAnimation();
    if (anim_.playing && !anim_.busy && !minimized_ && now >= anim_.nextAt) StepAnimation();
    if (burstWaitStart_ >= 0 && now >= burstWaitUntil_) {
        burstWaitStart_ = -1;
        Toast("Could not read the capture times in time");
    }
    if (slideshow_) {
        // Each slide gets its full time once it is on screen (a slow decode never shortens it).
        const FileEntry* f = Current();
        const bool ready = f && (TextureFor(f->id) || sched_.IsFailed({f->id, core::Variant::Fit}) ||
                                 (f->probed && FAILED(f->probeHr)));
        if (!f) {
            StopSlideshow();
        } else if (ready && slideShownAt_ == 0) {
            slideShownAt_ = now;
        } else if (ready && now >= slideShownAt_ + uint64_t(settings_.slideshowSeconds) * 1000) {
            if (!settings_.wrapAround && current_ >= ViewCount() - 1) {
                StopSlideshow();
                Toast("Slideshow finished");
            } else {
                Navigate(+1);
            }
        }
    }
    if (!toasts_.empty() && toasts_.front().until <= now) {
        while (!toasts_.empty() && toasts_.front().until <= now) toasts_.pop_front();
        RequestFrames(1);
    }
    if (settingsDirtyAt_ && now >= settingsDirtyAt_ + kSaveDelayMs) SaveSettingsNow();

    // Sleep until the earliest pending deadline; nothing pending means no wakeups at all.
    uint64_t w = 0;
    auto consider = [&](uint64_t t) { w = w ? std::min(w, t) : t; };
    if (folder_.Count()) consider(nextGovernorMs_);
    if (settleArmed_) consider(lastNavMs_ + kSettleMs);
    if (recheckAt_) consider(recheckAt_);
    if (minimized_ && settings_.releaseWhenMinimized && !trimmed_) consider(minimizedAt_ + kTrimAfterMs);
    if (!toasts_.empty()) consider(toasts_.front().until);
    if (settingsDirtyAt_) consider(settingsDirtyAt_ + kSaveDelayMs);
    if (slideshow_ && slideShownAt_) consider(slideShownAt_ + uint64_t(settings_.slideshowSeconds) * 1000);
    if (burstWaitStart_ >= 0) consider(burstWaitUntil_);
    if (anim_.playing && !anim_.busy && !minimized_) consider(anim_.nextAt);
    if (detailCheckAt_) consider(detailCheckAt_);
    if (!readbacks_.empty()) consider(now + 2);  // the GPU copy finishes within a frame or two
    wakeAt_ = w;
}

void App::RenderFrame() {
    if (inRender_ || !gpu_.Alive() || suspended_) return;
    inRender_ = true;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (grid_)
        DrawGrid();
    else
        DrawImage();
    if (settings_.showSidebar) DrawSidebar();
    DrawOverlay();
    DrawToasts();
    if (showSettings_) DrawSettings();
    if (!showSettings_) recordingAction_ = -1;  // closed while waiting for a key
    if (settings_.showStats) DrawStats();
    if (settings_.showInfo) DrawInfo();
    if (showMarks_) DrawMarksPanel();
    if (showHelp_) DrawHelp();

    ImGui::Render();
    float clear[4] = {0, 0, 0, 0};
    if (!translucent_) {
        const bool black = settings_.backdrop == int(core::Backdrop::Black);
        clear[0] = clear[1] = clear[2] = black ? 0.0f : 0.075f;
        clear[3] = 1.0f;
    }
    gpu_.BeginFrame(clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    const HRESULT hr = gpu_.Present();
    inRender_ = false;
    graveyard_.clear();
    if (Gpu::IsDeviceLost(hr)) RecreateDevice();
}

// ---- Window procedure ---------------------------------------------------------------------

LRESULT CALLBACK App::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(self));
    }
    if (self) return self->WndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool App::HandleKey(WPARAM vk, LPARAM lp, bool sys) {
    (void)sys;
    const bool repeat = (lp & (1 << 30)) != 0;
    if (core::IsModifierKey(uint16_t(vk))) return false;
    const core::Chord chord{uint16_t(vk), GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_MENU) < 0};
    const bool plain = !chord.ctrl && !chord.shift && !chord.alt;

    if (recordingAction_ >= 0) {  // Settings > Controls is waiting for a new key
        if (repeat) return true;
        const core::Action target = core::Action(recordingAction_);
        recordingAction_ = -1;
        RequestFrames();
        if (vk == VK_ESCAPE && plain) return true;  // cancelled
        // A key does one thing: take it away from whatever had it.
        for (const core::Action other : keymap_.Conflicts(target, {chord})) {
            auto chords = keymap_.Bindings(other);
            chords.erase(std::remove(chords.begin(), chords.end(), chord), chords.end());
            SetBindings(other, chords);
            Toast(core::ChordToString(chord) + " no longer does \"" + core::InfoFor(other).label + "\"");
        }
        auto chords = keymap_.Bindings(target);
        if (std::find(chords.begin(), chords.end(), chord) == chords.end()) chords.push_back(chord);
        SetBindings(target, chords);
        return true;
    }

    // Esc is fixed, so a bad key map can never lock you in.
    if (vk == VK_ESCAPE && plain) {
        if (repeat) return true;
        if (showHelp_) showHelp_ = false;
        else if (showSettings_) showSettings_ = false;
        else if (showMarks_) showMarks_ = false;
        else if (grid_) ToggleGrid();
        else if (slideshow_) StopSlideshow();
        else if (compareCount_ > 1) SetCompare(1);
        else if (filter_ != core::Filter::All) SetFilter(core::Filter::All);
        else if (fullscreen_) ToggleFullscreen();
        else PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        RequestFrames();
        return true;
    }
    if (grid_ && plain) {  // the grid moves by rows; Enter opens the selected image
        if (vk == VK_UP || vk == VK_DOWN) {
            const int to = std::clamp(current_ + (vk == VK_DOWN ? gridCols_ : -gridCols_), 0, std::max(0, ViewCount() - 1));
            if (to != current_) JumpTo(to, vk == VK_DOWN ? 1 : -1);
            return true;
        }
        if (vk == VK_RETURN) {
            if (!repeat) ToggleGrid();
            return true;
        }
    }
    const core::Action a = keymap_.Lookup(chord);
    if (a == core::Action::None) return false;  // unbound: Alt+F4, Alt+Space etc. reach Windows
    if (!repeat || core::InfoFor(a).repeats) Execute(a, repeat);
    return true;
}

void App::Execute(core::Action a, bool repeat) {
    using A = core::Action;
    (void)repeat;
    const ViewGeometry g = Geometry();
    const ImVec2 vc((g.viewMin.x + g.viewMax.x) * 0.5f, (g.viewMin.y + g.viewMax.y) * 0.5f);
    const int page = std::max(1, TileCount());
    auto toggle = [&](bool& flag) {
        flag = !flag;
        MarkSettingsDirty();
        RequestFrames();
    };
    switch (a) {
        case A::Next: Navigate(+1); break;
        case A::Prev: Navigate(-1); break;
        case A::NextSet: Navigate(+page); break;  // in compare mode: the next set
        case A::PrevSet: Navigate(-page); break;
        case A::First: JumpTo(0, 1); break;
        case A::Last: JumpTo(std::max(0, ViewCount() - page), -1); break;
        case A::ZoomIn:
        case A::ZoomOut:
            if (grid_) {  // bigger or smaller cells
                settings_.gridSize = std::clamp(settings_.gridSize + (a == A::ZoomIn ? 40 : -40), 96, 400);
                MarkSettingsDirty();
                scrollGridToCurrent_ = true;
                UpdateCache();
                RequestFrames();
            } else {
                ZoomAt(a == A::ZoomIn ? 1.25f : 0.8f, vc);
            }
            break;
        case A::Fit:
            zoomMode_ = ZoomMode::Fit;
            cmpZoomRel_ = 1.0f;
            cmpCenter_ = ImVec2(0.5f, 0.5f);
            RequestFrames();
            break;
        case A::ActualSize: ToggleActualSize(vc); break;
        case A::Fullscreen: ToggleFullscreen(); break;
        case A::RotateCW: Rotate(1); break;
        case A::RotateCCW: Rotate(3); break;
        case A::Compare:
            if (grid_) ToggleGrid();
            SetCompare(compareCount_ == 1 ? 2 : compareCount_ == 2 ? 4 : 1);
            break;
        case A::CompareBurst: CompareBurst(); break;
        case A::Grid: ToggleGrid(); break;
        case A::Peaking:
            if (!gpu_.PeakingShader()) {
                Toast("Focus peaking is not available on this graphics device");
                break;
            }
            peaking_ = !peaking_;
            Toast(peaking_ ? "Focus peaking on: sharp edges are painted red" : "Focus peaking off");
            RequestFrames();
            break;
        case A::NextTile:
        case A::PrevTile:
            if (VisibleTiles() > 1) {
                activeTile_ = (activeTile_ + (a == A::PrevTile ? VisibleTiles() - 1 : 1)) % VisibleTiles();
                UpdateTitle();
                RequestFrames();
            }
            break;
        case A::Delete: RecycleActive(); break;
        case A::Move1: case A::Move2: case A::Move3: case A::Move4: case A::Move5:
        case A::Move6: case A::Move7: case A::Move8: case A::Move9:
            MoveActive(int(a) - int(A::Move1) + 1);
            break;
        case A::Undo: Undo(); break;
        case A::Pick: EditActiveMark([](core::Mark m, int) { return core::TogglePick(m); }, 0, "Pick"); break;
        case A::Reject: EditActiveMark([](core::Mark m, int) { return core::ToggleReject(m); }, 0, "Reject"); break;
        case A::Unflag: EditActiveMark([](core::Mark, int) { return core::Mark{}; }, 0, "Mark cleared"); break;
        case A::Rate0: case A::Rate1: case A::Rate2: case A::Rate3: case A::Rate4: case A::Rate5: {
            const int stars = int(a) - int(A::Rate0);
            EditActiveMark(core::SetStars, stars, stars ? "Rated" : "Stars cleared");
            break;
        }
        case A::PicksPanel: showMarks_ = !showMarks_; RequestFrames(); break;
        case A::CycleFilter: SetFilter(core::NextFilter(filter_)); break;
        case A::Slideshow: ToggleSlideshow(); break;
        case A::CopyImage: CopyActiveImage(); break;
        case A::CopyPath: CopyActivePath(); break;
        case A::ShowInExplorer:
            if (const FileEntry* f = Active(); f && !ShowInExplorer(f->path)) Toast("Could not open Explorer");
            break;
        case A::OpenInEditor:
            if (const FileEntry* f = Active(); f && !OpenInEditor(hwnd_, f->path, FromUtf8(settings_.editorPath)))
                Toast(settings_.editorPath.empty() ? "Could not open the \"Open with\" window"
                                                   : "Could not start " + settings_.editorPath);
            break;
        case A::ToggleOverlay: toggle(settings_.showOverlay); break;
        case A::ToggleSidebar:
            scrollSidebarToCurrent_ = true;
            toggle(settings_.showSidebar);
            break;
        case A::ToggleInfo: toggle(settings_.showInfo); break;
        case A::ToggleStats: toggle(settings_.showStats); break;
        case A::Settings: showSettings_ = !showSettings_; RequestFrames(); break;
        case A::Help: showHelp_ = !showHelp_; RequestFrames(); break;
        case A::OpenFile: ShowOpenDialog(false); break;
        case A::OpenFolder: ShowOpenDialog(true); break;
        case A::Rescan: ReloadFolder(); break;
        default: break;
    }
}

bool App::ZoomedIn() const {
    if (compareCount_ > 1) return cmpZoomRel_ > 1.001f;
    const ViewGeometry g = Geometry();
    return !g.source.empty() && (float(g.source.w) * g.zoom > g.viewMax.x - g.viewMin.x + 0.5f ||
                                 float(g.source.h) * g.zoom > g.viewMax.y - g.viewMin.y + 0.5f);
}

void App::OnWheel(int delta, bool horizontal, bool ctrl, ImVec2 screen) {
    const uint64_t now = NowMs();
    if (horizontal) {
        // Tilt wheel or a two-finger sideways swipe: pans a zoomed image, otherwise
        // turns one page per gesture (touchpads send a stream of small steps).
        if (ZoomedIn() && compareCount_ == 1) {
            PanBy(-float(delta), 0);
            return;
        }
        if (now < lastWheelNavMs_ + 250) {  // same gesture still running
            lastWheelNavMs_ = now;
            hwheelAccum_ = 0;
            return;
        }
        hwheelAccum_ += delta;
        if (std::abs(hwheelAccum_) >= WHEEL_DELTA) {
            Navigate(hwheelAccum_ > 0 ? +1 : -1);
            hwheelAccum_ = 0;
            lastWheelNavMs_ = now;
        }
        return;
    }
    if (settings_.wheelNavigates && !ctrl) {  // touchpad pinch arrives as Ctrl+wheel: still zooms
        wheelAccum_ += delta;
        const int steps = wheelAccum_ / WHEEL_DELTA;
        if (steps) {
            wheelAccum_ -= steps * WHEEL_DELTA;
            Navigate(-steps);  // wheel down = next
        }
        return;
    }
    ZoomAt(std::pow(1.2f, float(delta) / WHEEL_DELTA), screen);
}

void App::OnGesture(HGESTUREINFO h) {
    GESTUREINFO gi{};
    gi.cbSize = sizeof gi;
    if (!GetGestureInfo(h, &gi)) return;
    POINT pt{gi.ptsLocation.x, gi.ptsLocation.y};
    ScreenToClient(hwnd_, &pt);
    const ImVec2 at(float(pt.x), float(pt.y));
    switch (gi.dwID) {
        case GID_ZOOM:  // pinch: ullArguments is the distance between the fingers
            if (!(gi.dwFlags & GF_BEGIN) && gestureZoomLast_ && gi.ullArguments)
                ZoomAt(float(double(gi.ullArguments) / double(gestureZoomLast_)), at);
            gestureZoomLast_ = gi.ullArguments;
            break;
        case GID_PAN:
            if (gi.dwFlags & GF_BEGIN) {
                gestureSwipe_ = 0;
                dragStart_ = pt;
            } else if (ZoomedIn()) {
                PanBy(float(pt.x - gestureLast_.x), float(pt.y - gestureLast_.y));
            } else if (!(gi.dwFlags & GF_INERTIA)) {
                gestureSwipe_ += pt.x - gestureLast_.x;  // swipe sideways to change image
            }
            gestureLast_ = pt;
            if ((gi.dwFlags & GF_END) && std::abs(gestureSwipe_) > int(60 * dpiScale_)) {
                Navigate(gestureSwipe_ < 0 ? +1 : -1);
                gestureSwipe_ = 0;
            }
            break;
        case GID_TWOFINGERTAP: ToggleActualSize(at); break;
        default: break;
    }
    RequestFrames();
}

LRESULT App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    const ImGuiIO* io = ImGui::GetCurrentContext() ? &ImGui::GetIO() : nullptr;
    const bool uiMouse = io && io->WantCaptureMouse;

    switch (msg) {
        case WM_APP_WAKE: DrainCompletions(); return 0;
        case WM_APP_DIRCHANGED: ApplyDirChanges(); return 0;

        case WM_COPYDATA: {
            const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (cds && cds->dwData == kCopyDataOpen && cds->cbData >= sizeof(wchar_t)) {
                std::wstring path(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t));
                path.resize(wcsnlen(path.c_str(), path.size()));
                OpenPath(path);
                return TRUE;
            }
            break;
        }
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            const UINT len = DragQueryFileW(drop, 0, nullptr, 0);
            std::wstring path(len, L'\0');
            DragQueryFileW(drop, 0, path.data(), len + 1);
            DragFinish(drop);
            OpenPath(path);
            return 0;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            RequestFrames();
            if (io && io->WantTextInput) break;
            if (HandleKey(wp, lp, msg == WM_SYSKEYDOWN)) return 0;
            break;

        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            RequestFrames();
            if (!uiMouse) {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(hwnd, &pt);
                OnWheel(GET_WHEEL_DELTA_WPARAM(wp), msg == WM_MOUSEHWHEEL, (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) != 0,
                        ImVec2(float(pt.x), float(pt.y)));
            }
            return 0;
        case WM_GESTURENOTIFY: {
            // Pan in every direction with one finger (no "gutter"), with inertia.
            GESTURECONFIG cfg[] = {{GID_ZOOM, GC_ZOOM, 0},
                                   {GID_PAN, GC_PAN | GC_PAN_WITH_SINGLE_FINGER_VERTICALLY |
                                                 GC_PAN_WITH_SINGLE_FINGER_HORIZONTALLY | GC_PAN_WITH_INERTIA,
                                    GC_PAN_WITH_GUTTER},
                                   {GID_TWOFINGERTAP, GC_TWOFINGERTAP, 0}};
            SetGestureConfig(hwnd, 0, UINT(std::size(cfg)), cfg, sizeof(GESTURECONFIG));
            break;
        }
        case WM_GESTURE:
            if (!uiMouse && (wp == GID_ZOOM || wp == GID_PAN || wp == GID_TWOFINGERTAP)) {
                OnGesture(reinterpret_cast<HGESTUREINFO>(lp));
                CloseGestureInfoHandle(reinterpret_cast<HGESTUREINFO>(lp));
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
            RequestFrames();
            if (!uiMouse) {
                dragging_ = true;
                dragMoved_ = false;
                dragLast_ = dragStart_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                SetCapture(hwnd);
            }
            return 0;
        case WM_MOUSEMOVE:
            RequestFrames();
            if (dragging_) {
                const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (std::abs(p.x - dragStart_.x) + std::abs(p.y - dragStart_.y) > 3) dragMoved_ = true;
                PanBy(float(p.x - dragLast_.x), float(p.y - dragLast_.y));
                dragLast_ = p;
            }
            return 0;
        case WM_LBUTTONUP:
            RequestFrames();
            if (dragging_) {
                dragging_ = false;
                ReleaseCapture();
                if (!dragMoved_ && TileCount() > 1) {  // a click (not a drag) picks the active tile
                    const int s = TileAt(ImVec2(float(dragStart_.x), float(dragStart_.y)));
                    if (s >= 0 && s != activeTile_ && TileIndex(s) >= 0) {
                        activeTile_ = s;
                        UpdateTitle();
                    }
                }
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            RequestFrames();
            if (!uiMouse) ToggleActualSize(ImVec2(float(GET_X_LPARAM(lp)), float(GET_Y_LPARAM(lp))));
            return 0;
        case WM_XBUTTONUP:
            Navigate(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? -1 : +1);
            return TRUE;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MOUSELEAVE:
            RequestFrames();
            break;

        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) {
                minimized_ = true;
                minimizedAt_ = NowMs();
            } else {
                minimized_ = false;
                if (trimmed_) {
                    trimmed_ = false;
                    UpdateCache();
                }
                if (gpu_.Alive()) {
                    const HRESULT hr = gpu_.Resize(LOWORD(lp), HIWORD(lp));
                    if (Gpu::IsDeviceLost(hr)) RecreateDevice();
                    RequestFrames();
                    RenderFrame();  // keeps live resizing smooth (the main loop is blocked in the sizing loop)
                }
            }
            return 0;
        case WM_EXITSIZEMOVE:
        case WM_DISPLAYCHANGE:
            UpdateMonitor();
            RequestFrames();
            break;
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (ImGui::GetCurrentContext()) ApplyDpi(HIWORD(wp));
            UpdateMonitor();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize = {MulDiv(480, int(GetDpiForWindow(hwnd)), 96), MulDiv(320, int(GetDpiForWindow(hwnd)), 96)};
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            RequestFrames(1);
            return 0;
        }
        case WM_ACTIVATE:
            RequestFrames();
            break;
        case WM_CLOSE:
            SaveSettingsNow();
            running_ = false;
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace fiv
