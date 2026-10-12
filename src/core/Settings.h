#pragma once
// User settings: plain data, validation against hardware caps, and a tiny
// key=value text format. File I/O lives in the platform layer.
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "MemoryLimits.h"

namespace fiv::core {

enum class DecodeMode : int { FitScreen = 0, FullResolution = 1 };
enum class Backdrop : int { Mica = 0, Dark = 1, Black = 2 };

struct Settings {
    // Navigation
    bool wrapAround = true;
    bool closeWhenEmpty = false;

    // Cache
    int ahead = 4;
    int behind = 5;
    bool autoBudget = true;
    uint32_t ramBudgetMB = 0;   // used when !autoBudget
    uint32_t vramBudgetMB = 0;  // used when !autoBudget (ignored on unified memory)
    int decodeMode = int(DecodeMode::FitScreen);
    bool releaseWhenMinimized = true;
    bool skipCloudFiles = true;

    // Display
    int backdrop = int(Backdrop::Mica);
    bool upscaleSmall = false;
    bool pixelatedZoom = true;

    // Overlay
    bool showOverlay = true;
    int overlayCorner = 1;  // 0 TL, 1 TR, 2 BL, 3 BR
    bool ovName = true, ovType = true, ovIndex = true, ovSize = true, ovDims = true, ovZoom = true;

    // Sidebar
    bool showSidebar = false;
    int sidebarWidth = 280;  // logical pixels (scaled by DPI)
    int thumbHeight = 72;

    bool showStats = false;
    bool showInfo = false;

    // Sorting
    bool pairRawJpeg = true;       // RAW/XMP with the same name travel with the shown image
    bool renameOnConflict = true;  // moving onto an existing name: "name (2).jpg" instead of refusing
    // Keys 1..9: destination folder (UTF-8). Empty = subfolder "1".."9" of the
    // current folder; a relative path is relative to the current folder.
    std::array<std::string, 9> destinations{};

    // Input & tools
    bool wheelNavigates = false;   // wheel = previous/next (Ctrl+wheel zooms) instead of zoom
    int slideshowSeconds = 4;
    std::string editorPath;        // "Open in editor"; empty = Windows "Open with" chooser
    bool autoAdvance = true;       // after marking an image, show the next one
    std::map<std::string, std::string> keyBindings;  // action id -> chords, only where changed

    // Camera RAW is off by default: RAW+JPEG pairs would otherwise show every photo twice.
    std::vector<std::string> disabledFormats = {"Camera RAW"};

    // Window placement (physical pixels); w == 0 means "not saved yet".
    int winX = 0, winY = 0, winW = 0, winH = 0;
    bool winMaximized = false;
};

constexpr int kMaxAhead = 16;
constexpr int kMaxBehind = 16;
constexpr uint32_t kMinBudgetMB = 64;

// Clamps every field to its legal range; budgets are limited to the hardware caps.
void Validate(Settings& s, const MemoryCaps& caps);

// Budgets in bytes after applying auto mode and caps.
uint64_t EffectiveRamSetting(const Settings& s, const MemoryCaps& caps);
uint64_t EffectiveVramSetting(const Settings& s, const MemoryCaps& caps);

std::string Serialize(const Settings& s);
// Unknown keys and malformed lines are ignored; missing keys keep their current value.
void Deserialize(const std::string& text, Settings& s);

}  // namespace fiv::core
