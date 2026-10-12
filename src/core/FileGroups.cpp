#include "FileGroups.h"

#include <algorithm>
#include <unordered_map>

namespace fiv::core {

std::vector<FileGroup> GroupFiles(const std::vector<GroupInput>& in, const GroupOptions& opt) {
    std::unordered_map<std::wstring, std::vector<size_t>> byStem;
    for (size_t i = 0; i < in.size(); ++i)
        if (in[i].kind != FileKind::Ignore) byStem[in[i].stem].push_back(i);

    std::vector<FileGroup> out;
    for (auto& [stem, idx] : byStem) {
        std::vector<size_t> prim, raw, side;
        for (size_t i : idx) {
            switch (in[i].kind) {
                case FileKind::Primary: prim.push_back(i); break;
                case FileKind::Raw: raw.push_back(i); break;
                case FileKind::Sidecar: side.push_back(i); break;
                default: break;
            }
        }
        if (!opt.pairRaw) {
            for (size_t p : prim) out.push_back({p, {}});
            if (opt.rawVisible)
                for (size_t r : raw) out.push_back({r, {}});
            continue;
        }
        if (!prim.empty()) {
            FileGroup g{prim[0], {}};
            g.companions.insert(g.companions.end(), raw.begin(), raw.end());
            g.companions.insert(g.companions.end(), side.begin(), side.end());
            out.push_back(std::move(g));
            for (size_t k = 1; k < prim.size(); ++k) out.push_back({prim[k], {}});
        } else if (!raw.empty() && opt.rawVisible) {
            FileGroup g{raw[0], {}};
            g.companions.insert(g.companions.end(), raw.begin() + 1, raw.end());
            g.companions.insert(g.companions.end(), side.begin(), side.end());
            out.push_back(std::move(g));
        }
    }
    std::sort(out.begin(), out.end(), [](const FileGroup& a, const FileGroup& b) { return a.primary < b.primary; });
    for (auto& g : out) std::sort(g.companions.begin(), g.companions.end());
    return out;
}

std::vector<std::wstring> SuffixedNames(const std::vector<std::wstring>& leaves, int n) {
    if (n <= 1) return leaves;
    std::vector<std::wstring> out;
    out.reserve(leaves.size());
    const std::wstring tag = L" (" + std::to_wstring(n) + L")";
    for (const auto& leaf : leaves) {
        const size_t dot = leaf.find_last_of(L'.');
        out.push_back(dot == std::wstring::npos || dot == 0 ? leaf + tag : leaf.substr(0, dot) + tag + leaf.substr(dot));
    }
    return out;
}

}  // namespace fiv::core
