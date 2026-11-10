// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/edits.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

#include "CleanupEdits.h"

#include <algorithm>
#include <optional>
#include <set>
#include <unordered_map>

namespace jasperengine::editor::pixelart::cleanup {

    using jasperengine::pixelart::PaletteMatcher;
    using jasperengine::pixelart::packRgb;

    // file-local helpers, in a namespace of their own so unity builds keep them apart
    namespace edits_ts {

        // The 4-connected pixels holding the same palette index as (x, y)
        std::vector<int> editRegion(const PixelArt& art, int x, int y) {
            const int width = art.width;
            const int start = y * width + x;
            const int32_t value = art.indices[size_t(start)];
            std::vector<uint8_t> seen(art.indices.size(), 0);
            std::vector<int> stack{start};
            std::vector<int> out;
            seen[size_t(start)] = 1;
            while (!stack.empty()) {
                const int i = stack.back();
                stack.pop_back();
                out.push_back(i);
                const int px = i % width;
                const int around[4] = {px > 0 ? i - 1 : -1, px < width - 1 ? i + 1 : -1, i - width, i + width};
                for (int n : around) {
                    if (n < 0 || n >= int(art.indices.size()) || seen[size_t(n)] == 1 || art.indices[size_t(n)] != value) continue;
                    seen[size_t(n)] = 1;
                    stack.push_back(n);
                }
            }
            return out;
        }

    }

    Edited applyEdits(const PixelArt& art, const std::vector<uint8_t>& mask, const std::vector<Edit>& edits,
                      jasperengine::pixelart::ColorMetric metric) {
        using namespace edits_ts;
        Edited out{art, mask, {}};
        std::set<int> painted;
        // A JS Map built from the palette: a repeated colour keeps its last index
        std::unordered_map<uint32_t, int> exact;
        for (size_t i = 0; i < art.palette.size(); i++) exact[packRgb(art.palette[i])] = int(i);
        std::optional<PaletteMatcher> matcher;
        if (!art.palette.empty()) matcher.emplace(art.palette, metric);
        for (const Edit& edit : edits) {
            if (edit.x < 0 || edit.y < 0 || edit.x >= art.width || edit.y >= art.height) continue;
            const std::vector<int> cells = edit.area ? editRegion(out.art, edit.x, edit.y) : std::vector<int>{edit.y * art.width + edit.x};
            if (edit.tool == EditTool::Paint) {
                if (art.palette.empty()) continue;
                const auto found = exact.find(packRgb(edit.color));
                const int index = found != exact.end() ? found->second : matcher->nearest(Rgba8{edit.color.r, edit.color.g, edit.color.b, 255});
                for (int i : cells) {
                    out.art.indices[size_t(i)] = index;
                    out.mask[size_t(i)] = 0;
                    painted.insert(i);
                }
            } else {
                const uint8_t value = edit.tool == EditTool::Erase ? 1 : 0;
                for (int i : cells) {
                    out.mask[size_t(i)] = value;
                    if (value == 1) painted.erase(i);
                }
            }
        }
        out.painted.assign(painted.begin(), painted.end());
        return out;
    }

}
