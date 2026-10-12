#include "App.h"

#include <imgui_impl_dx11.h>
#include <shlwapi.h>
#include <imgui_impl_win32.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "../platform/FileOps.h"

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
    if (settingsDirtyAt_ || hwnd_) SaveSettingsNow();
    watcher_.Stop();
    jobs_.Stop();
    {
        std::lock_guard lk(completionsMutex_);
        completions_.clear();  // drops any textures produced after shutdown began
    }
    textures_.clear();
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
    suspended_ = true;
    ++deviceGen_;  // anything still running finishes on the old device and is discarded
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
    if (current_ < 0 || current_ >= folder_.Count()) return nullptr;
    return &folder_.Files()[size_t(current_)];
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
    if (!folder_.Load(folder, formats_)) Toast("Could not read the folder");
    storage_ = DetectStorage(folder);
    gate_.SetSlots(ReadersFor(storage_));
    if (!same) watcher_.Start(folder, hwnd_, WM_APP_DIRCHANGED);

    int index = 0;
    if (!select.empty()) {
        index = folder_.IndexOfPath(select);
        if (index < 0) {
            index = 0;
            if (GetFileAttributesW(select.c_str()) != INVALID_FILE_ATTRIBUTES)
                Toast("That file type is turned off or not supported (see Settings > Formats)");
        }
    }
    current_ = index;
    currentPath_ = folder_.Count() ? folder_.Files()[size_t(index)].path : L"";
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
    for (const auto& n : names) changed |= folder_.Refresh(n, formats_, now);
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
    const int n = folder_.Count();
    const int i = folder_.IndexOfPath(currentPath_);
    if (i >= 0) {
        current_ = i;
    } else {
        // Current file vanished: whatever now sits at its position (the next file) takes over.
        current_ = n ? std::clamp(current_, 0, n - 1) : 0;
        currentPath_ = n ? folder_.Files()[size_t(current_)].path : L"";
        if (compareCount_ == 1) ResetView();
    }
    activeTile_ = std::clamp(activeTile_, 0, std::max(0, VisibleTiles() - 1));
    UpdateTitle();
}

void App::Navigate(int delta) {
    const int n = folder_.Count();
    if (n == 0) return;
    int next = current_ + delta;
    if (settings_.wrapAround)
        next = ((next % n) + n) % n;
    else
        next = std::clamp(next, 0, n - 1);
    if (next != current_) JumpTo(next, delta > 0 ? 1 : -1);
}

void App::JumpTo(int index, int direction) {
    if (index < 0 || index >= folder_.Count()) return;
    current_ = index;
    currentPath_ = folder_.Files()[size_t(index)].path;
    direction_ = direction < 0 ? -1 : 1;
    lastNavMs_ = NowMs();
    settleArmed_ = true;
    scrollSidebarToCurrent_ = true;
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

std::wstring App::DestinationFor(int key) const {
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
        const std::wstring& folder = folder_.Folder();
        d = folder + (folder.empty() || folder.back() == L'\\' ? L"" : L"\\") + d;
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
    if (CompareStringOrdinal(ParentFolder(primary).c_str(), -1, folder_.Folder().c_str(), -1, TRUE) == CSTR_EQUAL) {
        for (const auto& f : op.files) folder_.Refresh(LeafOf(f.original), formats_, 0);
        const int i = folder_.IndexOfPath(primary);
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

void App::UpdateTitle() {
    std::wstring title = L"FastImageViewer";
    if (const FileEntry* f = Active())
        title = f->name + L"  \u2014  " + std::to_wstring(ActiveIndex() + 1) + L" / " + std::to_wstring(folder_.Count()) +
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

core::DecodePlan App::PlanFor(const FileEntry& f, core::Variant v, uint64_t maxBytes) const {
    core::PlanRequest r;
    r.source = f.Oriented();
    const bool fit = v == core::Variant::Fit && settings_.decodeMode == int(core::DecodeMode::FitScreen);
    if (fit) r.target = monitor_;
    r.maxTextureDim = gpu_.MaxTextureDim();
    r.maxGpuBytes = maxBytes;
    return core::PlanDecode(r);
}

uint64_t App::TransientFor(const FileEntry& f, const core::DecodePlan& plan) const {
    core::TransientRequest t;
    t.fileBytes = f.size;
    t.readIntoMemory = f.size <= core::kMaxInMemoryFile;
    t.stored = f.stored;
    t.canPrescale = f.canPrescale;
    t.reorients = f.orientation != 1;
    t.progressive = f.progressive;
    t.plan = plan;
    return core::TransientBytes(t);
}

std::vector<core::Want> App::BuildWants() {
    std::vector<core::Want> out;
    const int n = folder_.Count();
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
        FileEntry& f = folder_.Files()[size_t(index)];
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

    for (size_t t = 0; t < visible.size(); ++t) addWant(visible[t], int(t), true, int(t));
    for (const core::WindowSlot& slot : core::ComputeWindow(p)) {
        if (std::find(visible.begin(), visible.end(), slot.index) != visible.end()) continue;
        addWant(slot.index, 2 * tiles + slot.rank, false, 0);
    }
    // Forget frozen full-size plans of images that are no longer on screen.
    for (auto it = fullPlans_.begin(); it != fullPlans_.end();) {
        const bool shown = std::any_of(visible.begin(), visible.end(),
                                       [&](int i) { return folder_.Files()[size_t(i)].id == it->first; });
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
    }
    UpdateCache();
}

void App::StartDecode(uint64_t job, const core::EntryKey& key, const core::DecodePlan& plan, int rank) {
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
        textures_[key] = {r.srv, plan.size, r.histogram};
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
        if (IsVisible(key.item)) RequestFrames();
    }
    if (FileEntry* f = folder_.FindById(key.item)) {
        if (outcome == core::Outcome::Failed) f->error = DescribeError(r.hr);
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

void App::CancelJob(uint64_t job) { jobs_.Cancel(job); }

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

const App::Texture* App::TextureFor(uint64_t item) const {
    auto full = textures_.find({item, core::Variant::Full});
    if (full != textures_.end()) return &full->second;
    auto fit = textures_.find({item, core::Variant::Fit});
    if (fit != textures_.end()) return &fit->second;
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

int App::TileCount() const { return std::max(0, std::min(compareCount_, folder_.Count())); }

int App::VisibleTiles() const {
    int n = 0;
    while (n < TileCount() && TileIndex(n) >= 0) ++n;  // tiles fill from the first one
    return n;
}

int App::TileIndex(int slot) const {
    const int n = folder_.Count();
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
    if (folder_.Count() == 0) return -1;
    if (compareCount_ <= 1) return current_;
    return TileIndex(std::clamp(activeTile_, 0, std::max(0, VisibleTiles() - 1)));
}

const FileEntry* App::Active() const {
    const int i = ActiveIndex();
    return i >= 0 && i < folder_.Count() ? &folder_.Files()[size_t(i)] : nullptr;
}

bool App::IsVisible(uint64_t item) const {
    for (int t = 0; t < TileCount(); ++t) {
        const int i = TileIndex(t);
        if (i >= 0 && folder_.Files()[size_t(i)].id == item) return true;
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
    if (index < 0 || index >= folder_.Count()) return g;
    g.source = SourceSize(folder_.Files()[size_t(index)]);
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
    return GeometryFor(folder_.Count() ? current_ : -1, mn, mx);
}

void App::ResetView() {
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

void App::ZoomAt(float factor, ImVec2 screen) {
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
    wakeAt_ = w;
}

void App::RenderFrame() {
    if (inRender_ || !gpu_.Alive() || suspended_) return;
    inRender_ = true;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    DrawImage();
    if (settings_.showSidebar) DrawSidebar();
    DrawOverlay();
    DrawToasts();
    if (showSettings_) DrawSettings();
    if (settings_.showStats) DrawStats();
    if (settings_.showInfo) DrawInfo();
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
    const bool repeat = (lp & (1 << 30)) != 0;
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    const ViewGeometry g = Geometry();
    const ImVec2 vc((g.viewMin.x + g.viewMax.x) * 0.5f, (g.viewMin.y + g.viewMax.y) * 0.5f);

    if (sys) {
        if (vk == VK_RETURN && !repeat) {
            ToggleFullscreen();
            return true;
        }
        return false;
    }
    auto fit = [&] {
        zoomMode_ = ZoomMode::Fit;
        cmpZoomRel_ = 1.0f;
        cmpCenter_ = ImVec2(0.5f, 0.5f);
        RequestFrames();
    };
    const int page = std::max(1, TileCount());
    switch (vk) {
        case VK_RIGHT: case VK_DOWN: case VK_SPACE: Navigate(+1); return true;
        case VK_LEFT: case VK_UP: case VK_BACK: Navigate(-1); return true;
        case VK_NEXT: Navigate(+page); return true;   // in compare mode: the next set
        case VK_PRIOR: Navigate(-page); return true;
        case VK_HOME: JumpTo(0, 1); return true;
        case VK_END: JumpTo(std::max(0, folder_.Count() - page), -1); return true;
        case VK_DELETE: if (!repeat) RecycleActive(); return true;
        case VK_TAB:
            if (VisibleTiles() > 1) {
                activeTile_ = (activeTile_ + (shift ? VisibleTiles() - 1 : 1)) % VisibleTiles();
                UpdateTitle();
                RequestFrames();
            }
            return true;
        case VK_ESCAPE:
            if (showHelp_) showHelp_ = false;
            else if (showSettings_) showSettings_ = false;
            else if (compareCount_ > 1) SetCompare(1);
            else if (fullscreen_) ToggleFullscreen();
            else PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            RequestFrames();
            return true;
        case VK_F1: showHelp_ = !showHelp_; RequestFrames(); return true;
        case VK_F3: settings_.showStats = !settings_.showStats; MarkSettingsDirty(); RequestFrames(); return true;
        case VK_F5: ReloadFolder(); return true;
        case VK_F11: if (!repeat) ToggleFullscreen(); return true;
        case VK_ADD: case VK_OEM_PLUS: ZoomAt(1.25f, vc); return true;
        case VK_SUBTRACT: case VK_OEM_MINUS: ZoomAt(0.8f, vc); return true;
        case VK_NUMPAD0: fit(); return true;
        default: break;
    }
    if (vk >= '1' && vk <= '9' && !ctrl) {
        if (!repeat) MoveActive(int(vk - '0'));
        return true;
    }
    if (repeat) return false;
    switch (vk) {
        case '0': fit(); return true;
        case 'C': if (!ctrl) SetCompare(compareCount_ == 1 ? 2 : compareCount_ == 2 ? 4 : 1); return true;
        case 'O':
            if (ctrl) ShowOpenDialog(shift);
            else { settings_.showOverlay = !settings_.showOverlay; MarkSettingsDirty(); }
            RequestFrames();
            return true;
        case 'S': showSettings_ = !showSettings_; RequestFrames(); return true;
        case 'B': settings_.showSidebar = !settings_.showSidebar; scrollSidebarToCurrent_ = true; MarkSettingsDirty(); RequestFrames(); return true;
        case 'I': settings_.showInfo = !settings_.showInfo; MarkSettingsDirty(); RequestFrames(); return true;
        case 'H': showHelp_ = !showHelp_; RequestFrames(); return true;
        case 'F': ToggleFullscreen(); return true;
        case 'Z':
            if (ctrl) Undo();
            else ToggleActualSize(vc);
            return true;
        default: return false;
    }
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
            RequestFrames();
            if (!uiMouse) {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(hwnd, &pt);
                ZoomAt(std::pow(1.2f, float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA), ImVec2(float(pt.x), float(pt.y)));
            }
            return 0;
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
