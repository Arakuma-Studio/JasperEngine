// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/lost.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

#include "CleanupLost.h"

#include "CleanupCommon.h"
#include "CleanupSample.h"

#include <algorithm>

namespace jasperengine::editor::pixelart::cleanup {

    using jasperengine::pixelart::Lab;

    // file-local helpers, in a namespace of their own so unity builds keep them apart
    namespace lost_ts {

        // Lost cells whose own colours are closer than this (OKLab) are one lost colour (lost.ts)
        constexpr double GROUP_RADIUS = 0.06;

        struct LabSum {
            double l = 0, a = 0, b = 0;
        };

        double lostDistance(const LabSum& x, const Lab& y) {
            return std::hypot(x.l - double(y.l), x.a - double(y.a), x.b - double(y.b));
        }

    }

    std::vector<LostColor> lostColors(const CellSamples& samples, const PixelArt& art, double threshold,
                                      const std::vector<uint8_t>* skip, int minCells, const PixelArt* own) {
        using namespace lost_ts;
        std::vector<Lab> palette;
        for (const Rgba8& color : art.palette) palette.push_back(oklabOf(color));
        std::vector<Lab> ownLabs;
        if (own) {
            ownLabs.reserve(own->palette.size());
            for (const Rgba8& color : own->palette) ownLabs.push_back(oklabOf(color));
        }
        struct Group {
            LabSum sum, center;
            std::vector<int> cells;
            std::vector<std::pair<int32_t, int>> became;   // index -> cells, in the order first met
        };
        std::vector<Group> groups;
        for (size_t i = 0; i < art.indices.size(); i++) {
            const int32_t index = art.indices[i];
            if (index == TRANSPARENT || (skip && i < skip->size() && (*skip)[i] == 1) || i >= samples.cells.size() ||
                samples.cells[i].colors.empty()) {
                continue;
            }
            const int32_t ownIndex = own && i < own->indices.size() ? own->indices[i] : TRANSPARENT;
            const Lab cellLab = ownIndex != TRANSPARENT ? ownLabs[size_t(ownIndex)] : oklabOf(medianColor(samples.cells[i].colors));
            if (labDistance(cellLab, palette[size_t(index)]) <= threshold) continue;
            auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& other) { return lostDistance(other.center, cellLab) <= GROUP_RADIUS; });
            if (it == groups.end()) {
                groups.push_back({{}, {cellLab.l, cellLab.a, cellLab.b}, {}, {}});
                it = groups.end() - 1;
            }
            Group& group = *it;
            group.sum = {group.sum.l + cellLab.l, group.sum.a + cellLab.a, group.sum.b + cellLab.b};
            group.cells.push_back(int(i));
            const double n = double(group.cells.size());
            group.center = {group.sum.l / n, group.sum.a / n, group.sum.b / n};
            auto b = std::find_if(group.became.begin(), group.became.end(), [&](const auto& entry) { return entry.first == index; });
            if (b == group.became.end()) group.became.push_back({index, 1});
            else b->second++;
        }
        std::vector<const Group*> kept;
        for (const Group& group : groups) {
            if (int(group.cells.size()) >= minCells) kept.push_back(&group);
        }
        std::stable_sort(kept.begin(), kept.end(), [](const Group* x, const Group* y) { return y->cells.size() < x->cells.size(); });
        std::vector<LostColor> out;
        for (const Group* group : kept) {
            std::vector<std::pair<int32_t, int>> became = group->became;
            std::stable_sort(became.begin(), became.end(), [](const auto& x, const auto& y) { return y.second < x.second; });
            LostColor lost;
            lost.color = jasperengine::pixelart::fromOklab(Lab{float(group->center.l), float(group->center.a), float(group->center.b)}, 255);
            lost.became = art.palette[size_t(became.front().first)];
            lost.cells = group->cells;
            out.push_back(std::move(lost));
        }
        return out;
    }

}
