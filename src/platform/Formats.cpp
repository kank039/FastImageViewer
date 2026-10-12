#include "Formats.h"

#include <algorithm>

#include "WicCodec.h"

namespace fiv {

FormatRegistry::FormatRegistry() {
    formats_ = {
        {"JPEG", {L".jpg", L".jpeg", L".jpe", L".jfif"}, "", "", true, true},
        {"PNG", {L".png"}, "", "", true, true},
        {"BMP", {L".bmp", L".dib"}, "", "", true, true},
        {"GIF (first frame)", {L".gif"}, "", "", true, true},
        {"TIFF", {L".tif", L".tiff"}, "", "", true, true},
        {"ICO", {L".ico"}, "", "", true, true},
        {"JPEG XR", {L".jxr", L".wdp", L".hdp"}, "", "", true, true},
        {"DDS", {L".dds"}, "", "", true, true},
        {"WebP", {L".webp"}, "ms-windows-store://pdp/?ProductId=9PG2DK419DRG", "WebP Image Extensions", true},
        {"HEIC / HEIF", {L".heic", L".heif", L".hif"}, "ms-windows-store://pdp/?ProductId=9PMMSR1CGPWG",
         "HEIF Image Extensions (+ HEVC Video Extensions)", false},
        {"AVIF", {L".avif"}, "ms-windows-store://pdp/?ProductId=9MVZQVXJBQ9V", "AV1 Video Extension", false},
        {"JPEG XL", {L".jxl"}, "", "JPEG XL Image Extension (Windows 11 24H2+)", true},
        {"Camera RAW",
         {L".dng", L".cr2", L".cr3", L".nef", L".nrw", L".arw", L".orf", L".rw2", L".raf", L".pef", L".srw"},
         "ms-windows-store://pdp/?ProductId=9NCTDW2W1BH8",
         "Raw Image Extension",
         true},
    };
}

void FormatRegistry::Detect() {
    const auto exts = InstalledDecoderExtensions();
    for (auto& f : formats_) {
        f.installed = f.builtIn || std::any_of(f.extensions.begin(), f.extensions.end(),
                                               [&](const std::wstring& e) { return exts.count(e) != 0; });
        if (!f.installed) f.enabled = false;
    }
}

void FormatRegistry::ApplyDisabled(const std::vector<std::string>& disabled) {
    for (auto& f : formats_)
        f.enabled = f.installed && std::find(disabled.begin(), disabled.end(), f.name) == disabled.end();
}

std::vector<std::string> FormatRegistry::DisabledNames() const {
    std::vector<std::string> out;
    for (const auto& f : formats_)
        if (f.installed && !f.enabled) out.push_back(f.name);
    return out;
}

const ImageFormat* FormatRegistry::ForExtension(const std::wstring& ext) const {
    for (const auto& f : formats_)
        if (std::find(f.extensions.begin(), f.extensions.end(), ext) != f.extensions.end()) return &f;
    return nullptr;
}

bool FormatRegistry::IsEnabled(const std::wstring& ext) const {
    const ImageFormat* f = ForExtension(ext);
    return f && f->enabled;
}

}  // namespace fiv
