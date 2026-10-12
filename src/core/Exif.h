#pragma once
// Camera/exposure metadata for the info panel. Parses EXIF from the first
// bytes of a JPEG (APP1) or a TIFF-based file (TIFF, DNG, NEF, CR2, ARW, ORF,
// RW2, PEF, SRW). Bounds-checked: safe on truncated or hostile data.
#include <cstddef>
#include <cstdint>
#include <string>

namespace fiv::core {

struct PhotoInfo {
    std::string make, model, lens;
    std::string dateTaken;          // EXIF form "YYYY:MM:DD HH:MM:SS"
    uint32_t exposureNum = 0, exposureDen = 0;
    double fNumber = 0;
    uint32_t iso = 0;
    double focalLength = 0;
    uint32_t focal35 = 0;
    double exposureBias = 0;
    bool hasBias = false;

    bool Empty() const {
        return make.empty() && model.empty() && lens.empty() && dateTaken.empty() && exposureDen == 0 &&
               fNumber == 0 && iso == 0 && focalLength == 0;
    }
};

// `head` is the start of the file. Returns true if any EXIF field was found.
bool ParseExifHead(const uint8_t* head, size_t size, PhotoInfo& out);

// Display helpers (UTF-8).
std::string CameraLine(const PhotoInfo& p);     // "Canon EOS R5  ·  RF24-70mm F2.8 L IS USM"
std::string ExposureLine(const PhotoInfo& p);   // "1/250 s  ·  f/2.8  ·  ISO 400  ·  50 mm (75 mm eq.)  ·  +0.3 EV"
std::string FormatExposure(uint32_t num, uint32_t den);
std::string FormatExifDate(const std::string& exif);  // "2024-05-01 12:34:56"

}  // namespace fiv::core
