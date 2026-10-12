// All Dear ImGui drawing. Rendering is event driven: nothing here animates,
// so an idle viewer draws no frames at all.
#include "App.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdarg>
#include <cstdio>

namespace fiv {

namespace {

ImTextureID ToTex(ID3D11ShaderResourceView* srv) { return (ImTextureID)(intptr_t)srv; }

const ImVec4 kGreen(0.35f, 0.80f, 0.45f, 1), kAmber(0.95f, 0.70f, 0.25f, 1), kRed(0.95f, 0.35f, 0.35f, 1),
    kBlue(0.40f, 0.65f, 1.0f, 1), kDim(0.60f, 0.60f, 0.62f, 1);

void TextDim(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

bool SliderMB(const char* label, uint32_t* value, uint32_t maxMB) {
    const uint32_t lo = core::kMinBudgetMB;
    const uint32_t hi = std::max(lo, maxMB);
    return ImGui::SliderScalar(label, ImGuiDataType_U32, value, &lo, &hi, "%u MB", ImGuiSliderFlags_AlwaysClamp);
}

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void CompanionKinds(const FileEntry& f, bool& raw, bool& xmp) {
    for (const auto& c : f.companions) {
        const size_t dot = c.find_last_of(L'.');
        (dot != std::wstring::npos && ToLower(c.substr(dot)) == L".xmp" ? xmp : raw) = true;
    }
}

void SetPointSampler(const ImDrawList*, const ImDrawCmd* cmd) {
    auto* gpu = static_cast<Gpu*>(cmd->UserCallbackData);
    ID3D11SamplerState* s = gpu->PointSampler();
    gpu->Context()->PSSetSamplers(0, 1, &s);
}

}  // namespace

// ---- Image ------------------------------------------------------------------------

void App::DrawImage() {
    if (!Current()) {
        DrawEmpty();
        return;
    }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const bool compare = compareCount_ > 1;
    for (const Tile& tile : Tiles()) {
        if (tile.index < 0) continue;
        const FileEntry& f = folder_.Files()[size_t(tile.index)];
        const ViewGeometry g = GeometryFor(tile.index, tile.min, tile.max);
        const ImVec2 vc((g.viewMin.x + g.viewMax.x) * 0.5f, (g.viewMin.y + g.viewMax.y) * 0.5f);

        const Texture* tex = TextureFor(f.id);
        const Texture* shown = tex ? tex : (previewItem_ == f.id && preview_.srv ? &preview_ : nullptr);
        if (shown && !g.source.empty()) {
            const ImVec2 size(float(g.source.w) * g.zoom, float(g.source.h) * g.zoom);
            const ImVec2 pmin(std::round(vc.x - g.center.x * g.zoom), std::round(vc.y - g.center.y * g.zoom));
            const ImVec2 pmax(pmin.x + size.x, pmin.y + size.y);
            const float texelScale = size.x / float(std::max(1u, shown->size.w));  // screen pixels per texel
            const bool point = settings_.pixelatedZoom && shown != &preview_ && texelScale > 1.01f;
            dl->PushClipRect(g.viewMin, g.viewMax, false);
            if (point) dl->AddCallback(SetPointSampler, &gpu_);
            dl->AddImage(ToTex(shown->srv.Get()), pmin, pmax);
            if (point) dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
            dl->PopClipRect();

            // Zoomed past the screen-sized texture: fetch full-resolution versions of the visible images.
            if (tex && !wantFull_ && texelScale > 1.02f && tex->size.w < g.source.w &&
                settings_.decodeMode == int(core::DecodeMode::FitScreen)) {
                wantFull_ = true;
                UpdateCache();
            }
        }

        if (!tex) {
            std::string msg;
            ImVec4 color = kDim;
            if (!f.error.empty() && f.retryAt <= NowMs()) {
                msg = f.error;
                color = kRed;
            } else if (f.CloudOnly()) {
                msg = "Downloading online-only file...";
            } else {
                msg = "Loading...";
            }
            const ImVec2 ts = ImGui::CalcTextSize(msg.c_str());
            const ImVec2 pos(vc.x - ts.x * 0.5f, shown ? g.viewMax.y - ts.y - 24 * dpiScale_ : vc.y - ts.y * 0.5f);
            dl->PushClipRect(g.viewMin, g.viewMax, true);
            dl->AddRectFilled(ImVec2(pos.x - 10 * dpiScale_, pos.y - 6 * dpiScale_),
                              ImVec2(pos.x + ts.x + 10 * dpiScale_, pos.y + ts.y + 6 * dpiScale_), IM_COL32(0, 0, 0, 150),
                              6 * dpiScale_);
            dl->AddText(pos, ImGui::ColorConvertFloat4ToU32(color), msg.c_str());
            dl->PopClipRect();
        }

        if (compare) {  // tile caption + active outline
            const bool active = tile.slot == activeTile_;
            char caption[32];
            std::snprintf(caption, sizeof caption, "%d  ", tile.index + 1);
            const std::string text = caption + ToUtf8(f.name);
            const float pad = 6 * dpiScale_;
            const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
            dl->PushClipRect(tile.min, tile.max, true);
            dl->AddRectFilled(ImVec2(tile.min.x + pad, tile.min.y + pad),
                              ImVec2(tile.min.x + 3 * pad + ts.x, tile.min.y + 2 * pad + ts.y + pad * 0.5f),
                              active ? IM_COL32(40, 90, 170, 220) : IM_COL32(0, 0, 0, 150), 4 * dpiScale_);
            dl->AddText(ImVec2(tile.min.x + 2 * pad, tile.min.y + 1.5f * pad), IM_COL32_WHITE, text.c_str());
            if (active)
                dl->AddRect(ImVec2(tile.min.x + 1, tile.min.y + 1), ImVec2(tile.max.x - 1, tile.max.y - 1),
                            IM_COL32(70, 140, 255, 255), 0, 0, 2 * dpiScale_);
            dl->PopClipRect();
        }
    }
}

void App::DrawEmpty() {
    ImVec2 mn, mx;
    ViewArea(mn, mx);
    const ImVec2 vc((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
    ImGui::SetNextWindowPos(vc, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##empty", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (folder_.Folder().empty()) {
        ImGui::TextUnformatted("Drop an image or a folder here");
        TextDim("or use Ctrl+O, or \"Open with\" in Explorer");
    } else {
        ImGui::TextUnformatted("No images in this folder");
        TextDim("%s", ToUtf8(folder_.Folder()).c_str());
    }
    ImGui::Spacing();
    if (ImGui::Button("Open image...")) pendingOpen_ = 1;
    ImGui::SameLine();
    if (ImGui::Button("Open folder...")) pendingOpen_ = 2;
    ImGui::SameLine();
    if (ImGui::Button("Settings")) showSettings_ = true;
    ImGui::End();
}

// ---- Overlay & toasts ------------------------------------------------------------------

void App::DrawOverlay() {
    const FileEntry* f = Active();
    if (!settings_.showOverlay || !f) return;
    const ViewGeometry g = Geometry();
    ImVec2 mn, mx;
    ViewArea(mn, mx);
    const float pad = 10 * dpiScale_;
    const int c = settings_.overlayCorner;
    const bool right = c == 1 || c == 3, bottom = c == 2 || c == 3;
    ImGui::SetNextWindowPos(ImVec2(right ? mx.x - pad : mn.x + pad, bottom ? mx.y - pad : pad),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    if (TileCount() > 1) ImGui::TextColored(kBlue, "Compare %d  \xC2\xB7  tile %d", TileCount(), activeTile_ + 1);
    if (settings_.ovName) ImGui::TextUnformatted(ToUtf8(f->name).c_str());
    if (!f->companions.empty()) {
        bool raw = false, xmp = false;
        CompanionKinds(*f, raw, xmp);
        ImGui::TextColored(kAmber, "%s%s", raw ? "+RAW " : "", xmp ? "+XMP" : "");
    }
    if (settings_.ovType) {
        const ImageFormat* fmt = formats_.ForExtension(f->ext);
        TextDim("%s", fmt ? fmt->name.c_str() : ToUtf8(f->ext).c_str());
    }
    if (settings_.ovIndex) TextDim("%d / %d", ActiveIndex() + 1, folder_.Count());
    if (settings_.ovSize) TextDim("%s", FormatBytes(f->size).c_str());
    if (settings_.ovDims && f->probed && SUCCEEDED(f->probeHr)) TextDim("%u x %u", f->Oriented().w, f->Oriented().h);
    if (settings_.ovZoom && !g.source.empty()) TextDim("%.0f%%", g.zoom * 100.0f);
    if (f->CloudOnly()) ImGui::TextColored(kBlue, "Online-only file");
    ImGui::End();
}

void App::DrawToasts() {
    if (toasts_.empty()) return;
    ImVec2 mn, mx;
    ViewArea(mn, mx);
    ImGui::SetNextWindowPos(ImVec2((mn.x + mx.x) * 0.5f, mx.y - 16 * dpiScale_), ImGuiCond_Always,
                            ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("##toasts", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    for (const auto& t : toasts_) ImGui::TextUnformatted(t.text.c_str());
    ImGui::End();
}

// ---- Sidebar ---------------------------------------------------------------------------

void App::DrawSidebar() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float w = SidebarWidthPx();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, float(rc.bottom)));
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("##sidebar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    const std::wstring& folder = folder_.Folder();
    const size_t slash = folder.find_last_of(L'\\');
    ImGui::TextUnformatted(ToUtf8(slash == std::wstring::npos ? folder : folder.substr(slash + 1)).c_str());
    TextDim("%d images  |  %s", folder_.Count(), StorageName(storage_));
    TextDim("Cache %s of %s %s", FormatBytes(sched_.ResidentGpu()).c_str(), FormatBytes(limits_.gpu).c_str(),
            caps_.unified ? "(shared RAM)" : "VRAM");
    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginChild("##list", ImVec2(0, 0), false, ImGuiWindowFlags_NoNav);
    const float rowH = std::round((float(settings_.thumbHeight) + 8) * dpiScale_);
    const int n = folder_.Count();
    if (scrollSidebarToCurrent_ && n) {
        const float target = float(current_) * rowH - (ImGui::GetWindowHeight() - rowH) * 0.5f;
        ImGui::SetScrollY(std::max(0.0f, target));
        scrollSidebarToCurrent_ = false;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = 4 * dpiScale_, box = rowH - 2 * pad;
    ImGuiListClipper clip;
    clip.Begin(n, rowH);
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const FileEntry& e = folder_.Files()[size_t(i)];
            ImGui::PushID(i);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float rowW = ImGui::GetContentRegionAvail().x;
            bool shown = false;
            for (int t = 0; t < TileCount(); ++t) shown |= TileIndex(t) == i;
            if (ImGui::Selectable("##row", shown, 0, ImVec2(0, rowH))) JumpTo(i, i >= current_ ? 1 : -1);

            const ImVec2 b0(p.x + pad, p.y + pad), b1(b0.x + box, b0.y + box);
            if (const Texture* t = TextureFor(e.id)) {
                const float a = float(t->size.w) / float(std::max(1u, t->size.h));
                const ImVec2 s = a >= 1 ? ImVec2(box, box / a) : ImVec2(box * a, box);
                const ImVec2 o(b0.x + (box - s.x) * 0.5f, b0.y + (box - s.y) * 0.5f);
                dl->AddImage(ToTex(t->srv.Get()), o, ImVec2(o.x + s.x, o.y + s.y));
            } else {
                ImVec4 c(0.18f, 0.18f, 0.20f, 1);
                if (sched_.IsDecoding({e.id, core::Variant::Fit}) || sched_.IsProbing(e.id)) c = ImVec4(0.45f, 0.36f, 0.15f, 1);
                if (!e.error.empty() || sched_.IsFailed({e.id, core::Variant::Fit})) c = ImVec4(0.45f, 0.15f, 0.15f, 1);
                if (e.CloudOnly()) c = ImVec4(0.15f, 0.25f, 0.45f, 1);
                dl->AddRectFilled(b0, b1, ImGui::ColorConvertFloat4ToU32(c), 3 * dpiScale_);
            }

            const float tx = b1.x + 8 * dpiScale_;
            dl->PushClipRect(ImVec2(tx, p.y), ImVec2(p.x + rowW, p.y + rowH), true);
            char line[64];
            std::snprintf(line, sizeof line, "%d", i + 1);
            const float lh = ImGui::GetTextLineHeight();
            dl->AddText(ImVec2(tx, p.y + pad), ImGui::GetColorU32(ImGuiCol_Text), ToUtf8(e.name).c_str());
            std::string info = line;
            if (e.probed && SUCCEEDED(e.probeHr)) {
                std::snprintf(line, sizeof line, "  |  %u x %u", e.Oriented().w, e.Oriented().h);
                info += line;
            }
            info += "  |  " + FormatBytes(e.size);
            dl->AddText(ImVec2(tx, p.y + pad + lh), ImGui::ColorConvertFloat4ToU32(kDim), info.c_str());
            dl->PopClipRect();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
}

// ---- Settings --------------------------------------------------------------------------

void App::DrawSettings() {
    ImGui::SetNextWindowSize(ImVec2(600 * dpiScale_, 0), ImGuiCond_Appearing);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    ImGui::SetNextWindowPos(ImVec2(float(rc.right) * 0.5f, float(rc.bottom) * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Settings", &showSettings_, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Cache & memory")) {
            bool cache = false;
            ImGui::SeparatorText("Window");
            cache |= ImGui::SliderInt("Prefetch ahead", &settings_.ahead, 0, core::kMaxAhead, "%d images", ImGuiSliderFlags_AlwaysClamp);
            HelpMarker("Images decoded in advance in the direction you are browsing, so they appear instantly.");
            cache |= ImGui::SliderInt("Keep behind", &settings_.behind, 0, core::kMaxBehind, "%d images", ImGuiSliderFlags_AlwaysClamp);
            HelpMarker("Images you just viewed stay in memory so stepping back never touches the disk.");

            ImGui::SeparatorText("Memory budget");
            cache |= ImGui::Checkbox("Automatic (recommended)", &settings_.autoBudget);
            HelpMarker("Picks a budget from your hardware. Either way the cache always shrinks when Windows or other "
                       "programs need the memory, so nothing is pushed to the page file.");
            ImGui::BeginDisabled(settings_.autoBudget);
            if (caps_.unified) {
                uint32_t v = settings_.autoBudget ? uint32_t(caps_.defaultRam / core::kMiB) : settings_.ramBudgetMB;
                if (SliderMB("Image memory (shared)", &v, uint32_t(caps_.maxRam / core::kMiB))) {
                    settings_.ramBudgetMB = v;
                    cache = true;
                }
                TextDim("Integrated GPU: images live in system RAM. Hardware limit %s.", FormatBytes(caps_.maxRam).c_str());
            } else {
                uint32_t v = settings_.autoBudget ? uint32_t(caps_.defaultVram / core::kMiB) : settings_.vramBudgetMB;
                if (SliderMB("Video memory", &v, uint32_t(caps_.maxVram / core::kMiB))) {
                    settings_.vramBudgetMB = v;
                    cache = true;
                }
                TextDim("Hardware limit %s (%s card, 1 GB always left free).", FormatBytes(caps_.maxVram).c_str(),
                        FormatBytes(gpu_.Adapter().dedicatedVram).c_str());
                uint32_t r = settings_.autoBudget ? uint32_t(caps_.defaultRam / core::kMiB) : settings_.ramBudgetMB;
                if (SliderMB("Decode memory (RAM)", &r, uint32_t(caps_.maxRam / core::kMiB))) {
                    settings_.ramBudgetMB = r;
                    cache = true;
                }
                TextDim("Hardware limit %s of %s RAM. Only used briefly while decoding.", FormatBytes(caps_.maxRam).c_str(),
                        FormatBytes(live_.totalRam).c_str());
            }
            ImGui::EndDisabled();
            TextDim("In use now: %s of %s allowed%s", FormatBytes(sched_.CommittedGpu()).c_str(), FormatBytes(limits_.gpu).c_str(),
                    live_.lowMemory ? "  (Windows reports low memory: cache reduced)" : "");

            ImGui::SeparatorText("Decoding");
            const char* modes[] = {"Fit to screen (recommended)", "Full resolution (uses much more memory)"};
            cache |= ImGui::Combo("Resolution", &settings_.decodeMode, modes, 2);
            HelpMarker("Fit to screen decodes each image at your monitor's resolution. Zooming in past that loads the "
                       "full-resolution version of the current image only.");
            cache |= ImGui::Checkbox("Release cache when minimized", &settings_.releaseWhenMinimized);
            HelpMarker("Ten seconds after minimizing, everything except the current image is released.");
            cache |= ImGui::Checkbox("Don't prefetch online-only (OneDrive) files", &settings_.skipCloudFiles);
            HelpMarker("Reading an online-only file makes OneDrive download it to your disk. With this on, such files are "
                       "only read when you actually open them.");
            if (cache) OnCacheSettingsChanged();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display")) {
            bool changed = false;
            const char* backdrops[] = {"Mica (Windows 11)", "Dark", "Black"};
            if (ImGui::Combo("Background", &settings_.backdrop, backdrops, 3)) {
                ApplyBackdropSetting();
                changed = true;
            }
            if (settings_.backdrop == int(core::Backdrop::Mica) && !translucent_)
                TextDim("Mica is not available on this system; using Dark.");
            changed |= ImGui::Checkbox("Enlarge small images to fit the window", &settings_.upscaleSmall);
            changed |= ImGui::Checkbox("Sharp pixels when zoomed in", &settings_.pixelatedZoom);
            changed |= ImGui::Checkbox("Wrap around at the end of the folder", &settings_.wrapAround);
            if (ImGui::IsItemEdited()) UpdateCache();
            changed |= ImGui::Checkbox("Close when the folder becomes empty", &settings_.closeWhenEmpty);
            ImGui::SeparatorText("Sidebar");
            changed |= ImGui::Checkbox("Show sidebar (B)", &settings_.showSidebar);
            changed |= ImGui::SliderInt("Width", &settings_.sidebarWidth, 160, 900, "%d px", ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::SliderInt("Thumbnail size", &settings_.thumbHeight, 32, 240, "%d px", ImGuiSliderFlags_AlwaysClamp);
            if (changed) MarkSettingsDirty();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Overlay")) {
            bool changed = ImGui::Checkbox("Show overlay (O)", &settings_.showOverlay);
            const char* corners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
            changed |= ImGui::Combo("Corner", &settings_.overlayCorner, corners, 4);
            ImGui::SeparatorText("Show");
            changed |= ImGui::Checkbox("File name", &settings_.ovName);
            changed |= ImGui::Checkbox("Format", &settings_.ovType);
            changed |= ImGui::Checkbox("Position in folder", &settings_.ovIndex);
            changed |= ImGui::Checkbox("File size", &settings_.ovSize);
            changed |= ImGui::Checkbox("Dimensions", &settings_.ovDims);
            changed |= ImGui::Checkbox("Zoom", &settings_.ovZoom);
            if (changed) MarkSettingsDirty();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sorting")) {
            DrawSortingTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Formats")) {
            bool changed = false;
            for (auto& fmt : formats_.All()) {
                ImGui::PushID(fmt.name.c_str());
                ImGui::BeginDisabled(!fmt.installed);
                changed |= ImGui::Checkbox(fmt.name.c_str(), &fmt.enabled);
                ImGui::EndDisabled();
                if (!fmt.installed) {
                    ImGui::SameLine();
                    ImGui::TextColored(kAmber, "codec not installed");
                    if (!fmt.storeUrl.empty()) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Get it")) ShellExecuteA(nullptr, "open", fmt.storeUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                    if (!fmt.needs.empty()) TextDim("    Needs: %s", fmt.needs.c_str());
                }
                ImGui::PopID();
            }
            if (ImGui::Button("Check for newly installed codecs")) {
                formats_.Detect();
                formats_.ApplyDisabled(settings_.disabledFormats);
                changed = true;
            }
            if (changed) {
                settings_.disabledFormats = formats_.DisabledNames();
                MarkSettingsDirty();
                ReloadFolder();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("About")) {
            const AdapterInfo& a = gpu_.Adapter();
            ImGui::Text("GPU: %s", a.name.c_str());
            TextDim("%s dedicated video memory%s", FormatBytes(a.dedicatedVram).c_str(),
                    a.unified ? ", shares system RAM" : "");
            ImGui::Text("RAM: %s", FormatBytes(live_.totalRam).c_str());
            ImGui::Text("Folder storage: %s (%d parallel read%s)", StorageName(storage_), ReadersFor(storage_),
                        ReadersFor(storage_) == 1 ? "" : "s");
            ImGui::Text("Workers: %d (1 reserved for the image on screen)", jobs_.ThreadCount());
            ImGui::Separator();
            ImGui::TextWrapped("Images are only ever held in RAM and video memory. The only file this program writes is "
                               "its settings file, and only when a setting changes:");
            TextDim("%s", ToUtf8(settingsPath_).c_str());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---- Sorting, info & histogram -----------------------------------------------------------

void App::DrawSortingTab() {
    bool changed = false;
    if (ImGui::Checkbox("Keep RAW + JPEG pairs together", &settings_.pairRawJpeg)) {
        changed = true;
        ReloadFolder();
    }
    HelpMarker("Files with the same name (IMG_1234.jpg, IMG_1234.CR2, IMG_1234.xmp) are shown once, and are deleted, "
               "moved and restored together.");
    changed |= ImGui::Checkbox("Rename when the destination already has that name", &settings_.renameOnConflict);
    HelpMarker("On: the moved file becomes \"IMG_1234 (2).jpg\". Off: the move is refused. Files are never overwritten.");

    ImGui::SeparatorText("Keys 1 - 9");
    TextDim("Empty = a subfolder named 1-9 next to the images. Relative paths are relative to the open folder.");
    if (ImGui::BeginTable("##dest", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("buttons", ImGuiTableColumnFlags_WidthFixed);
        for (int i = 0; i < 9; ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d", i + 1);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            char buf[1025];
            std::snprintf(buf, sizeof buf, "%s", settings_.destinations[size_t(i)].c_str());
            char hint[32];
            std::snprintf(hint, sizeof hint, "subfolder %d", i + 1);
            if (ImGui::InputTextWithHint("##path", hint, buf, sizeof buf)) {
                settings_.destinations[size_t(i)] = buf;
                changed = true;
            }
            if (!folder_.Folder().empty() && !settings_.destinations[size_t(i)].empty())
                TextDim("-> %s", ToUtf8(DestinationFor(i + 1)).c_str());
            ImGui::TableSetColumnIndex(2);
            if (ImGui::Button("Browse...")) pendingDestPick_ = i + 1;
            ImGui::SameLine();
            ImGui::BeginDisabled(settings_.destinations[size_t(i)].empty());
            if (ImGui::Button("Clear")) {
                settings_.destinations[size_t(i)].clear();
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    TextDim("Ctrl+Z undoes deletes and moves, newest first (%d in history, up to 50).", int(undo_.size()));
    if (changed) MarkSettingsDirty();
}

void App::DrawHistogram(const core::Histogram& h, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float bottom = p.y + size.y;
    dl->AddRectFilled(p, ImVec2(p.x + size.x, bottom), IM_COL32(12, 12, 15, 235), 3 * dpiScale_);
    // Scale by the inner bins so clipped spikes at 0/255 do not flatten the curve.
    uint32_t peak = 1;
    for (int i = 1; i < 255; ++i) peak = std::max({peak, h.r[size_t(i)], h.g[size_t(i)], h.b[size_t(i)], h.luma[size_t(i)]});
    const float bw = size.x / 256.0f;
    struct Channel {
        const std::array<uint32_t, 256>* bins;
        ImU32 color;
    } channels[] = {{&h.luma, IM_COL32(200, 200, 200, 70)},
                    {&h.r, IM_COL32(255, 70, 70, 95)},
                    {&h.g, IM_COL32(70, 220, 90, 95)},
                    {&h.b, IM_COL32(80, 120, 255, 95)}};
    for (const auto& ch : channels) {
        for (int i = 0; i < 256; ++i) {
            const float v = std::min(1.0f, float((*ch.bins)[size_t(i)]) / float(peak));
            if (v <= 0) continue;
            const float x = p.x + float(i) * bw;
            dl->AddRectFilled(ImVec2(x, bottom - v * size.y), ImVec2(x + bw + 0.5f, bottom), ch.color);
        }
    }
    ImGui::Dummy(size);
}

void App::DrawInfo() {
    const FileEntry* f = Active();
    if (!f) return;
    ImVec2 mn, mx;
    ViewArea(mn, mx);
    const float pad = 10 * dpiScale_;
    const bool overlayBottomRight = settings_.showOverlay && settings_.overlayCorner == 3;
    ImGui::SetNextWindowPos(ImVec2(mx.x - pad, overlayBottomRight ? pad : mx.y - pad), ImGuiCond_Always,
                            ImVec2(1.0f, overlayBottomRight ? 0.0f : 1.0f));
    ImGui::SetNextWindowBgAlpha(0.82f);
    bool open = true;
    ImGui::Begin("Info (I)", &open,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoCollapse);
    ImGui::TextUnformatted(ToUtf8(f->name).c_str());
    if (!f->companions.empty()) {
        bool raw = false, xmp = false;
        CompanionKinds(*f, raw, xmp);
        ImGui::SameLine();
        ImGui::TextColored(kAmber, "%s%s", raw ? "+RAW " : "", xmp ? "+XMP" : "");
    }
    const core::PhotoInfo& p = f->photo;
    const std::string camera = core::CameraLine(p), exposure = core::ExposureLine(p);
    if (!camera.empty()) ImGui::TextUnformatted(camera.c_str());
    if (!exposure.empty()) ImGui::TextUnformatted(exposure.c_str());
    if (!p.dateTaken.empty()) TextDim("Taken %s", core::FormatExifDate(p.dateTaken).c_str());
    if (f->probed && SUCCEEDED(f->probeHr)) {
        const core::Size s = f->Oriented();
        const ImageFormat* fmt = formats_.ForExtension(f->ext);
        TextDim("%u x %u  \xC2\xB7  %.1f MP  \xC2\xB7  %s  \xC2\xB7  %s%s", s.w, s.h, double(s.w) * s.h / 1e6,
                FormatBytes(f->size).c_str(), fmt ? fmt->name.c_str() : "", f->progressive ? " (progressive)" : "");
    }
    if (camera.empty() && exposure.empty() && f->probed) TextDim("No camera data in this file");

    // Histogram of the decoded image (computed when it was decoded; no extra work here).
    const Texture* tex = nullptr;
    auto fit = textures_.find({f->id, core::Variant::Fit});
    if (fit != textures_.end()) tex = &fit->second;
    if (!tex) tex = TextureFor(f->id);
    if (tex && tex->histogram) {
        const core::Histogram& h = *tex->histogram;
        DrawHistogram(h, ImVec2(std::round(256 * dpiScale_), std::round(90 * dpiScale_)));
        const float lo = h.LowPercent(), hi = h.HighPercent();
        ImGui::TextColored(lo >= 1.0f ? kRed : kDim, "Shadows clipped %.1f%%", double(lo));
        ImGui::SameLine();
        ImGui::TextColored(hi >= 1.0f ? kRed : kDim, "   Highlights clipped %.1f%%", double(hi));
    } else {
        TextDim("Histogram appears when the image has loaded");
    }
    ImGui::End();
    if (!open) {
        settings_.showInfo = false;
        MarkSettingsDirty();
    }
}

// ---- Stats & help ------------------------------------------------------------------------

void App::DrawCacheMap(float height) {
    const int n = folder_.Count();
    if (!n) return;
    core::WindowParams p;
    p.count = n;
    p.current = current_;
    p.direction = direction_;
    p.ahead = settings_.ahead;
    p.behind = settings_.behind;
    p.wrap = settings_.wrapAround;
    auto slots = core::ComputeWindow(p);
    std::sort(slots.begin(), slots.end(), [&](const core::WindowSlot& a, const core::WindowSlot& b) {
        return a.offset * direction_ < b.offset * direction_;  // left to right in folder order
    });
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    const float cell = height, gap = 3 * dpiScale_;
    for (const auto& s : slots) {
        const FileEntry& e = folder_.Files()[size_t(s.index)];
        const core::EntryKey k{e.id, core::Variant::Fit};
        ImVec4 c(0.25f, 0.25f, 0.27f, 1);
        if (sched_.IsProbing(e.id) || sched_.IsDecoding(k)) c = kAmber;
        if (sched_.IsResident(k)) c = kGreen;
        if (sched_.IsFailed(k) || (e.probed && FAILED(e.probeHr))) c = kRed;
        if (e.CloudOnly() && !sched_.IsResident(k)) c = kBlue;
        dl->AddRectFilled(pos, ImVec2(pos.x + cell, pos.y + cell), ImGui::ColorConvertFloat4ToU32(c), 2 * dpiScale_);
        if (s.rank == 0) dl->AddRect(pos, ImVec2(pos.x + cell, pos.y + cell), IM_COL32_WHITE, 2 * dpiScale_, 0, 2 * dpiScale_);
        pos.x += cell + gap;
    }
    ImGui::Dummy(ImVec2(float(slots.size()) * (cell + gap), cell));
}

void App::DrawStats() {
    ImVec2 mn, mx;
    ViewArea(mn, mx);
    ImGui::SetNextWindowPos(ImVec2(mn.x + 10 * dpiScale_, mx.y - 10 * dpiScale_), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGui::Begin("Stats (F3)", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoCollapse);
    const ProcessMemory pm = QueryProcessMemory();
    ImGui::Text("Cache map  (%s ahead)", direction_ > 0 ? "right" : "left");
    DrawCacheMap(12 * dpiScale_);
    TextDim("green ready  amber loading  red failed  blue online-only  grey not loaded");
    ImGui::Separator();
    if (caps_.unified) {
        ImGui::Text("Images: %s / %s (shared RAM)", FormatBytes(sched_.ResidentGpu()).c_str(), FormatBytes(limits_.ram).c_str());
    } else {
        ImGui::Text("Textures: %s / %s VRAM", FormatBytes(sched_.ResidentGpu()).c_str(), FormatBytes(limits_.gpu).c_str());
        if (live_.vramBudget)
            TextDim("Windows VRAM budget %s, process uses %s", FormatBytes(live_.vramBudget).c_str(),
                    FormatBytes(live_.vramUsage).c_str());
        ImGui::Text("Decoding: %s / %s RAM", FormatBytes(sched_.TransientInUse()).c_str(), FormatBytes(limits_.ram).c_str());
    }
    TextDim("System RAM free %s of %s (keeps %s for Windows)%s", FormatBytes(live_.availRam).c_str(),
            FormatBytes(live_.totalRam).c_str(), FormatBytes(core::RamHeadroom(live_.totalRam)).c_str(),
            live_.lowMemory ? "  LOW" : "");
    ImGui::Text("Process: %s in RAM, %s private", FormatBytes(pm.workingSet).c_str(), FormatBytes(pm.privateBytes).c_str());
    ImGui::Separator();
    ImGui::Text("Running: %d decode, %d probe", sched_.RunningDecodes(), sched_.RunningProbes());
    ImGui::Text("Decoded %llu (last %.0f ms), headers read %llu", (unsigned long long)decodes_, lastDecodeMs_,
                (unsigned long long)probes_);
    ImGui::Text("Disk: %llu files, %s read  |  %s, %d reader%s", (unsigned long long)filesRead_, FormatBytes(bytesRead_).c_str(),
                StorageName(storage_), ReadersFor(storage_), ReadersFor(storage_) == 1 ? "" : "s");
    ImGui::End();
}

void App::DrawHelp() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    ImGui::SetNextWindowPos(ImVec2(float(rc.right) * 0.5f, float(rc.bottom) * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::Begin("Keyboard & mouse", &showHelp_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
    static const char* rows[][2] = {
        {"Right / Down / Space / PgDn", "Next image"},
        {"Left / Up / Backspace / PgUp", "Previous image"},
        {"Home / End", "First / last image"},
        {"Mouse back / forward", "Previous / next"},
        {"Mouse wheel, + / -", "Zoom at cursor"},
        {"Drag", "Pan"},
        {"Double-click, Z", "Toggle 100% / fit"},
        {"C", "Compare 2 / 4 images side by side (synced zoom)"},
        {"Tab, click", "Pick the active image in compare mode"},
        {"0", "Fit to window"},
        {"F, F11, Alt+Enter", "Fullscreen"},
        {"Delete", "Move to Recycle Bin (with RAW/XMP pair)"},
        {"1 ... 9", "Move to destination 1 ... 9 (Settings > Sorting)"},
        {"Ctrl+Z", "Undo the last delete / move"},
        {"Ctrl+O / Ctrl+Shift+O", "Open image / folder"},
        {"F5", "Rescan folder"},
        {"O", "Overlay"},
        {"B", "Sidebar"},
        {"S", "Settings"},
        {"I", "Info: camera data and histogram"},
        {"F3", "Stats"},
        {"H, F1", "This help"},
        {"Esc", "Close panel / leave compare / leave fullscreen / quit"},
    };
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        for (const auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(r[0]);
            ImGui::TableSetColumnIndex(1);
            TextDim("%s", r[1]);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

}  // namespace fiv
