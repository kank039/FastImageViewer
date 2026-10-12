#pragma once
#include <string>
#include <vector>

#include "../core/FileGroups.h"

namespace fiv {

struct ImageFormat {
    std::string name;
    std::vector<std::wstring> extensions;  // lower case, with dot
    std::string storeUrl;                  // Microsoft Store page for the codec, if one exists
    std::string needs;                     // what to install when missing
    bool honorsExif = true;                // false where the codec already applies rotation (HEIF/AVIF)
    bool builtIn = false;                  // ships with Windows 10/11: never hidden by detection
    bool installed = false;
    bool enabled = false;
};

class FormatRegistry {
public:
    FormatRegistry();
    void Detect();  // queries WIC; call on a thread with COM initialized
    void ApplyDisabled(const std::vector<std::string>& disabledNames);
    std::vector<std::string> DisabledNames() const;
    const ImageFormat* ForExtension(const std::wstring& lowerExt) const;
    bool IsEnabled(const std::wstring& lowerExt) const;
    // Pairing role of a file: RAW and .xmp are classified even when not shown.
    core::FileKind Classify(const std::wstring& lowerExt) const;
    bool RawEnabled() const;
    bool IsRaw(const std::wstring& lowerExt) const;
    std::vector<ImageFormat>& All() { return formats_; }

private:
    std::vector<ImageFormat> formats_;
};

}  // namespace fiv
