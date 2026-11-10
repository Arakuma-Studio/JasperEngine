// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/background.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

#include "CleanupBackground.h"

#include "CleanupCommon.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::editor::pixelart::cleanup {

    using jasperengine::pixelart::Lab;

    // file-local helpers, in a namespace of their own so unity builds keep them apart
    namespace background_ts {

        // Border colours closer than this (OKLab) count as one colour (background.ts)
        constexpr double SAME_COLOR = 0.04;
        // A border colour is background too when at least this share as common as the top one (background.ts)
        constexpr double COMMON_ENOUGH = 0.4;
        // A shadow's colour is a background colour darkened: about its hue, and this much darker (background.ts)
        constexpr double SHADOW_HUE = 0.035;
        constexpr double SHADOW_DARKER_MIN = 0.03;
        constexpr double SHADOW_DARKER_MAX = 0.4;

        // Masks a drop shadow: an area of the background's colour darkened, lying against the
        // background in the bottom half of the subject, at least as wide as it is tall
        void removeShadow(const PixelArt& art, std::vector<uint8_t>& mask, const std::vector<Lab>& targets) {
            const int width = art.width;
            const int height = art.height;
            std::vector<uint8_t> shadowy;
            for (const Rgba8& color : art.palette) {
                const Lab lab = oklabOf(color);
                bool any = false;
                for (const Lab& target : targets) {
                    const double darker = double(target.l) - double(lab.l);
                    if (std::hypot(double(lab.a) - double(target.a), double(lab.b) - double(target.b)) <= SHADOW_HUE &&
                        darker >= SHADOW_DARKER_MIN && darker <= SHADOW_DARKER_MAX) {
                        any = true;
                    }
                }
                shadowy.push_back(any ? 1 : 0);
            }
            auto isShadowy = [&](size_t i) { return art.indices[i] != TRANSPARENT && shadowy[size_t(art.indices[i])] == 1; };
            int top = height;
            int bottom = -1;
            for (size_t i = 0; i < mask.size(); i++) {
                if (mask[i] == 1 || art.indices[i] == TRANSPARENT) continue;
                const int y = int(i / size_t(width));
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
            if (bottom < top) return;
            const double lowest = top + (bottom - top + 1) / 2.0;
            std::vector<uint8_t> seen(mask.size(), 0);
            std::vector<int> group;
            for (size_t start = 0; start < mask.size(); start++) {
                if (seen[start] == 1 || mask[start] == 1 || !isShadowy(start)) continue;
                group.assign(1, int(start));
                seen[start] = 1;
                bool touches = false;
                for (size_t k = 0; k < group.size(); k++) {
                    const int i = group[k];
                    const int x = i % width;
                    const int around[4] = {x > 0 ? i - 1 : -1, x < width - 1 ? i + 1 : -1, i - width, i + width};
                    for (int n : around) {
                        if (n < 0 || n >= int(mask.size())) continue;
                        if (mask[size_t(n)] == 1) {
                            touches = true;
                        } else if (seen[size_t(n)] != 1 && isShadowy(size_t(n))) {
                            seen[size_t(n)] = 1;
                            group.push_back(n);
                        }
                    }
                }
                int minX = width, maxX = -1, minY = height, maxY = -1;
                for (int i : group) {
                    minX = std::min(minX, i % width);
                    maxX = std::max(maxX, i % width);
                    minY = std::min(minY, i / width);
                    maxY = std::max(maxY, i / width);
                }
                if (touches && group.size() >= 3 && minY >= lowest && maxX - minX + 1 >= maxY - minY + 1) {
                    for (int i : group) mask[size_t(i)] = 1;
                }
            }
        }

        // Masks every 8-connected group of at most SPECK_SIZE unmasked pixels
        void removeSpecks(std::vector<uint8_t>& mask, int width, int height) {
            std::vector<uint8_t> seen(mask.size(), 0);
            std::vector<int> group;
            for (size_t start = 0; start < mask.size(); start++) {
                if (mask[start] == 1 || seen[start] == 1) continue;
                group.assign(1, int(start));
                seen[start] = 1;
                for (size_t k = 0; k < group.size(); k++) {
                    const int i = group[k];
                    const int x = i % width;
                    const int y = (i - x) / width;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            const int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                            const size_t n = size_t(ny) * size_t(width) + size_t(nx);
                            if (mask[n] == 1 || seen[n] == 1) continue;
                            seen[n] = 1;
                            group.push_back(int(n));
                        }
                    }
                }
                if (group.size() <= size_t(SPECK_SIZE)) {
                    for (int i : group) mask[size_t(i)] = 1;
                }
            }
        }

    }

    std::vector<int> backgroundColors(const PixelArt& art) {
        using namespace background_ts;
        const int width = art.width;
        const int height = art.height;
        std::vector<std::pair<int, int>> counts;   // index -> border pixels, in the order first met
        auto count = [&](int x, int y) {
            const int32_t index = art.indices[size_t(y) * size_t(width) + size_t(x)];
            if (index == TRANSPARENT) return;
            for (auto& entry : counts) {
                if (entry.first == index) {
                    entry.second++;
                    return;
                }
            }
            counts.push_back({index, 1});
        };
        for (int x = 0; x < width; x++) {
            count(x, 0);
            if (height > 1) count(x, height - 1);
        }
        for (int y = 1; y < height - 1; y++) {
            count(0, y);
            if (width > 1) count(width - 1, y);
        }
        std::stable_sort(counts.begin(), counts.end(), [](const auto& x, const auto& y) { return y.second < x.second; });
        struct Group {
            int index;
            Lab lab;
            int n;
        };
        std::vector<Group> groups;
        for (const auto& [index, n] : counts) {
            const Lab lab = oklabOf(art.palette[size_t(index)]);
            auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& other) { return labDistance(other.lab, lab) <= SAME_COLOR; });
            if (it == groups.end()) groups.push_back({index, lab, n});
            else it->n += n;
        }
        std::stable_sort(groups.begin(), groups.end(), [](const Group& x, const Group& y) { return y.n < x.n; });
        if (groups.empty()) return {};
        const int topCount = groups.front().n;
        std::vector<int> out;
        for (size_t g = 0; g < groups.size() && g < 3; g++) {
            if (groups[g].n >= COMMON_ENOUGH * topCount) out.push_back(groups[g].index);
        }
        return out;
    }

    std::vector<uint8_t> findBackground(const PixelArt& art, const std::vector<int>& colors, const BackgroundOptions& options) {
        using namespace background_ts;
        const int width = art.width;
        const int height = art.height;
        std::vector<Lab> targets;
        for (int index : colors) targets.push_back(oklabOf(art.palette[size_t(index)]));
        std::vector<uint8_t> like;
        for (const Rgba8& color : art.palette) {
            const Lab lab = oklabOf(color);
            like.push_back(std::any_of(targets.begin(), targets.end(), [&](const Lab& target) { return labDistance(lab, target) <= options.tolerance; }) ? 1 : 0);
        }
        auto isLike = [&](size_t i) {
            const int32_t index = art.indices[i];
            return index == TRANSPARENT || like[size_t(index)] == 1;
        };
        std::vector<uint8_t> mask(size_t(width) * size_t(height), 0);
        const bool anyTransparent = std::find(art.indices.begin(), art.indices.end(), TRANSPARENT) != art.indices.end();
        if (!targets.empty() || anyTransparent) {
            if (options.enclosed) {
                for (size_t i = 0; i < mask.size(); i++) {
                    if (isLike(i)) mask[i] = 1;
                }
            } else {
                std::vector<size_t> stack;
                auto seed = [&](size_t i) {
                    if (mask[i] == 1 || !isLike(i)) return;
                    mask[i] = 1;
                    stack.push_back(i);
                };
                for (int x = 0; x < width; x++) {
                    seed(size_t(x));
                    seed(size_t(height - 1) * size_t(width) + size_t(x));
                }
                for (int y = 0; y < height; y++) {
                    seed(size_t(y) * size_t(width));
                    seed(size_t(y) * size_t(width) + size_t(width) - 1);
                }
                while (!stack.empty()) {
                    const size_t i = stack.back();
                    stack.pop_back();
                    const int x = int(i % size_t(width));
                    if (x > 0) seed(i - 1);
                    if (x < width - 1) seed(i + 1);
                    if (i >= size_t(width)) seed(i - size_t(width));
                    if (i + size_t(width) < mask.size()) seed(i + size_t(width));
                }
            }
        }
        const bool anyMasked = std::find(mask.begin(), mask.end(), uint8_t(1)) != mask.end();
        if (options.shadow && !targets.empty() && anyMasked) removeShadow(art, mask, targets);
        if (options.specks && std::find(mask.begin(), mask.end(), uint8_t(1)) != mask.end()) removeSpecks(mask, width, height);
        return mask;
    }

    PixelArt clearMasked(const PixelArt& art, const std::vector<uint8_t>& mask) {
        using namespace background_ts;
        PixelArt out = art;
        for (size_t i = 0; i < out.indices.size(); i++) {
            if (mask[i] == 1) out.indices[i] = TRANSPARENT;
        }
        return out;
    }

}
