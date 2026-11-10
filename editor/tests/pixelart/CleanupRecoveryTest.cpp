// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/test/core.test.ts, "grid" and "fixing"):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// S46 AC2 (R3, R5, R7, R8, R36): known sprites made into fake AI art by demo.ts's generator come
// back exactly: the cell within 0.05 of the scale, confidence above 0.5, 0 mismatched pixels.
// The same generator, seeds and numbers as ritmo's tests.

#include "pixelart/cleanup/CleanupGrid.h"
#include "pixelart/cleanup/CleanupSample.h"
#include "pixelart/cleanup/CleanupSynthetic.h"
#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::cleanup;
using jasperengine::pixelart::ColorMetric;

namespace {

    struct RecoveryFix {
        RgbaImage image;
        Detection detection;
        Grid grid;
        PixelArt art;
    };

    RecoveryFix recoveryFix(const PixelArt& sprite, const Upscale& options) {
        RecoveryFix out;
        out.image = upscale(sprite, options);
        const std::optional<Detection> detection = detectGrid(out.image);
        EXPECT_TRUE(detection);
        if (!detection) return out;
        out.detection = *detection;
        out.grid = uniformGrid(out.image.width(), out.image.height(), out.detection.spec);
        QuantizeOptions snap;
        snap.palette = sprite.palette;
        snap.ditherStrength = 0;
        out.art = quantize(sampleCells(out.image, out.grid, {0.25, 128}), snap);
        return out;
    }

    int recoveryMismatches(const PixelArt& expected, const PixelArt& actual) {
        if (expected.width != actual.width || expected.height != actual.height) return 1 << 30;
        int count = 0;
        for (size_t i = 0; i < expected.indices.size(); i++) count += expected.indices[i] != actual.indices[i] ? 1 : 0;
        return count;
    }

    Upscale upscaled(double scale, double offset, int blur = 0, double noise = 0) {
        Upscale options;
        options.scale = scale;
        options.offset = offset;
        options.blur = blur;
        options.noise = noise;
        return options;
    }

    // toBeCloseTo(_, 1): closer than 0.05
    void expectRecovered(const Upscale& options) {
        const PixelArt sprite = makeSprite(32, 24, sweetie16(), 11);
        const RecoveryFix fixed = recoveryFix(sprite, options);
        EXPECT_LT(std::abs(fixed.detection.spec.cellWidth - options.scale), 0.05);
        EXPECT_LT(std::abs(fixed.detection.spec.cellHeight - options.scale), 0.05);
        EXPECT_GT(fixed.detection.confidence, 0.5);
        EXPECT_EQ(recoveryMismatches(sprite, fixed.art), 0);
    }

}

TEST(CleanupRecovery, GetsTheSpriteBackExactlyAtWhole8x) { expectRecovered(upscaled(8, 0)); }
TEST(CleanupRecovery, GetsTheSpriteBackExactlyAtWhole5xShifted) { expectRecovered(upscaled(5, 2)); }
TEST(CleanupRecovery, GetsTheSpriteBackExactlyAt7_6xBlurredAndNoisy) { expectRecovered(upscaled(7.6, 3, 1, 6)); }
TEST(CleanupRecovery, GetsTheSpriteBackExactlyAt12_3xBlurredTwice) { expectRecovered(upscaled(12.3, 5, 2, 4)); }
TEST(CleanupRecovery, GetsTheSpriteBackExactlyAtSmall3x) { expectRecovered(upscaled(3, 1)); }

// R8's vote, pinned where it differs from the median: at AC2's own noise levels the two agree on
// every cell, so these are the cases that fail when the vote is replaced by the median
TEST(CleanupRecovery, VotingGetsAVeryNoisySpriteBackWhereTheMedianDoesNot) {
    // Not one of AC2's cases: detection is rightly unsure here (confidence 0.36), only the snap is judged
    const PixelArt sprite = makeSprite(32, 24, sweetie16(), 11);
    EXPECT_EQ(recoveryMismatches(sprite, recoveryFix(sprite, upscaled(7.6, 3, 1, 20)).art), 0);
}

TEST(CleanupRecovery, TheVoteNeverPicksAColourTheCellsPixelsDidNotVoteFor) {
    // Two red and two green pixels and one yellow: the per-channel median is yellow, which only
    // one pixel is. Red and green tie with two votes and the lowest index wins.
    const Rgba8 red{255, 0, 0, 255}, green{0, 255, 0, 255}, yellow{255, 255, 0, 255};
    CellSamples samples;
    samples.width = samples.height = 1;
    Cell cell;
    for (const Rgba8& color : {red, red, green, green, yellow}) cell.colors.push_back(jasperengine::pixelart::packRgb(color));
    samples.cells.push_back(cell);
    QuantizeOptions snap;
    snap.palette = {red, green, yellow};
    snap.ditherStrength = 0;
    EXPECT_EQ(quantize(samples, snap).indices[0], 0);
}

TEST(CleanupRecovery, FindsTheGridOfASmallSpriteInAWideEmptyBackground) {
    const PixelArt sprite = pad(makeSprite(16, 12, sweetie16(), 21), 14);
    const RecoveryFix fixed = recoveryFix(sprite, upscaled(9.5, 0, 1, 3));
    EXPECT_LT(std::abs(fixed.detection.spec.cellWidth - 9.5), 0.05);
    EXPECT_EQ(recoveryMismatches(sprite, fixed.art), 0);
}

TEST(CleanupRecovery, FindsALargeGridInALargePictureQuickly) {
    const PixelArt sprite = makeSprite(48, 48, sweetie16(), 8);
    const RgbaImage image = upscale(sprite, upscaled(21.3, 4, 2, 5));
    // R36's budget is with the editor's 8 workers
    setParallelFor([](int count, const std::function<void(int)>& body) {
        std::atomic<int> next{0};
        std::vector<std::thread> workers;
        for (int w = 0; w < 8; w++) {
            workers.emplace_back([&]() {
                for (int i = next++; i < count; i = next++) body(i);
            });
        }
        for (std::thread& worker : workers) worker.join();
    });
    const auto started = std::chrono::steady_clock::now();
    const std::optional<Detection> detection = detectGrid(image);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    setParallelFor(nullptr);
    std::printf("[bench] detectGrid %dx%d with 8 workers: %.1f ms (budget 120, R36)\n", image.width(), image.height(), ms);
    if (jasperengine::editor::test::perfSlack() > 0.0) {
        EXPECT_LT(ms, 120.0 * jasperengine::editor::test::perfSlack());
    }
    ASSERT_TRUE(detection);
    EXPECT_LT(std::abs(detection->spec.cellWidth - 21.3), 0.05);
    QuantizeOptions snap;
    snap.palette = sprite.palette;
    const PixelArt art = quantize(sampleCells(image, uniformGrid(image.width(), image.height(), detection->spec), {0.25, 128}), snap);
    EXPECT_EQ(recoveryMismatches(sprite, art), 0);
}

TEST(CleanupRecovery, FixesTheDemoPictureBackIntoTheMushroomInSweetie16) {
    const RgbaImage image = demoImage();
    const std::optional<Detection> detection = detectGrid(image);
    ASSERT_TRUE(detection);
    EXPECT_GT(detection->confidence, 0.5);
    const Grid grid = refineGrid(image, uniformGrid(image.width(), image.height(), detection->spec));
    QuantizeOptions snap;
    snap.palette = sweetie16();
    snap.ditherStrength = 0;
    const PixelArt art = quantize(sampleCells(image, grid, SampleOptions{}), snap);
    const PixelArt sprite = demoSprite();
    ASSERT_EQ(art.width, 16);
    ASSERT_EQ(art.height, 16);
    for (size_t i = 0; i < art.indices.size(); i++) {
        EXPECT_EQ(art.palette[size_t(art.indices[i])], sprite.palette[size_t(sprite.indices[i])]) << "pixel " << i;
    }
}

TEST(CleanupRecovery, KeepsTransparency) {
    const PixelArt sprite = makeSprite(20, 20, sweetie16(), 3, true);
    const RecoveryFix fixed = recoveryFix(sprite, upscaled(6, 0, 1));
    EXPECT_EQ(recoveryMismatches(sprite, fixed.art), 0);
    EXPECT_EQ(fixed.art.indices[0], TRANSPARENT);
}

TEST(CleanupRecovery, SnapsColoursOutsideThePaletteToTheNearest) {
    const PixelArt sprite = makeSprite(16, 16, sweetie16(), 9);
    Upscale options = upscaled(4, 0, 0, 20);
    options.seed = 3;
    const RgbaImage image = upscale(sprite, options);
    const Grid grid = uniformGrid(image.width(), image.height(), GridSpec{4, 4, 0, 0});
    QuantizeOptions snap;
    snap.palette = sweetie16();
    snap.metric = ColorMetric::Ciede2000;
    snap.ditherStrength = 0;
    EXPECT_EQ(recoveryMismatches(sprite, quantize(sampleCells(image, grid, {0, 128}), snap)), 0);
}

TEST(CleanupRecovery, KeepsEachCellsOwnColourWithoutAPalette) {
    const PixelArt sprite = makeSprite(10, 10, sweetie16(), 2);
    const RgbaImage image = upscale(sprite, upscaled(4, 0));
    const Grid grid = uniformGrid(image.width(), image.height(), GridSpec{4, 4, 0, 0});
    QuantizeOptions plain;
    plain.ditherStrength = 0;
    const PixelArt art = quantize(sampleCells(image, grid, {0.25, 128}), plain);
    for (size_t i = 0; i < art.indices.size(); i++) {
        EXPECT_EQ(art.palette[size_t(art.indices[i])], sprite.palette[size_t(sprite.indices[i])]) << "pixel " << i;
    }
}

TEST(CleanupRecovery, MixesTwoColoursWithOrderedDitheringForAShadeBetweenThem) {
    CellSamples gray;
    gray.width = gray.height = 8;
    gray.cells.assign(64, Cell{{0x808080u}, 0});
    QuantizeOptions flat;
    flat.palette = {Rgba8{0, 0, 0, 255}, Rgba8{255, 255, 255, 255}};
    flat.ditherStrength = 1;
    QuantizeOptions dithered = flat;
    dithered.dither = Dither::Ordered;
    const PixelArt a = quantize(gray, flat);
    const PixelArt b = quantize(gray, dithered);
    EXPECT_EQ(std::set<int32_t>(a.indices.begin(), a.indices.end()).size(), 1u);
    EXPECT_EQ(std::set<int32_t>(b.indices.begin(), b.indices.end()).size(), 2u);
}
