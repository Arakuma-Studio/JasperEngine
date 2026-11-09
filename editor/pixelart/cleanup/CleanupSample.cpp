// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/fix.ts, export.ts, sprite.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

#include "CleanupSample.h"

#include "CleanupCommon.h"
#include "CleanupGrid.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace jasperengine::editor::pixelart::cleanup {

    using jasperengine::pixelart::PaletteMatcher;
    using jasperengine::pixelart::packRgb;

    // file-local helpers, in a namespace of their own so unity builds keep them apart
    namespace fix_ts {

        // fix.ts's Bayer 4 x 4, centred on 0
        constexpr std::array<double, 16> bayer4Centered() {
            constexpr int order[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
            std::array<double, 16> out{};
            for (int i = 0; i < 16; i++) out[size_t(i)] = (order[i] + 0.5) / 16 - 0.5;
            return out;
        }

        bool cellOpaque(const Cell& cell) { return int(cell.colors.size()) > cell.transparent; }

        // Pixels whose centres fall inside the trimmed cell; at least the one in the middle
        std::pair<int, int> sampleSpan(double from, double to, int limit, double inset) {
            const double margin = (to - from) * inset;
            int first = int(std::ceil(from + margin - 0.5));
            int last = int(std::floor(to - margin - 0.5));
            if (last < first) first = last = int(std::floor((from + to) / 2));
            return {std::max(0, std::min(limit - 1, first)), std::max(0, std::min(limit - 1, last))};
        }

    }

    CellSamples sampleCells(const RgbaImage& image, const Grid& grid, const SampleOptions& options) {
        using namespace fix_ts;
        const GridSize size = gridSize(grid);
        const double inset = std::min(0.45, std::max(0.0, options.inset));
        CellSamples samples;
        samples.width = size.width;
        samples.height = size.height;
        samples.cells.resize(size_t(size.width) * size_t(size.height));
        const uint8_t* data = image.pixels().data();
        for (int cy = 0; cy < size.height; cy++) {
            for (int cx = 0; cx < size.width; cx++) {
                const std::array<double, 4> box = cellBox(grid, cx, cy);
                const auto [x0, x1] = sampleSpan(box[0], box[2], image.width(), inset);
                const auto [y0, y1] = sampleSpan(box[1], box[3], image.height(), inset);
                Cell& cell = samples.cells[size_t(cy) * size_t(size.width) + size_t(cx)];
                cell.colors.reserve(size_t(x1 - x0 + 1) * size_t(y1 - y0 + 1));
                for (int y = y0; y <= y1; y++) {
                    for (int x = x0; x <= x1; x++) {
                        const uint8_t* p = data + (size_t(y) * size_t(image.width()) + size_t(x)) * 4;
                        if (p[3] < options.alphaThreshold) {
                            cell.transparent++;
                        } else {
                            cell.colors.push_back((uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]));
                        }
                    }
                }
            }
        }
        return samples;
    }

    Rgba8 medianColor(const std::vector<uint32_t>& colors) {
        using namespace fix_ts;
        if (colors.empty()) return Rgba8{0, 0, 0, 255};
        // The element at n / 2 of the sorted values, as medianColor in fix.ts takes it. A cell of a
        // few pixels selects it directly (a slider re-run reads every cell's median, R39); a big
        // cell's values are many and only 256 apart, so it counts them.
        constexpr size_t kSelected = 64;
        if (colors.size() <= kSelected) {
            std::array<uint8_t, kSelected> values{};
            auto selected = [&](int shift) {
                for (size_t i = 0; i < colors.size(); i++) values[i] = uint8_t((colors[i] >> shift) & 255u);
                const auto middle = values.begin() + std::ptrdiff_t(colors.size() / 2);
                std::nth_element(values.begin(), middle, values.begin() + std::ptrdiff_t(colors.size()));
                return *middle;
            };
            const uint8_t r = selected(16), g = selected(8), b = selected(0);
            return Rgba8{r, g, b, 255};
        }
        auto median = [&](int shift) {
            std::array<int, 256> counts{};
            for (uint32_t color : colors) counts[(color >> shift) & 255u]++;
            const size_t wanted = colors.size() / 2;
            size_t seen = 0;
            for (int v = 0; v < 256; v++) {
                seen += size_t(counts[size_t(v)]);
                if (seen > wanted) return uint8_t(v);
            }
            return uint8_t(255);
        };
        return Rgba8{median(16), median(8), median(0), 255};
    }

    std::vector<Rgba8> cellColors(const CellSamples& samples) {
        using namespace fix_ts;
        std::vector<Rgba8> out;
        for (const Cell& cell : samples.cells) {
            if (cellOpaque(cell)) out.push_back(medianColor(cell.colors));
        }
        return out;
    }

    PixelArt quantize(const CellSamples& samples, const QuantizeOptions& options) {
        using namespace fix_ts;
        PixelArt art;
        art.width = samples.width;
        art.height = samples.height;
        art.indices.assign(samples.cells.size(), TRANSPARENT);

        if (options.palette.empty()) {
            std::unordered_map<uint32_t, int32_t> slots;
            for (size_t i = 0; i < samples.cells.size(); i++) {
                const Cell& cell = samples.cells[i];
                if (!cellOpaque(cell)) continue;
                const Rgba8 color = medianColor(cell.colors);
                const auto [it, inserted] = slots.emplace(packRgb(color), int32_t(art.palette.size()));
                if (inserted) art.palette.push_back(color);
                art.indices[i] = it->second;
            }
            return art;
        }

        art.palette = options.palette;
        for (Rgba8& color : art.palette) color.a = 255;
        PaletteMatcher matcher(art.palette, options.metric);
        static constexpr std::array<double, 16> bayer4 = bayer4Centered();
        std::vector<int> votes(art.palette.size());
        for (size_t i = 0; i < samples.cells.size(); i++) {
            const Cell& cell = samples.cells[i];
            if (!cellOpaque(cell)) continue;
            if (options.dither == Dither::Ordered) {
                const Rgba8 color = medianColor(cell.colors);
                const int x = int(i % size_t(art.width));
                const int y = int(i / size_t(art.width));
                const double push = bayer4[size_t((y % 4) * 4 + (x % 4))] * options.ditherStrength * 64;
                art.indices[i] = matcher.nearest(Rgba8{channel(color.r + push), channel(color.g + push), channel(color.b + push), 255});
                continue;
            }
            std::fill(votes.begin(), votes.end(), 0);
            for (uint32_t color : cell.colors) votes[size_t(matcher.nearest(rgbOf(color)))]++;
            size_t best = 0;
            for (size_t v = 1; v < votes.size(); v++) {
                if (votes[v] > votes[best]) best = v;
            }
            art.indices[i] = int32_t(best);
        }
        return art;
    }

    std::vector<UsedColor> usedColors(const PixelArt& art) {
        using namespace fix_ts;
        std::vector<int> counts(art.palette.size(), 0);
        for (int32_t index : art.indices) {
            if (index >= 0) counts[size_t(index)]++;
        }
        std::vector<UsedColor> out;
        for (size_t i = 0; i < art.palette.size(); i++) {
            if (counts[i] > 0) out.push_back({art.palette[i], counts[i]});
        }
        return out;
    }

    PixelArt cropToContent(const PixelArt& art) {
        using namespace fix_ts;
        int left = art.width, right = -1, top = art.height, bottom = -1;
        for (size_t i = 0; i < art.indices.size(); i++) {
            if (art.indices[i] == TRANSPARENT) continue;
            const int x = int(i % size_t(art.width));
            const int y = int(i / size_t(art.width));
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
        if (right < left || (left == 0 && top == 0 && right == art.width - 1 && bottom == art.height - 1)) return art;
        PixelArt out;
        out.width = right - left + 1;
        out.height = bottom - top + 1;
        out.palette = art.palette;
        out.indices.resize(size_t(out.width) * size_t(out.height));
        for (int y = 0; y < out.height; y++) {
            const int32_t* from = art.indices.data() + size_t(top + y) * size_t(art.width) + size_t(left);
            std::copy(from, from + out.width, out.indices.begin() + long(size_t(y) * size_t(out.width)));
        }
        return out;
    }

    RgbaImage toRgba(const PixelArt& art, int scale) {
        using namespace fix_ts;
        const int factor = std::max(1, scale);
        RgbaImage image(art.width * factor, art.height * factor);
        if (image.empty()) return image;
        std::span<uint8_t> data = image.editPixels();
        for (int y = 0; y < image.height(); y++) {
            for (int x = 0; x < image.width(); x++) {
                const int32_t index = art.indices[size_t(y / factor) * size_t(art.width) + size_t(x / factor)];
                if (index == TRANSPARENT) continue;
                const Rgba8 color = art.palette[size_t(index)];
                uint8_t* p = data.data() + (size_t(y) * size_t(image.width()) + size_t(x)) * 4;
                p[0] = color.r;
                p[1] = color.g;
                p[2] = color.b;
                p[3] = 255;
            }
        }
        return image;
    }

    ArtOfImage artOfImage(const RgbaImage& image, int alphaThreshold) {
        using namespace fix_ts;
        ArtOfImage out;
        out.art.width = image.width();
        out.art.height = image.height();
        const size_t count = size_t(image.width()) * size_t(image.height());
        out.art.indices.assign(count, TRANSPARENT);
        std::unordered_map<uint32_t, int32_t> slots;
        const uint8_t* data = image.pixels().data();
        for (size_t i = 0; i < count; i++) {
            const uint8_t* p = data + i * 4;
            if (p[3] > 0 && p[3] < 255) out.partial++;
            if (p[3] < alphaThreshold) continue;
            const uint32_t key = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
            const auto [it, inserted] = slots.emplace(key, int32_t(out.art.palette.size()));
            if (inserted) out.art.palette.push_back(Rgba8{p[0], p[1], p[2], 255});
            out.art.indices[i] = it->second;
        }
        return out;
    }

}
