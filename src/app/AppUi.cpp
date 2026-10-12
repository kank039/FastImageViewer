// All Dear ImGui drawing. Rendering is event driven: nothing here animates,
// so an idle viewer draws no frames at all.
#include "App.h"

#include "../platform/Shell.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <climits>
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

// Draws a texture into [pmin, pmax] turned `rot` quarter turns clockwise.
void AddRotatedImage(ImDrawList* dl, ImTextureID tex, ImVec2 pmin, ImVec2 pmax, int rot) {
    if ((rot & 3) == 0) {
        dl->AddImage(tex, pmin, pmax);
        return;
    }
    // Texture coordinates shown at the top-left, top-right, bottom-right and bottom-left corners.
    static const ImVec2 kUv[4][4] = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}},
                                     {{0, 1}, {0, 0}, {1, 0}, {1, 1}},
                                     {{1, 1}, {0, 1}, {0, 0}, {1, 0}},
                                     {{1, 0}, {1, 1}, {0, 1}, {0, 0}}};
    const ImVec2* uv = kUv[rot & 3];
    dl->AddImageQuad(tex, pmin, ImVec2(pmax.x, pmin.y), pmax, ImVec2(pmin.x, pmax.y), uv[0], uv[1], uv[2], uv[3]);
}

core::Size Turned(core::Size s, int rot) { return rot & 1 ? core::Size{s.h, s.w} : s; }

void AddStar(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        const float a = -1.5707963f + float(i) * 0.62831853f;  // 10 points, 36 degrees apart
        const float rr = (i & 1) ? r * 0.45f : r;
        pts[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
    }
    // A fan of triangles from the centre (the star itself is not convex);
    // without anti-aliasing so the seams between them do not show.
    const ImDrawListFlags saved = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (int i = 0; i < 10; ++i) dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], col);
    dl->Flags = saved;
}

void SetPeakingShader(const ImDrawList*, const ImDrawCmd* cmd) {
    auto* gpu = static_cast<Gpu*>(cmd->UserCallbackData);
    gpu->Context()->PSSetShader(gpu->PeakingShader(), nullptr, 0);
}

// Fits `content` into a box, centred.
void FitInBox(core::Size content, ImVec2 b0, float box, ImVec2& o0, ImVec2& o1) {
    const float a = float(content.w) / float(std::max(1u, content.h));
    const ImVec2 sz = a >= 1 ? ImVec2(box, box / a) : ImVec2(box * a, box);
    o0 = ImVec2(std::round(b0.x + (box - sz.x) * 0.5f), std::round(b0.y + (box - sz.y) * 0.5f));
    o1 = ImVec2(o0.x + sz.x, o0.y + sz.y);
}

void SetPointSampler(const ImDrawList*, const ImDrawCmd* cmd) {
    auto* gpu = static_cast<Gpu*>(cmd->UserCallbackData);
    ID3D11SamplerState* s = gpu->PointSampler();
    gpu->Context()->PSSetSamplers(0, 1, &s);
}

}  // namespace

// ---- Marks ------------------------------------------------------------------------

float App::MarkBadgeWidth(float size, const core::Mark& m) {
    float w = m.flag != core::Flag::None ? size : 0.0f;
    if (m.stars) w += (w > 0 ? size * 0.3f : 0.0f) + float(m.stars) * size * 0.95f;
    return w;
}

float App::DrawMarkBadge(ImDrawList* dl, ImVec2 pos, float size, const core::Mark& m) {
    float x = pos.x;
    const float t = std::max(1.5f, size * 0.14f);
    if (m.flag == core::Flag::Pick) {
        dl->AddRectFilled(pos, ImVec2(x + size, pos.y + size), IM_COL32(40, 150, 70, 235), size * 0.2f);
        dl->AddLine(ImVec2(x + size * 0.24f, pos.y + size * 0.52f), ImVec2(x + size * 0.43f, pos.y + size * 0.72f), IM_COL32_WHITE, t);
        dl->AddLine(ImVec2(x + size * 0.43f, pos.y + size * 0.72f), ImVec2(x + size * 0.78f, pos.y + size * 0.3f), IM_COL32_WHITE, t);
        x += size;
    } else if (m.flag == core::Flag::Reject) {
        dl->AddRectFilled(pos, ImVec2(x + size, pos.y + size), IM_COL32(190, 50, 50, 235), size * 0.2f);
        dl->AddLine(ImVec2(x + size * 0.28f, pos.y + size * 0.28f), ImVec2(x + size * 0.72f, pos.y + size * 0.72f), IM_COL32_WHITE, t);
        dl->AddLine(ImVec2(x + size * 0.72f, pos.y + size * 0.28f), ImVec2(x + size * 0.28f, pos.y + size * 0.72f), IM_COL32_WHITE, t);
        x += size;
    }
    if (m.stars) {
        if (x > pos.x) x += size * 0.3f;
        for (int i = 0; i < m.stars; ++i) {
            AddStar(dl, ImVec2(x + size * 0.475f, pos.y + size * 0.53f), size * 0.5f, IM_COL32(250, 190, 40, 255));
            x += size * 0.95f;
        }
    }
    return x - pos.x;
}

// ---- Image ------------------------------------------------------------------------

void App::DrawImage() {
    if (!Current()) {
        DrawEmpty();
        return;
    }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const bool compare = compareCount_ > 1;
    // In compare mode the sharpest image of the set is marked (once all are scored).
    int sharpestSlot = -1;
    if (compare) {
        float best = -1;
        bool all = true;
        for (const Tile& t : Tiles()) {
            if (t.index < 0) continue;
            const float sc = FileAt(t.index).sharpness;
            all &= sc >= 0;
            if (sc > best) {
                best = sc;
                sharpestSlot = t.slot;
            }
        }
        if (!all || VisibleTiles() < 2) sharpestSlot = -1;
    }
    const bool peaking = peaking_ && gpu_.PeakingShader();
    for (const Tile& tile : Tiles()) {
        if (tile.index < 0) continue;
        const FileEntry& f = FileAt(tile.index);
        const ViewGeometry g = GeometryFor(tile.index, tile.min, tile.max);
        const ImVec2 vc((g.viewMin.x + g.viewMax.x) * 0.5f, (g.viewMin.y + g.viewMax.y) * 0.5f);

        const Texture* tex = anim_.item == f.id && animTex_.srv ? &animTex_ : TextureFor(f.id);
        auto thumbIt = textures_.find({f.id, core::Variant::Thumb});
        const Texture* shown = tex                                        ? tex
                               : previewItem_ == f.id && preview_.srv     ? &preview_
                               : thumbIt != textures_.end()               ? &thumbIt->second
                                                                          : nullptr;
        const int rot = RotationOf(f);
        if (shown && !g.source.empty()) {
            const ImVec2 size(float(g.source.w) * g.zoom, float(g.source.h) * g.zoom);
            const ImVec2 pmin(std::round(vc.x - g.center.x * g.zoom), std::round(vc.y - g.center.y * g.zoom));
            const ImVec2 pmax(pmin.x + size.x, pmin.y + size.y);
            const core::Size texSize = Turned(shown->size, rot);  // as displayed
            const float texelScale = size.x / float(std::max(1u, texSize.w));  // screen pixels per texel
            const bool point = settings_.pixelatedZoom && shown == tex && texelScale > 1.01f;
            dl->PushClipRect(g.viewMin, g.viewMax, false);
            if (point) dl->AddCallback(SetPointSampler, &gpu_);
            if (peaking && shown == tex) dl->AddCallback(SetPeakingShader, &gpu_);
            AddRotatedImage(dl, ToTex(shown->srv.Get()), pmin, pmax, rot);
            if (point || (peaking && shown == tex)) dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
            // The 1:1 detail of a huge image, over the part it covers.
            auto detail = textures_.find({f.id, core::Variant::Detail});
            if (!compare && rot == 0 && detail != textures_.end() && !detail->second.region.empty()) {
                const core::Rect& r = detail->second.region;
                const ImVec2 d0(pmin.x + float(r.x) * g.zoom, pmin.y + float(r.y) * g.zoom);
                const ImVec2 d1(d0.x + float(r.w) * g.zoom, d0.y + float(r.h) * g.zoom);
                const bool dpoint = settings_.pixelatedZoom && g.zoom > 1.01f;
                if (dpoint) dl->AddCallback(SetPointSampler, &gpu_);
                if (peaking) dl->AddCallback(SetPeakingShader, &gpu_);
                dl->AddImage(ToTex(detail->second.srv.Get()), d0, d1);
                if (dpoint || peaking) dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
            }
            dl->PopClipRect();

            // Zoomed past the screen-sized texture: fetch full-resolution versions of the visible images.
            if (tex && !wantFull_ && texelScale > 1.02f && Turned(tex->size, rot).w < g.source.w &&
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
            if (tile.slot == sharpestSlot) {
                const char* label = "Sharpest";
                const ImVec2 ls = ImGui::CalcTextSize(label);
                const ImVec2 l0(tile.max.x - 3 * pad - ls.x, tile.min.y + pad);
                dl->AddRectFilled(l0, ImVec2(tile.max.x - pad, l0.y + ls.y + pad), IM_COL32(40, 150, 70, 230), 4 * dpiScale_);
                dl->AddText(ImVec2(l0.x + pad, l0.y + pad * 0.5f), IM_COL32_WHITE, label);
            }
            const core::Mark mark = MarkOf(f);
            if (!mark.empty()) {
                const float bs = ts.y;
                const float bw = MarkBadgeWidth(bs, mark);
                const ImVec2 b0(tile.min.x + pad, tile.min.y + 3 * pad + ts.y);
                dl->AddRectFilled(b0, ImVec2(b0.x + bw + 2 * pad, b0.y + bs + pad), IM_COL32(0, 0, 0, 150), 4 * dpiScale_);
                DrawMarkBadge(dl, ImVec2(b0.x + pad, b0.y + pad * 0.5f), bs, mark);
            }
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
    if (folder_.Count() > 0 && filter_ != core::Filter::All) {
        ImGui::Text("No images match \"%s\"", core::FilterName(filter_));
        TextDim("Ctrl+F changes the filter, Esc shows everything");
        ImGui::Spacing();
        if (ImGui::Button("Show all images")) SetFilter(core::Filter::All);
        ImGui::End();
        return;
    }
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
    if (slideshow_) ImGui::TextColored(kBlue, "Slideshow  \xC2\xB7  %d s", settings_.slideshowSeconds);
    if (settings_.ovName) ImGui::TextUnformatted(ToUtf8(f->name).c_str());
    if (settings_.ovName && f->rel.size() > f->name.size())
        TextDim("in %s", ToUtf8(f->rel.substr(0, f->rel.size() - f->name.size() - 1)).c_str());
    if (peaking_) ImGui::TextColored(kRed, "Focus peaking");
    if (anim_.playing && anim_.item == f->id) TextDim("Animated  \xC2\xB7  frame %u / %u", anim_.frame + 1, anim_.frames);
    if (const core::Mark mark = MarkOf(*f); !mark.empty()) {
        const float bs = ImGui::GetTextLineHeight();
        const float bw = DrawMarkBadge(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), bs, mark);
        ImGui::Dummy(ImVec2(bw, bs));
    }
    if (filter_ != core::Filter::All) ImGui::TextColored(kBlue, "Showing %s", core::FilterName(filter_));
    if (!f->companions.empty()) {
        bool raw = false, xmp = false;
        CompanionKinds(*f, raw, xmp);
        ImGui::TextColored(kAmber, "%s%s", raw ? "+RAW " : "", xmp ? "+XMP" : "");
    }
    if (settings_.ovType) {
        const ImageFormat* fmt = formats_.ForExtension(f->ext);
        TextDim("%s", fmt ? fmt->name.c_str() : ToUtf8(f->ext).c_str());
    }
    if (settings_.ovIndex) TextDim("%d / %d", ActiveIndex() + 1, ViewCount());
    if (settings_.ovSize) TextDim("%s", FormatBytes(f->size).c_str());
    if (settings_.ovDims && f->probed && SUCCEEDED(f->probeHr)) TextDim("%u x %u", f->Oriented().w, f->Oriented().h);
    if (settings_.ovZoom && !g.source.empty()) TextDim("%.0f%%", g.zoom * 100.0f);
    if (const int rot = RotationOf(*f)) TextDim("Rotated %d\xC2\xB0 (view only)", rot * 90);
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
    if (filter_ != core::Filter::All) ImGui::TextColored(kBlue, "Showing %s: %d", core::FilterName(filter_), ViewCount());
    if (NeedsScan() && scanLeft_ > 0) ImGui::TextColored(kAmber, "Reading capture dates: %d left", scanLeft_);
    TextDim("Cache %s of %s %s", FormatBytes(sched_.ResidentGpu()).c_str(), FormatBytes(limits_.gpu).c_str(),
            caps_.unified ? "(shared RAM)" : "VRAM");
    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginChild("##list", ImVec2(0, 0), false, ImGuiWindowFlags_NoNav);
    const float rowH = std::round((float(settings_.thumbHeight) + 8) * dpiScale_);
    const int n = ViewCount();
    if (scrollSidebarToCurrent_ && n) {
        const float target = float(current_) * rowH - (ImGui::GetWindowHeight() - rowH) * 0.5f;
        ImGui::SetScrollY(std::max(0.0f, target));
        scrollSidebarToCurrent_ = false;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = 4 * dpiScale_, box = rowH - 2 * pad;
    ImGuiListClipper clip;
    clip.Begin(n, rowH);
    int firstRow = INT_MAX, lastRow = -1;
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            firstRow = std::min(firstRow, i);
            lastRow = std::max(lastRow, i);
            const FileEntry& e = FileAt(i);
            ImGui::PushID(i);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float rowW = ImGui::GetContentRegionAvail().x;
            bool shown = false;
            for (int t = 0; t < TileCount(); ++t) shown |= TileIndex(t) == i;
            if (ImGui::Selectable("##row", shown, 0, ImVec2(0, rowH))) JumpTo(i, i >= current_ ? 1 : -1);

            const ImVec2 b0(p.x + pad, p.y + pad), b1(b0.x + box, b0.y + box);
            if (i < int(burst_.size()) && burst_[size_t(i)] >= 0)  // burst members share a coloured bar
                dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + 3 * dpiScale_, p.y + rowH),
                                  burst_[size_t(i)] % 2 ? IM_COL32(230, 160, 40, 255) : IM_COL32(70, 140, 255, 255));
            if (const Texture* t = ThumbFor(e.id)) {
                const int rot = RotationOf(e);
                ImVec2 o0, o1;
                FitInBox(Turned(t->size, rot), b0, box, o0, o1);
                AddRotatedImage(dl, ToTex(t->srv.Get()), o0, o1, rot);
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
            dl->AddText(ImVec2(tx, p.y + pad), ImGui::GetColorU32(ImGuiCol_Text), ToUtf8(e.rel).c_str());
            if (const core::Mark mark = MarkOf(e); !mark.empty()) {  // badge on its own line under the details
                const float bs = std::round(ImGui::GetTextLineHeight() * 0.85f);
                const float by = std::min(p.y + pad + 2 * ImGui::GetTextLineHeight(), p.y + rowH - bs - pad);
                DrawMarkBadge(dl, ImVec2(tx, by), bs, mark);
            }
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
    if (lastRow >= 0 && (firstRow != sidebarFirst_ || lastRow != sidebarLast_)) {
        sidebarFirst_ = firstRow;
        sidebarLast_ = lastRow;
        UpdateCache();  // thumbnails for the rows now in view
    }
}

// ---- Grid ------------------------------------------------------------------------------

void App::DrawGrid() {
    ImVec2 mn, mx;
    ViewArea(mn, mx);
    ImGui::SetNextWindowPos(mn);
    ImGui::SetNextWindowSize(ImVec2(mx.x - mn.x, mx.y - mn.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("##grid", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    const int n = ViewCount();
    if (n == 0) {
        ImGui::End();
        DrawEmpty();
        return;
    }
    const float cell = std::round(float(settings_.gridSize) * dpiScale_), gap = std::round(8 * dpiScale_);
    const float lineH = ImGui::GetTextLineHeight();
    const float cellH = cell + lineH + gap * 0.5f;
    const int cols = std::max(1, int((ImGui::GetContentRegionAvail().x + gap) / (cell + gap)));
    gridCols_ = cols;
    const int rows = (n + cols - 1) / cols;
    if (scrollGridToCurrent_) {
        scrollGridToCurrent_ = false;
        const float rowTop = float(current_ / cols) * (cellH + gap), viewH = ImGui::GetWindowHeight();
        const float y = ImGui::GetScrollY();
        if (rowTop < y)
            ImGui::SetScrollY(rowTop);
        else if (rowTop + cellH > y + viewH - gap)
            ImGui::SetScrollY(rowTop + cellH - viewH + 2 * gap);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    int first = INT_MAX, last = -1;
    int open = -1;
    ImGuiListClipper clip;
    clip.Begin(rows, cellH + gap);
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            for (int c = 0; c < cols; ++c) {
                const int i = r * cols + c;
                if (i >= n) break;
                if (c) ImGui::SameLine();
                first = std::min(first, i);
                last = std::max(last, i);
                const FileEntry& e = FileAt(i);
                ImGui::PushID(i);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##cell", ImVec2(cell, cellH));
                if (ImGui::IsItemClicked()) JumpTo(i, i >= current_ ? 1 : -1);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) open = i;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ToUtf8(e.rel).c_str());
                const bool selected = i == current_;
                dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell),
                                  selected ? IM_COL32(45, 70, 115, 255) : IM_COL32(32, 32, 36, 235), 4 * dpiScale_);
                if (const Texture* t = ThumbFor(e.id)) {
                    const int rot = RotationOf(e);
                    ImVec2 o0, o1;
                    FitInBox(Turned(t->size, rot), ImVec2(p.x + gap * 0.5f, p.y + gap * 0.5f), cell - gap, o0, o1);
                    AddRotatedImage(dl, ToTex(t->srv.Get()), o0, o1, rot);
                } else {
                    const char* msg = !e.error.empty() ? "Cannot open" : e.CloudOnly() && settings_.skipCloudFiles ? "Online only" : "";
                    if (*msg) {
                        const ImVec2 ts = ImGui::CalcTextSize(msg);
                        dl->AddText(ImVec2(p.x + (cell - ts.x) * 0.5f, p.y + (cell - ts.y) * 0.5f),
                                    ImGui::ColorConvertFloat4ToU32(e.error.empty() ? kBlue : kRed), msg);
                    }
                }
                if (selected)
                    dl->AddRect(p, ImVec2(p.x + cell, p.y + cell), IM_COL32(70, 140, 255, 255), 4 * dpiScale_, 0, 2 * dpiScale_);
                if (i < int(burst_.size()) && burst_[size_t(i)] >= 0)
                    dl->AddRectFilled(ImVec2(p.x, p.y + cell - 3 * dpiScale_), ImVec2(p.x + cell, p.y + cell),
                                      burst_[size_t(i)] % 2 ? IM_COL32(230, 160, 40, 255) : IM_COL32(70, 140, 255, 255));
                if (const core::Mark mark = MarkOf(e); !mark.empty()) {
                    const float bs = std::round(lineH * 0.9f);
                    const ImVec2 b0(p.x + gap * 0.5f, p.y + cell - bs - gap * 0.5f);
                    dl->AddRectFilled(ImVec2(b0.x - 2, b0.y - 2), ImVec2(b0.x + MarkBadgeWidth(bs, mark) + 2, b0.y + bs + 2),
                                      IM_COL32(0, 0, 0, 160), 3 * dpiScale_);
                    DrawMarkBadge(dl, b0, bs, mark);
                }
                dl->PushClipRect(ImVec2(p.x, p.y + cell), ImVec2(p.x + cell, p.y + cellH), true);
                dl->AddText(ImVec2(p.x + 2, p.y + cell + 2), selected ? IM_COL32_WHITE : ImGui::ColorConvertFloat4ToU32(kDim),
                            ToUtf8(e.name).c_str());
                dl->PopClipRect();
                ImGui::PopID();
            }
        }
    }
    ImGui::PopStyleVar();
    ImGui::End();
    if (last >= 0 && (first != gridFirst_ || last != gridLast_)) {
        gridFirst_ = first;
        gridLast_ = last;
        UpdateCache();
    }
    if (open >= 0) {
        JumpTo(open, 1);
        ToggleGrid();
    }
}

// ---- Settings --------------------------------------------------------------------------

void App::DrawSettings() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    ImGui::SetNextWindowSize(ImVec2(std::round(620 * dpiScale_), std::min(std::round(580 * dpiScale_), float(rc.bottom) * 0.9f)),
                             ImGuiCond_Appearing);
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
            const char* colorModes[] = {"Off (show pixels as stored)", "Convert embedded profiles to sRGB (recommended)",
                                        "Convert to this display's profile"};
            if (ImGui::Combo("Colour management", &settings_.colorMode, colorModes, 3)) {
                changed = true;
                FlushCache();
            }
            HelpMarker("Photos with an embedded colour profile (Adobe RGB, Display P3, ProPhoto) look washed out or "
                       "oversaturated unless converted. \"This display's profile\" also corrects for a calibrated or "
                       "wide-gamut monitor (Windows colour management > device profile). HDR images are tone-mapped "
                       "so bright highlights keep their detail.");
            if (settings_.colorMode == 2)
                TextDim("Display profile: %s", displayProfile_.empty() ? "none assigned (sRGB is used)"
                                                                       : ToUtf8(displayProfile_).c_str());
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
        if (ImGui::BeginTabItem("Controls")) {
            DrawControlsTab();
            ImGui::EndTabItem();
        } else if (recordingAction_ >= 0) {
            recordingAction_ = -1;  // left the tab while it was waiting for a key
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
    ImGui::SeparatorText("Order");
    const char* keys[] = {"Name", "Date taken", "Date modified", "File size", "File type"};
    ImGui::SetNextItemWidth(std::round(200 * dpiScale_));
    bool sort = ImGui::Combo("Sort by", &settings_.sortMode, keys, IM_ARRAYSIZE(keys));
    ImGui::SameLine();
    sort |= ImGui::Checkbox("Descending", &settings_.sortDescending);
    if (sort) OnSortChanged();
    HelpMarker("Date taken reads the capture time from each file's header (a small read per image, in the background, "
               "at low priority). Images without one use their modified date.");
    if (ImGui::Checkbox("Include subfolders", &settings_.includeSubfolders)) {
        MarkSettingsDirty();
        ApplyFolderOptions();
    }
    HelpMarker("Shows the images of every subfolder too (up to 8 levels, 50,000 images). The folders of keys 1-9 are "
               "left out, so sorted images do not come back.");
    ImGui::SetNextItemWidth(std::round(200 * dpiScale_));
    if (ImGui::SliderInt("Burst: shots within", &settings_.burstGapSeconds, 1, 30, "%d s", ImGuiSliderFlags_AlwaysClamp)) {
        changed = true;
        UpdateBursts();
    }
    HelpMarker("Consecutive shots from the same camera this close together form a burst: a coloured bar in the sidebar "
               "and grid, and Shift+C compares them side by side with the sharpest one marked.");
    ImGui::SetNextItemWidth(std::round(200 * dpiScale_));
    if (ImGui::SliderInt("Grid cell size", &settings_.gridSize, 96, 400, "%d px", ImGuiSliderFlags_AlwaysClamp)) {
        changed = true;
        UpdateCache();
    }

    ImGui::SeparatorText("Pairs");
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
    if (f->sharpness >= 0) TextDim("Focus score %.0f (higher is sharper; compare shots of the same scene)", double(f->sharpness));

    // Histogram of the decoded image (computed when it was decoded; no extra work here).
    const Texture* tex = nullptr;
    auto fit = textures_.find({f->id, core::Variant::Fit});
    if (fit != textures_.end()) tex = &fit->second;
    if (!tex) tex = TextureFor(f->id);
    if (tex && (tex->hdr || tex->colorConverted))
        TextDim("%s", tex->hdr ? "HDR image, tone-mapped for this screen" : "Colours converted from the embedded profile");
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

// ---- Marks panel ----------------------------------------------------------------------------

void App::DrawMarksPanel() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    ImGui::SetNextWindowPos(ImVec2(float(rc.right) - 16 * dpiScale_, 16 * dpiScale_), ImGuiCond_Appearing, ImVec2(1, 0));
    if (!ImGui::Begin("Marks (Shift+P)", &showMarks_,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }
    int picks = 0, rejects = 0, rated = 0, marked = 0, stars[6] = {};
    for (const FileEntry& f : folder_.Files()) {
        const core::Mark m = MarkOf(f);
        marked += !m.empty();
        picks += m.flag == core::Flag::Pick;
        rejects += m.flag == core::Flag::Reject;
        rated += m.stars > 0;
        ++stars[m.stars];
    }
    ImGui::Text("Picked %d    Rejected %d    Rated %d    of %d", picks, rejects, rated, folder_.Count());
    if (rated) TextDim("5 stars %d   4: %d   3: %d   2: %d   1: %d", stars[5], stars[4], stars[3], stars[2], stars[1]);
    TextDim("P pick, X reject, U clear, Ctrl+1-5 stars. Kept in memory for this session only;");
    TextDim("nothing is written to your files.");
    bool changed = ImGui::Checkbox("Show the next image after marking", &settings_.autoAdvance);

    ImGui::SeparatorText("Show (Ctrl+F)");
    for (int i = 0; i < int(core::Filter::Count); ++i) {
        if (i % 3) ImGui::SameLine();
        if (ImGui::RadioButton(core::FilterName(core::Filter(i)), int(filter_) == i)) SetFilter(core::Filter(i));
    }

    ImGui::SeparatorText("Apply to marked images");
    std::vector<std::string> selLabels;
    for (int i = 0; i < int(core::Selection::Count); ++i)
        selLabels.push_back(std::string(core::SelectionName(core::Selection(i))) + " (" +
                            std::to_string(SelectionIndices(core::Selection(i)).size()) + ")");
    const float comboW = std::round(260 * dpiScale_);
    ImGui::SetNextItemWidth(comboW);
    if (ImGui::BeginCombo("Images", selLabels[size_t(marksSelection_)].c_str())) {
        for (int i = 0; i < int(selLabels.size()); ++i)
            if (ImGui::Selectable(selLabels[size_t(i)].c_str(), i == marksSelection_)) {
                marksSelection_ = i;
                confirmBatch_ = false;
            }
        ImGui::EndCombo();
    }
    auto actionLabel = [&](int a) {
        if (a == 0) return std::string("Move to the Recycle Bin");
        if (a == 10) return std::string("Copy their file paths");
        return "Move to " + std::to_string(a) + ": " + DestinationLabel(a);
    };
    ImGui::SetNextItemWidth(comboW);
    if (ImGui::BeginCombo("Action", actionLabel(marksAction_).c_str())) {
        for (int a : {1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 10})
            if (ImGui::Selectable(actionLabel(a).c_str(), a == marksAction_)) {
                marksAction_ = a;
                confirmBatch_ = false;
            }
        ImGui::EndCombo();
    }
    const core::Selection sel = core::Selection(marksSelection_);
    const std::vector<int> chosen = SelectionIndices(sel);
    ImGui::BeginDisabled(chosen.empty() || folder_.Folder().empty());
    if (!confirmBatch_) {
        char label[64];
        std::snprintf(label, sizeof label, "Apply to %d image%s", int(chosen.size()), chosen.size() == 1 ? "" : "s");
        if (ImGui::Button(label)) {
            if (marksAction_ == 10) {
                std::wstring text;
                for (int i : chosen) {
                    const FileEntry& f = folder_.Files()[size_t(i)];
                    text += f.path + L"\r\n";
                    for (const auto& c : f.companions) text += c + L"\r\n";
                }
                Toast(CopyTextToClipboard(hwnd_, text) ? "Copied " + std::to_string(chosen.size()) + " paths"
                                                       : "Could not open the clipboard");
            } else {
                confirmBatch_ = true;
            }
        }
    } else {
        const std::string count = std::to_string(chosen.size()) + (chosen.size() == 1 ? " image" : " images");
        const std::string where = marksAction_ == 0 ? std::string("the Recycle Bin") : DestinationLabel(marksAction_);
        ImGui::TextColored(kAmber, "Move %s to %s?  Paired RAW/XMP files go with them.", count.c_str(), where.c_str());
        if (ImGui::Button("Yes, do it")) {
            pendingBatch_ = {sel, marksAction_};
            pendingBatchSet_ = true;
            confirmBatch_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) confirmBatch_ = false;
    }
    ImGui::EndDisabled();
    TextDim("Ctrl+Z undoes a whole batch in one step.");

    ImGui::Separator();
    ImGui::BeginDisabled(marked == 0);
    if (!confirmClearMarks_) {
        if (ImGui::Button("Clear all marks in this folder")) confirmClearMarks_ = true;
    } else {
        ImGui::TextColored(kAmber, "Clear the marks of %d image%s?", marked, marked == 1 ? "" : "s");
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            for (const FileEntry& f : folder_.Files()) marks_.erase(ToLower(f.path));
            confirmClearMarks_ = false;
            SyncCurrentIndex();
            UpdateCache();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep")) confirmClearMarks_ = false;
    }
    ImGui::EndDisabled();
    ImGui::End();
    if (changed) MarkSettingsDirty();
}

// ---- Stats & help ------------------------------------------------------------------------

void App::DrawCacheMap(float height) {
    const int n = ViewCount();
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
        const FileEntry& e = FileAt(s.index);
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
    if (detailItem_) {
        const core::EntryKey dk{detailItem_, core::Variant::Detail};
        const core::Rect& r = detailPlan_.region;
        TextDim("Detail %u x %u at %u,%u: %s", r.w, r.h, r.x, r.y,
                sched_.IsResident(dk) ? "shown" : sched_.IsDecoding(dk) ? "decoding" : sched_.IsFailed(dk) ? "failed" : "waiting");
        if (sched_.IsFailed(dk) && Current() && !Current()->error.empty()) TextDim("  %s", Current()->error.c_str());
    }
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
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, float(rc.bottom) * 0.85f));
    ImGui::Begin("Keyboard & mouse", &showHelp_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
    TextDim("Keys can be changed in Settings > Controls.");
    static const char* mouse[][2] = {
        {"Mouse back / forward", "Previous / next image"},
        {"Wheel", "Zoom at the cursor (or change image: Settings > Controls)"},
        {"Ctrl+wheel, pinch", "Zoom"},
        {"Tilt wheel, two-finger swipe", "Previous / next (pans when zoomed in)"},
        {"Drag, one-finger drag", "Pan"},
        {"Double-click, two-finger tap", "Toggle 100% / fit"},
        {"Click a tile", "Pick the active image in compare mode"},
        {"Grid: arrows, Enter, double-click", "Move, open the image (+ / - change the cell size)"},
        {"Esc", "Close panel / stop slideshow / leave compare / leave fullscreen / quit"},
    };
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        auto row = [](const char* keys, const char* what, bool dim) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (dim)
                TextDim("%s", keys);
            else
                ImGui::TextUnformatted(keys);
            ImGui::TableSetColumnIndex(1);
            TextDim("%s", what);
        };
        for (const auto& r : mouse) row(r[0], r[1], false);
        for (const auto& info : core::AllActions()) {
            const auto& chords = keymap_.Bindings(info.action);
            const std::string keys = chords.empty() ? std::string("(none)") : core::ChordsToString(chords);
            row(keys.c_str(), info.label, chords.empty());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void App::DrawControlsTab() {
    bool changed = false;
    ImGui::SeparatorText("Mouse, touchpad & touch");
    int wheel = settings_.wheelNavigates ? 1 : 0;
    changed |= ImGui::RadioButton("Wheel zooms", &wheel, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Wheel changes image", &wheel, 1);
    settings_.wheelNavigates = wheel == 1;
    HelpMarker("Ctrl+wheel and touchpad pinch always zoom. Tilting the wheel or swiping sideways with two fingers "
               "changes the image (or pans when zoomed in). On a touch screen: pinch to zoom, swipe sideways to change "
               "image, drag to pan, two-finger tap for 100%.");

    ImGui::SeparatorText("Slideshow");
    changed |= ImGui::SliderInt("Seconds per image", &settings_.slideshowSeconds, 1, 120, "%d s", ImGuiSliderFlags_AlwaysClamp);
    HelpMarker("Each image gets its full time once it is on screen. Stops at the last image unless wrap-around is on.");

    ImGui::SeparatorText("External editor");
    char buf[1025];
    std::snprintf(buf, sizeof buf, "%s", settings_.editorPath.c_str());
    ImGui::SetNextItemWidth(-std::round(150 * dpiScale_));
    if (ImGui::InputTextWithHint("##editor", "Windows \"Open with\" chooser", buf, sizeof buf)) {
        settings_.editorPath = buf;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) pendingEditorPick_ = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(settings_.editorPath.empty());
    if (ImGui::Button("Clear##editor")) {
        settings_.editorPath.clear();
        changed = true;
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Keys");
    TextDim("+ then press a key, with Ctrl / Shift / Alt if you like. Esc cancels; Esc itself is fixed.");
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    // Fill the rest of the window, leaving room for the button below.
    const float tableH = std::max(std::round(160 * dpiScale_),
                                  ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing());
    if (ImGui::BeginTable("##keymap", 3, flags, ImVec2(0, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("##edit", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (const auto& info : core::AllActions()) {
            const core::Action a = info.action;
            const auto& chords = keymap_.Bindings(a);
            ImGui::PushID(int(a));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(info.label);
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            if (recordingAction_ == int(a))
                ImGui::TextColored(kBlue, "Press a key...");
            else if (chords.empty())
                TextDim("none");
            else
                ImGui::TextUnformatted(core::ChordsToString(chords).c_str());
            const auto clash = keymap_.Conflicts(a, chords);
            if (!clash.empty()) ImGui::TextColored(kAmber, "also \"%s\" (the first in this list wins)", core::InfoFor(clash[0]).label);
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton("+")) recordingAction_ = int(a);
            ImGui::SameLine();
            ImGui::BeginDisabled(chords.empty());
            if (ImGui::SmallButton("Clear")) SetBindings(a, {});
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!settings_.keyBindings.count(info.id));
            if (ImGui::SmallButton("Default")) {
                settings_.keyBindings.erase(info.id);
                RebuildKeyMap();
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(settings_.keyBindings.empty());
    if (ImGui::Button("Restore all default keys")) {
        settings_.keyBindings.clear();
        RebuildKeyMap();
        changed = true;
    }
    ImGui::EndDisabled();
    if (changed) MarkSettingsDirty();
}

}  // namespace fiv
