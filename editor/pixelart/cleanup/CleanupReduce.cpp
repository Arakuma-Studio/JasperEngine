// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/reduce.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.
// The stroke handling follows ToPixel's forced resize (https://github.com/daisuqe/ToPixel).

#include "CleanupReduce.h"

#include "CleanupCommon.h"
#include "CleanupOutline.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace jasperengine::editor::pixelart::cleanup {

    // file-local helpers, in a namespace of their own so unity builds keep them apart
    namespace reduce_ts {

        // A new pixel at least this much covered by outline is outline, whatever thinning says (reduce.ts)
        constexpr double SOLID_OUTLINE = 0.6;
        // A block with no pixel of the kind asked for (distinct from transparent, a colour here; reduce.ts)
        constexpr int32_t NONE = -2;
        // The 8 neighbours around a pixel, from the one above, clockwise (reduce.ts)
        constexpr int REDUCE_RING[8][2] = {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};

        // Weights by colour in the order first met, as a JS Map iterates
        struct Weights {
            std::vector<std::pair<int32_t, double>> entries;
            void add(int32_t index, double weight) {
                for (auto& entry : entries) {
                    if (entry.first == index) {
                        entry.second += weight;
                        return;
                    }
                }
                entries.push_back({index, weight});
            }
            int32_t top() const {
                int32_t best = NONE;
                double most = -1;
                for (const auto& [index, weight] : entries) {
                    if (weight > most) {
                        best = index;
                        most = weight;
                    }
                }
                return best;
            }
        };

        // Groups among the ring members linked by `linked`, started from `starts` in order
        template <typename Linked>
        int ringGroups(const bool member[8], Linked&& linked, std::initializer_list<int> starts) {
            bool seen[8] = {};
            int count = 0;
            for (int start : starts) {
                if (!member[start] || seen[start]) continue;
                count++;
                int stack[8];
                int top = 0;
                stack[top++] = start;
                seen[start] = true;
                while (top > 0) {
                    const int u = stack[--top];
                    for (int v = 0; v < 8; v++) {
                        if (member[v] && !seen[v] && linked(u, v)) {
                            seen[v] = true;
                            stack[top++] = v;
                        }
                    }
                }
            }
            return count;
        }

    }

    PixelArt reduce(const PixelArt& art, double width, double height, ReduceMode mode) {
        using namespace reduce_ts;
        const int outWidth = std::max(1, std::min(art.width, int(jsRound(width))));
        const int outHeight = std::max(1, std::min(art.height, int(jsRound(height))));
        if (outWidth == art.width && outHeight == art.height) return art;
        const double fx = double(art.width) / outWidth;
        const double fy = double(art.height) / outHeight;
        const std::vector<uint8_t> outline = mode == ReduceMode::Outlines ? outlineColorsOfArt(art) : std::vector<uint8_t>(art.palette.size(), 0);
        auto isOutline = [&](int32_t index) { return index != TRANSPARENT && outline[size_t(index)] == 1; };
        const size_t size = size_t(outWidth) * size_t(outHeight);
        // Per new pixel: the most common outline colour and the most common other colour (or
        // transparent), and the share of the block the outline covers
        std::vector<int32_t> lineColor(size, NONE), fillColor(size, NONE);
        std::vector<double> lineShare(size, 0.0);
        Weights lines, fills;
        for (int oy = 0; oy < outHeight; oy++) {
            const double y0 = oy * fy;
            const double y1 = y0 + fy;
            for (int ox = 0; ox < outWidth; ox++) {
                const double x0 = ox * fx;
                const double x1 = x0 + fx;
                lines.entries.clear();
                fills.entries.clear();
                double lineWeight = 0, total = 0;
                for (int y = int(std::floor(y0)); y < std::min(art.height, int(std::ceil(y1))); y++) {
                    const double h = std::min(y1, double(y + 1)) - std::max(y0, double(y));
                    for (int x = int(std::floor(x0)); x < std::min(art.width, int(std::ceil(x1))); x++) {
                        const double w = (std::min(x1, double(x + 1)) - std::max(x0, double(x))) * h;
                        const int32_t index = art.indices[size_t(y) * size_t(art.width) + size_t(x)];
                        (isOutline(index) ? lines : fills).add(index, w);
                        total += w;
                        if (isOutline(index)) lineWeight += w;
                    }
                }
                const size_t cell = size_t(oy) * size_t(outWidth) + size_t(ox);
                lineColor[cell] = lines.top();
                fillColor[cell] = fills.top();
                lineShare[cell] = total > 0 ? lineWeight / total : 0;
            }
        }
        if (mode == ReduceMode::Common) return PixelArt{outWidth, outHeight, art.palette, fillColor};

        // Strokes: the new pixel holding each outline pixel's centre, and a straight path between
        // those of two touching outline pixels, so no link of a stroke can be lost. A lone outline
        // pixel (no outline neighbour) is no stroke.
        std::vector<uint8_t> marked(size, 0);
        auto cellX = [&](int x) { return std::min(outWidth - 1, int(std::floor((x + 0.5) / fx))); };
        auto cellY = [&](int y) { return std::min(outHeight - 1, int(std::floor((y + 0.5) / fy))); };
        auto darkAt = [&](int x, int y) { return x >= 0 && y >= 0 && x < art.width && y < art.height && isOutline(art.indices[size_t(y) * size_t(art.width) + size_t(x)]); };
        constexpr int LINKS[4][2] = {{1, 0}, {0, 1}, {1, 1}, {-1, 1}};
        for (int y = 0; y < art.height; y++) {
            for (int x = 0; x < art.width; x++) {
                if (!darkAt(x, y)) continue;
                bool linked = false;
                for (const auto& d : REDUCE_RING) linked = linked || darkAt(x + d[0], y + d[1]);
                if (!linked) continue;
                const int ax = cellX(x);
                const int ay = cellY(y);
                marked[size_t(ay) * size_t(outWidth) + size_t(ax)] = 1;
                for (const auto& d : LINKS) {
                    if (!darkAt(x + d[0], y + d[1])) continue;
                    const int bx = cellX(x + d[0]);
                    const int by = cellY(y + d[1]);
                    const int steps = std::max(std::abs(bx - ax), std::abs(by - ay));
                    for (int s = 1; s < steps; s++) {
                        const int my = int(jsRound(ay + double((by - ay) * s) / steps));
                        const int mx = int(jsRound(ax + double((bx - ax) * s) / steps));
                        marked[size_t(my) * size_t(outWidth) + size_t(mx)] = 1;
                    }
                }
            }
        }
        for (size_t cell = 0; cell < size; cell++) {
            if (lineShare[cell] >= SOLID_OUTLINE) marked[cell] = 1;
        }

        // Thinning, least covered first: a mark goes only when the marks around it stay one
        // connected group and no hole opens; stroke ends and solid outline stay
        auto on = [&](int x, int y) { return x >= 0 && y >= 0 && x < outWidth && y < outHeight && marked[size_t(y) * size_t(outWidth) + size_t(x)] == 1; };
        auto removable = [&](int x, int y) {
            bool ring[8], gaps[8];
            int lit = 0;
            for (int k = 0; k < 8; k++) {
                ring[k] = on(x + REDUCE_RING[k][0], y + REDUCE_RING[k][1]);
                gaps[k] = !ring[k];
                lit += ring[k] ? 1 : 0;
            }
            if (lit < 2) return false;
            auto dx = [](int u, int v) { return std::abs(REDUCE_RING[u][0] - REDUCE_RING[v][0]); };
            auto dy = [](int u, int v) { return std::abs(REDUCE_RING[u][1] - REDUCE_RING[v][1]); };
            if (ringGroups(ring, [&](int u, int v) { return std::max(dx(u, v), dy(u, v)) == 1; }, {0, 1, 2, 3, 4, 5, 6, 7}) != 1) return false;
            // The background around it, joined only side by side, touches it in one place
            return ringGroups(gaps, [&](int u, int v) { return dx(u, v) + dy(u, v) == 1; }, {0, 2, 4, 6}) == 1;
        };
        std::vector<int> candidates;
        for (size_t cell = 0; cell < size; cell++) {
            if (marked[cell] == 1 && lineShare[cell] < SOLID_OUTLINE) candidates.push_back(int(cell));
        }
        std::stable_sort(candidates.begin(), candidates.end(), [&](int a, int b) { return lineShare[size_t(a)] < lineShare[size_t(b)]; });
        for (int pass = 0; pass < 4; pass++) {
            bool changed = false;
            for (int cell : candidates) {
                if (marked[size_t(cell)] == 1 && removable(cell % outWidth, cell / outWidth)) {
                    marked[size_t(cell)] = 0;
                    changed = true;
                }
            }
            if (!changed) break;
        }

        // Colours: a stroke pixel its block's outline colour, any other its block's other colour. A
        // block with none of the colour it needs borrows a neighbour's.
        std::vector<int32_t> indices(size);
        constexpr int NEIGHBOURS[12][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};
        for (size_t cell = 0; cell < size; cell++) {
            const bool stroke = marked[cell] == 1;
            const std::vector<int32_t>& wanted = stroke ? lineColor : fillColor;
            long from = -1;
            if (wanted[cell] != NONE) {
                from = long(cell);
            } else {
                const int x = int(cell % size_t(outWidth));
                const int y = int(cell / size_t(outWidth));
                for (const auto& d : NEIGHBOURS) {
                    const int nx = x + d[0], ny = y + d[1];
                    if (nx < 0 || ny < 0 || nx >= outWidth || ny >= outHeight) continue;
                    const size_t other = size_t(ny) * size_t(outWidth) + size_t(nx);
                    if ((marked[other] == 1) == stroke && wanted[other] != NONE) {
                        from = long(other);
                        break;
                    }
                }
            }
            indices[cell] = from >= 0 ? wanted[size_t(from)] : stroke ? fillColor[cell] : lineColor[cell];
        }
        smoothStairs(indices, outWidth, outHeight, isOutline);
        return PixelArt{outWidth, outHeight, art.palette, std::move(indices)};
    }

    void smoothStairs(std::vector<int32_t>& indices, int width, int height, const std::function<bool(int32_t)>& isOutline) {
        using namespace reduce_ts;
        auto dark = [&](int x, int y) { return x >= 0 && y >= 0 && x < width && y < height && isOutline(indices[size_t(y) * size_t(width) + size_t(x)]); };
        auto arm = [&](int x, int y, int dx, int dy) {
            int length = 0;
            while (dark(x + dx * (length + 1), y + dy * (length + 1))) length++;
            return length;
        };
        struct Elbow {
            int vy, hx, score;
        };
        auto elbow = [&](int x, int y) -> std::optional<Elbow> {
            if (!dark(x, y)) return std::nullopt;
            const bool up = dark(x, y - 1), down = dark(x, y + 1), left = dark(x - 1, y), right = dark(x + 1, y);
            if (int(up) + int(down) + int(left) + int(right) != 2 || !(up || down) || !(left || right)) return std::nullopt;
            // Any other outline neighbour (a diagonal) might hang on this pixel: it stays
            int around = 0;
            for (const auto& d : REDUCE_RING) around += dark(x + d[0], y + d[1]) ? 1 : 0;
            if (around != 2) return std::nullopt;
            const int vy = up ? -1 : 1;
            const int hx = left ? -1 : 1;
            const int vertical = arm(x, y, 0, vy);
            const int horizontal = arm(x, y, hx, 0);
            if (vertical > 1 && horizontal > 1) return std::nullopt;
            return Elbow{vy, hx, vertical + horizontal};
        };
        for (int pass = 0; pass < 4; pass++) {
            bool changed = false;
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    const std::optional<Elbow> here = elbow(x, y);
                    if (!here) continue;
                    const std::optional<Elbow> along = elbow(x, y + here->vy);
                    const std::optional<Elbow> beside = elbow(x + here->hx, y);
                    if ((along && along->score > here->score) || (beside && beside->score > here->score)) continue;
                    const int sx = x - here->hx;
                    const int sy = y - here->vy;
                    const int from = sx >= 0 && sx < width ? y * width + sx : sy >= 0 && sy < height ? sy * width + x : -1;
                    indices[size_t(y) * size_t(width) + size_t(x)] = from >= 0 ? indices[size_t(from)] : TRANSPARENT;
                    changed = true;
                }
            }
            if (!changed) break;
        }
    }

}
