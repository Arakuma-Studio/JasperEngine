// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Palette window's usage counts (docs/specs/S43-palettes.md R33, R31) and the content hash
// they are cached by.

#include "pixelart/PaletteUsage.h"
#include "pixelart/PixelColor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

namespace paletteusage_test {

    namespace px = jasperengine::pixelart;
    using namespace jasperengine::editor::pixelart;
    using px::Rgba8;

    const Rgba8 kBlack{0, 0, 0, 255};
    const Rgba8 kWhite{255, 255, 255, 255};
    const Rgba8 kRed{255, 0, 0, 255};

    struct UsageRandom {
        uint32_t state = 12345u;
        uint32_t next() {
            state = state * 1664525u + 1013904223u;
            return state >> 8;
        }
    };

    bool sameRgb(Rgba8 x, Rgba8 y) { return x.r == y.r && x.g == y.g && x.b == y.b; }

    RgbaImage randomImage(int width, int height, const std::vector<Rgba8>& palette, const std::vector<Rgba8>& off, uint32_t seed) {
        UsageRandom random;
        random.state = seed;
        RgbaImage image(width, height);
        for (Rgba8& pixel : image.pixels) {
            const uint32_t roll = random.next() % 100;
            if (roll < 70) {
                pixel = palette[random.next() % palette.size()];
            } else if (roll < 90) {
                pixel = off[random.next() % off.size()];
            } else {
                pixel = palette[random.next() % palette.size()];
                pixel.a = (uint8_t)(random.next() % 3 == 0 ? 0 : random.next() % 256);
            }
        }
        return image;
    }

    TEST(PaletteUsage, CountsEqualADirectCount) {
        const std::vector<Rgba8> palette = {kBlack, kWhite, kRed, {29, 43, 83, 255}, {0, 135, 81, 255}, {255, 236, 39, 255}};
        const std::vector<Rgba8> off = {{1, 0, 0, 255}, {128, 128, 128, 255}, {255, 0, 1, 200}, {30, 43, 83, 255}, {77, 66, 55, 255}};
        const RgbaImage image = randomImage(40, 30, palette, off, 99u);
        const PaletteUsageReport report = measureUsage(image, palette);

        std::vector<int64_t> counts(palette.size(), 0);
        std::map<uint32_t, int64_t> offCounts;
        int64_t visible = 0;
        for (const Rgba8& pixel : image.pixels) {
            if (pixel.a < 1) continue;
            visible++;
            bool found = false;
            for (size_t i = 0; i < palette.size() && !found; i++) {
                if (sameRgb(pixel, palette[i])) {
                    counts[i]++;
                    found = true;
                }
            }
            if (!found) offCounts[px::packRgb(pixel)]++;
        }
        EXPECT_EQ(report.counts, counts);
        int64_t offPixels = 0;
        for (const auto& entry : offCounts) offPixels += entry.second;
        EXPECT_EQ(report.offPalettePixels, offPixels);
        EXPECT_EQ(report.offColorCount, (int64_t)offCounts.size());
        int64_t counted = report.offPalettePixels;
        for (int64_t count : report.counts) counted += count;
        EXPECT_EQ(counted, visible);

        std::vector<std::pair<uint32_t, int64_t>> expected(offCounts.begin(), offCounts.end());
        std::stable_sort(expected.begin(), expected.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
        ASSERT_EQ(report.offColors.size(), expected.size());
        for (size_t i = 0; i < expected.size(); i++) {
            EXPECT_EQ(px::packRgb(report.offColors[i].color), expected[i].first) << i;
            EXPECT_EQ(report.offColors[i].color.a, 255);
            EXPECT_EQ(report.offColors[i].count, expected[i].second) << i;
        }
    }

    TEST(PaletteUsage, OffPaletteColoursWithNearestAndDeltaE) {
        const std::vector<Rgba8> palette = {kBlack, kWhite, kRed};
        RgbaImage image(9, 1);
        for (int x = 0; x < 5; x++) image.set(x, 0, Rgba8{250, 10, 10, 255});
        for (int x = 5; x < 8; x++) image.set(x, 0, Rgba8{20, 20, 20, 255});
        image.set(8, 0, Rgba8{240, 240, 240, 255});
        const PaletteUsageReport report = measureUsage(image, palette);
        EXPECT_EQ(report.offPalettePixels, 9);
        EXPECT_EQ(report.offColorCount, 3);
        ASSERT_EQ(report.offColors.size(), 3u);
        const int nearest[3] = {2, 0, 1};
        const int counts[3] = {5, 3, 1};
        for (int i = 0; i < 3; i++) {
            const PaletteUsageReport::OffColor& color = report.offColors[(size_t)i];
            EXPECT_EQ(color.count, counts[i]);
            EXPECT_EQ(color.nearest, nearest[i]);
            EXPECT_NEAR(color.deltaE, px::distance(color.color, palette[(size_t)nearest[i]], px::ColorMetric::OkLab) * 100.0f, 1e-4f);
            EXPECT_GT(color.deltaE, 0.0f);
        }

        const PaletteUsageReport limited = measureUsage(image, palette, 1, 2);
        EXPECT_EQ(limited.offColors.size(), 2u);
        EXPECT_EQ(limited.offColorCount, 3);
        EXPECT_EQ(limited.offPalettePixels, 9);
        EXPECT_TRUE(measureUsage(image, palette, 1, 0).offColors.empty());

        const PaletteUsageReport noPalette = measureUsage(image, {});
        EXPECT_TRUE(noPalette.counts.empty());
        ASSERT_EQ(noPalette.offColors.size(), 3u);
        EXPECT_EQ(noPalette.offColors[0].nearest, -1);
        EXPECT_EQ(noPalette.offColors[0].deltaE, 0.0f);
    }

    TEST(PaletteUsage, DuplicatesTransparentSlotAndAlphaThreshold) {
        const std::vector<Rgba8> palette = {{0, 0, 0, 0}, kBlack, kWhite, kBlack};
        RgbaImage image(9, 1);
        for (int x = 0; x < 4; x++) image.set(x, 0, kBlack);
        for (int x = 4; x < 6; x++) image.set(x, 0, Rgba8{255, 255, 255, 100});
        for (int x = 6; x < 9; x++) image.set(x, 0, Rgba8{0, 0, 0, 0});

        // the transparent slot matches nothing and the duplicate counts on its first index
        EXPECT_EQ(measureUsage(image, palette).counts, (std::vector<int64_t>{0, 4, 2, 0}));
        EXPECT_EQ(measureUsage(image, palette, 128).counts, (std::vector<int64_t>{0, 4, 0, 0}));
        EXPECT_EQ(measureUsage(image, palette, 0).counts, (std::vector<int64_t>{0, 7, 2, 0}));
        EXPECT_EQ(measureUsage(image, palette).offPalettePixels, 0);

        // a palette of transparent slots only: they are its colours
        const std::vector<Rgba8> clear = {{0, 0, 0, 0}, {255, 255, 255, 0}};
        EXPECT_EQ(measureUsage(image, clear).counts, (std::vector<int64_t>{4, 2}));

        const PaletteUsageReport empty = measureUsage(RgbaImage(), palette);
        EXPECT_EQ(empty.counts, (std::vector<int64_t>{0, 0, 0, 0}));
        EXPECT_EQ(empty.offPalettePixels, 0);
    }

    TEST(PaletteUsage, ContentHashIsStableAndSensitive) {
        const std::vector<Rgba8> palette = {kBlack, kWhite, kRed};
        const RgbaImage image = randomImage(16, 16, palette, {{1, 2, 3, 255}}, 7u);
        const RgbaImage copy = image;
        const uint64_t hash = imageContentHash(image);
        EXPECT_EQ(imageContentHash(copy), hash);
        EXPECT_EQ(imageContentHash(image), hash);
        EXPECT_EQ(measureUsage(image, palette).contentHash, hash);

        RgbaImage changed = image;
        Rgba8 pixel = changed.at(9, 4);
        pixel.a = (uint8_t)(pixel.a ^ 1u);
        changed.set(9, 4, pixel);
        EXPECT_NE(imageContentHash(changed), hash);

        RgbaImage wide(8, 2, kRed), tall(2, 8, kRed);
        EXPECT_NE(imageContentHash(wide), imageContentHash(tall));

        // the definition, pinned: FNV-1a 64 over the little-endian width and height, then RGBA bytes
        RgbaImage tiny(2, 1);
        tiny.set(0, 0, Rgba8{1, 2, 3, 4});
        tiny.set(1, 0, Rgba8{5, 6, 7, 8});
        EXPECT_EQ(imageContentHash(tiny), 0xf86dd2cd636ccf86ull);
        EXPECT_EQ(imageContentHash(RgbaImage()), 0xa8c7f832281a39c5ull);
    }

    TEST(PaletteUsage, ExactColorIndexAndTally) {
        const std::vector<Rgba8> colors = {{0, 0, 0, 0}, kBlack, kWhite, {0, 0, 0, 128}};
        const ExactColorIndex all(colors);
        EXPECT_EQ(all.find(kBlack), 0);
        EXPECT_EQ(all.find(Rgba8{255, 255, 255, 3}), 2);
        EXPECT_EQ(all.find(kRed), -1);
        const ExactColorIndex visible(colors, true);
        EXPECT_EQ(visible.find(kBlack), 1);

        ColorTally tally;
        EXPECT_EQ(tally.size(), 0u);
        EXPECT_TRUE(tally.entries().empty());
        for (uint32_t key = 0; key < 5000; key++) tally.add(key % 1000, 2);
        tally.add(0xffffffffu);
        EXPECT_EQ(tally.size(), 1001u);
        const std::vector<std::pair<uint32_t, int64_t>> entries = tally.entries();
        ASSERT_EQ(entries.size(), 1001u);
        EXPECT_EQ(entries.front(), (std::pair<uint32_t, int64_t>{0u, 10}));
        EXPECT_EQ(entries.back(), (std::pair<uint32_t, int64_t>{0xffffffffu, 1}));
    }

    TEST(PaletteUsage, Benchmark1024) {
        std::vector<Rgba8> palette;
        for (int i = 0; i < 64; i++) palette.push_back(Rgba8{(uint8_t)(i * 4), (uint8_t)(255 - i * 4), (uint8_t)((i * 37) % 256), 255});
        std::vector<Rgba8> off;
        for (int i = 0; i < 200; i++) off.push_back(Rgba8{(uint8_t)(i + 3), (uint8_t)(i * 3 % 256), 1, 255});
        const RgbaImage image = randomImage(1024, 1024, palette, off, 2026u);
        const auto start = std::chrono::steady_clock::now();
        const PaletteUsageReport report = measureUsage(image, palette);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("[bench] palette usage 1024x1024, 64 colours, %lld off-palette colours: %.1f ms (target <= 30 ms, R31)\n",
                    (long long)report.offColorCount, ms);
        EXPECT_GT(report.offPalettePixels, 0);
    }

}
