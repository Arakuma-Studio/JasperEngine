// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/outline.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.
// After ToPixel (https://github.com/daisuqe/ToPixel), whose outline mode ritmo's follows.

#include "CleanupOutline.h"

#include "CleanupGrid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace jasperengine::editor::pixelart::cleanup {

    // file-local helpers, in a namespace of their own so unity builds keep them apart
    namespace outline_ts {

        double luminance(double r, double g, double b) { return r * 0.299 + g * 0.587 + b * 0.114; }

        // The darkest luminance that still counts as outline (outline.ts): past the darkest 5 % of
        // the opaque pixels, at the emptiest luminance within reach, capped well below middle grey
        int darkThreshold(const std::array<uint32_t, 256>& histogram) {
            double opaque = 0;
            for (uint32_t count : histogram) opaque += count;
            if (opaque == 0) return -1;
            double seen = 0;
            int darkest = 0;
            for (; darkest < 255; darkest++) {
                seen += histogram[size_t(darkest)];
                if (seen >= opaque * 0.05) break;
            }
            auto smooth = [&](int value) {
                double sum = 0;
                for (int k = std::max(0, value - 2); k <= std::min(255, value + 2); k++) sum += histogram[size_t(k)];
                return sum;
            };
            const int top = std::min(darkest + 60, 120);
            int threshold = std::min(darkest + 30, 100);
            double emptiest = std::numeric_limits<double>::infinity();
            for (int value = darkest + 8; value <= top; value++) {
                const double count = smooth(value);
                if (count < emptiest) {
                    emptiest = count;
                    threshold = value;
                }
            }
            // A run of equally empty values: the cut goes in its middle
            int end = threshold;
            while (end + 1 <= top && smooth(end + 1) == emptiest) end++;
            return (threshold + end) / 2;
        }

        // The 8 neighbours around a pixel, from the one above, clockwise
        constexpr int OUTLINE_RING[8][2] = {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};

    }

    OutlineMask outlineMask(const RgbaImage& image) {
        using namespace outline_ts;
        const int width = image.width();
        const int height = image.height();
        const size_t count = size_t(width) * size_t(height);
        const uint8_t* data = image.pixels().data();
        std::array<uint32_t, 256> histogram{};
        for (size_t i = 0; i < count; i++) {
            const uint8_t* p = data + i * 4;
            if (p[3] >= 128) histogram[size_t(std::floor(luminance(p[0], p[1], p[2])))]++;
        }
        OutlineMask mask;
        mask.width = width;
        mask.height = height;
        mask.threshold = darkThreshold(histogram);
        mask.dark.assign(count, 0);
        for (size_t i = 0; i < count; i++) {
            const uint8_t* p = data + i * 4;
            if (p[3] >= 128 && luminance(p[0], p[1], p[2]) <= mask.threshold) mask.dark[i] = 1;
        }
        return mask;
    }

    std::vector<uint8_t> outlineColorsOfArt(const PixelArt& art) {
        using namespace outline_ts;
        std::array<uint32_t, 256> histogram{};
        std::vector<int> level;
        for (const Rgba8& color : art.palette) level.push_back(int(std::floor(luminance(color.r, color.g, color.b))));
        for (int32_t index : art.indices) {
            if (index != TRANSPARENT) histogram[size_t(level[size_t(index)])]++;
        }
        const int threshold = darkThreshold(histogram);
        std::vector<uint8_t> out;
        for (int value : level) out.push_back(value <= threshold ? 1 : 0);
        return out;
    }

    bool hasOutlines(const OutlineMask& mask, double cell) {
        using namespace outline_ts;
        if (!(cell >= 2)) return false;
        const int width = mask.width;
        const int height = mask.height;
        std::vector<int> runs;
        auto scan = [&](int length, int lines, auto&& at) {
            for (int line = 0; line < lines; line += 2) {
                int run = 0;
                for (int t = 0; t <= length; t++) {
                    if (t < length && mask.dark[size_t(at(line, t))] == 1) {
                        run++;
                    } else {
                        if (run >= cell * 0.4 && run < cell * 2.6) runs.push_back(run);
                        run = 0;
                    }
                }
            }
        };
        scan(width, height, [&](int y, int x) { return y * width + x; });
        scan(height, width, [&](int x, int y) { return y * width + x; });
        if (runs.size() < 40) return false;
        std::sort(runs.begin(), runs.end());
        const double median = runs[runs.size() >> 1] / cell;
        return median >= 0.6 && median <= 1.5;
    }

    std::vector<uint8_t> outlineColors(const PixelArt& art, const OutlineMask& mask) {
        using namespace outline_ts;
        std::vector<uint8_t> out;
        for (const Rgba8& color : art.palette) out.push_back(luminance(color.r, color.g, color.b) <= mask.threshold ? 1 : 0);
        return out;
    }

    PixelArt thinOutlines(const PixelArt& art, const Grid& grid, const OutlineMask& mask) {
        using namespace outline_ts;
        const GridSize size = gridSize(grid);
        const int width = size.width;
        const int height = size.height;
        if (width != art.width || height != art.height) return art;
        const std::vector<uint8_t> dark = outlineColors(art, mask);
        PixelArt out = art;
        std::vector<int32_t>& indices = out.indices;
        auto isOutline = [&](int i) { return indices[size_t(i)] != TRANSPARENT && dark[size_t(indices[size_t(i)])] == 1; };
        using Box = std::array<double, 4>;
        std::vector<Box> boxes(size_t(width) * size_t(height));
        for (int i = 0; i < width * height; i++) boxes[size_t(i)] = cellBox(grid, i % width, i / width);
        auto darkIn = [&](const Box& box) {
            int count = 0;
            for (int y = std::max(0, int(std::floor(box[1]))); y < std::min(mask.height, int(std::ceil(box[3]))); y++) {
                for (int x = std::max(0, int(std::floor(box[0]))); x < std::min(mask.width, int(std::ceil(box[2]))); x++) {
                    count += mask.dark[size_t(y) * size_t(mask.width) + size_t(x)];
                }
            }
            return count;
        };
        // The mean dark width across a box, along rows (horizontal) or columns, over the lines that have any
        auto span = [&](const Box& box, bool horizontal) {
            const double left = box[0], top = box[1], right = box[2], bottom = box[3];
            const int a0 = std::max(0, int(std::floor(horizontal ? top : left)));
            const int a1 = std::min(horizontal ? mask.height : mask.width, int(std::ceil(horizontal ? bottom : right)));
            const int b0 = std::max(0, int(std::floor(horizontal ? left : top)));
            const int b1 = std::min(horizontal ? mask.width : mask.height, int(std::ceil(horizontal ? right : bottom)));
            double total = 0;
            int lines = 0;
            for (int a = a0; a < a1; a++) {
                int sum = 0;
                for (int b = b0; b < b1; b++) sum += mask.dark[horizontal ? size_t(a) * mask.width + size_t(b) : size_t(b) * mask.width + size_t(a)];
                if (sum > 0) {
                    total += sum;
                    lines++;
                }
            }
            return lines == 0 ? 0.0 : total / lines;
        };
        // Whether the outline pixel can go without splitting the outline around it: its outline
        // neighbours (diagonals count) stay one connected group, and there are at least two
        auto removable = [&](int i) {
            const int x = i % width;
            const int y = (i - x) / width;
            bool on[8];
            int lit = 0;
            for (int k = 0; k < 8; k++) {
                const int nx = x + OUTLINE_RING[k][0], ny = y + OUTLINE_RING[k][1];
                on[k] = nx >= 0 && ny >= 0 && nx < width && ny < height && isOutline(ny * width + nx);
                lit += on[k] ? 1 : 0;
            }
            bool seen[8] = {};
            int groups = 0;
            for (int k = 0; k < 8; k++) {
                if (!on[k] || seen[k]) continue;
                groups++;
                int stack[8];
                int top = 0;
                stack[top++] = k;
                seen[k] = true;
                while (top > 0) {
                    const int u = stack[--top];
                    for (int v = 0; v < 8; v++) {
                        if (!on[v] || seen[v]) continue;
                        if (std::max(std::abs(OUTLINE_RING[u][0] - OUTLINE_RING[v][0]), std::abs(OUTLINE_RING[u][1] - OUTLINE_RING[v][1])) == 1) {
                            seen[v] = true;
                            stack[top++] = v;
                        }
                    }
                }
            }
            // An end of a stroke (touching nothing but its pair) stays: the stroke would only get shorter
            return groups == 1 && lit >= 2;
        };
        // Across the stroke, every outline neighbour of the pixel going is one of the pixel staying
        // too: two strokes meeting (a T, a corner) are not a split stroke
        auto sameSides = [&](int lose, int keep, bool horizontal) {
            const int step = horizontal ? width : 1;
            for (int d : {-step, step}) {
                auto valid = [&](int i) {
                    if (horizontal) return i >= 0 && i < width * height;
                    // JS Math.floor on negative indices: a pixel left of column 0 is on the row above
                    return int(std::floor(double(i) / width)) == int(std::floor(double(i - d) / width));
                };
                const int a = lose + d;
                const int b = keep + d;
                if (valid(a) && isOutline(a) && !(valid(b) && isOutline(b))) return false;
            }
            return true;
        };
        for (bool horizontal : {true, false}) {
            const int lines = horizontal ? height : width;
            const int along = horizontal ? width : height;
            auto at = [&](int line, int t) { return horizontal ? line * width + t : t * width + line; };
            for (int line = 0; line < lines; line++) {
                int t = 0;
                while (t < along) {
                    if (!isOutline(at(line, t))) {
                        t++;
                        continue;
                    }
                    int end = t;
                    while (end < along && isOutline(at(line, end))) end++;
                    if (end - t == 2) {
                        const int i = at(line, t);
                        const int j = at(line, t + 1);
                        const Box& a = boxes[size_t(i)];
                        const Box& b = boxes[size_t(j)];
                        const double cell = horizontal ? (a[2] - a[0] + b[2] - b[0]) / 2 : (a[3] - a[1] + b[3] - b[1]) / 2;
                        const Box both = {std::min(a[0], b[0]), std::min(a[1], b[1]), std::max(a[2], b[2]), std::max(a[3], b[3])};
                        if (span(both, horizontal) < 1.4 * cell) {
                            // The pixel holding less of the stroke goes, unless the outline would break without it
                            const bool firstLess = darkIn(a) < darkIn(b);
                            auto can = [&](int lose, int keep) { return removable(lose) && sameSides(lose, keep, horizontal); };
                            int loseFirst = -1;   // -1: neither can go
                            if (firstLess) loseFirst = can(i, j) ? 1 : can(j, i) ? 0 : -1;
                            else loseFirst = can(j, i) ? 0 : can(i, j) ? 1 : -1;
                            if (loseFirst >= 0) {
                                const int lose = loseFirst == 1 ? i : j;
                                const int from = loseFirst == 1 ? (t > 0 ? at(line, t - 1) : -1) : end < along ? at(line, end) : -1;
                                if (from < 0) indices[size_t(lose)] = TRANSPARENT;
                                else if (!isOutline(from)) indices[size_t(lose)] = indices[size_t(from)];
                            }
                        }
                    }
                    t = end;
                }
            }
        }
        return out;
    }

}
