// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/cleanup.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

#include "CleanupSpecks.h"

#include "CleanupCommon.h"

#include <algorithm>
#include <unordered_set>

namespace jasperengine::editor::pixelart::cleanup {

    using jasperengine::pixelart::Lab;
    using jasperengine::pixelart::PaletteMatcher;

    // file-local helpers, in a namespace of their own so unity builds keep them apart
    namespace cleanup_ts {

        // Two colours this far apart (OKLab) or more are not alike at all; closer ones are alike in
        // proportion (cleanup.ts)
        constexpr double ALIKE_RANGE = 0.3;

    }

    Cleaned cleanUp(const PixelArt& art, const CellSamples& samples, const CleanupOptions& options) {
        using namespace cleanup_ts;
        const int width = art.width;
        const int height = art.height;
        if (options.size < 1 || art.palette.size() < 2) return {art, {}};
        const std::unordered_set<int> protect(options.protect.begin(), options.protect.end());
        const std::unordered_set<int> keep(options.keep.begin(), options.keep.end());
        std::vector<Lab> labs;
        for (const Rgba8& color : art.palette) labs.push_back(oklabOf(color));
        PaletteMatcher matcher(art.palette, options.metric);
        Cleaned out{art, {}};
        std::vector<uint8_t> seen(art.indices.size(), 0);

        auto neighbours = [&](int i, auto&& visit) {
            const int x = i % width;
            const int y = (i - x) / width;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if ((dx != 0 || dy != 0) && nx >= 0 && ny >= 0 && nx < width && ny < height) visit(ny * width + nx);
                }
            }
        };

        std::vector<int> group;
        std::vector<std::pair<int32_t, int>> touching;   // value -> count, in the order first met
        for (int start = 0; start < int(art.indices.size()); start++) {
            const int32_t own = art.indices[size_t(start)];
            if (seen[size_t(start)] == 1 || own == TRANSPARENT) continue;
            group.assign(1, start);
            seen[size_t(start)] = 1;
            for (size_t k = 0; k < group.size(); k++) {
                neighbours(group[k], [&](int n) {
                    if (seen[size_t(n)] == 1 || art.indices[size_t(n)] != own) return;
                    seen[size_t(n)] = 1;
                    group.push_back(n);
                });
            }
            if (double(group.size()) > options.size || protect.count(own) ||
                std::any_of(group.begin(), group.end(), [&](int i) { return keep.count(i) > 0; })) {
                continue;
            }

            // The colour the speck touches most
            touching.clear();
            for (int i : group) {
                neighbours(i, [&](int n) {
                    const int32_t value = art.indices[size_t(n)];
                    if (value == own || value == TRANSPARENT) return;
                    auto it = std::find_if(touching.begin(), touching.end(), [&](const auto& entry) { return entry.first == value; });
                    if (it == touching.end()) touching.push_back({value, 1});
                    else it->second++;
                });
            }
            int32_t into = TRANSPARENT;
            int most = 0;
            for (const auto& [value, count] : touching) {
                if (count > most) {
                    into = value;
                    most = count;
                }
            }
            if (into == TRANSPARENT) continue;

            // Doubt: how much the speck's own cells voted for that colour, against their own
            double votes = 0;
            for (int i : group) {
                int forOwn = 0, forInto = 0;
                if (size_t(i) < samples.cells.size()) {
                    for (uint32_t color : samples.cells[size_t(i)].colors) {
                        const int nearest = matcher.nearest(rgbOf(color));
                        if (nearest == own) forOwn++;
                        else if (nearest == into) forInto++;
                    }
                }
                votes += std::min(1.0, double(forInto) / std::max(1, forOwn));
            }
            const double alike = std::max(0.0, 1 - labDistance(labs[size_t(own)], labs[size_t(into)]) / ALIKE_RANGE);
            const double doubt = std::max(votes / double(group.size()), alike);
            if (doubt < 1 - options.strength) continue;
            for (int i : group) out.art.indices[size_t(i)] = into;
            out.specks.push_back(group);
        }
        return out;
    }

}
