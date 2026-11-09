// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/test/core.test.ts, "grid"):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// S46 AC1 and AC3 (R2, R3, R4, R40): uniform cuts, no grid in a blank picture, drifting cells
// followed once refined, cells kept square, and the same detection whatever the thread count.
// Same generator, seeds and numbers as ritmo's tests.

#include "pixelart/cleanup/CleanupGrid.h"
#include "pixelart/cleanup/CleanupSample.h"
#include "pixelart/cleanup/CleanupSynthetic.h"

#include <gtest/gtest.h>

#include <cmath>
#include <thread>
#include <vector>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::cleanup;

namespace {

    int gridMismatches(const PixelArt& expected, const PixelArt& actual) {
        if (expected.width != actual.width || expected.height != actual.height) return 1 << 30;
        int count = 0;
        for (size_t i = 0; i < expected.indices.size(); i++) count += expected.indices[i] != actual.indices[i] ? 1 : 0;
        return count;
    }

    // core.test.ts's fix with `refine`: detect, cut, refine each axis on its own profile, snap
    struct GridFix {
        RgbaImage image;
        Detection detection;
        Grid grid;
        PixelArt art;
    };

    GridFix gridFixRefined(const PixelArt& sprite, const Upscale& options) {
        GridFix out;
        out.image = upscale(sprite, options);
        out.detection = *detectGrid(out.image);
        out.grid = uniformGrid(out.image.width(), out.image.height(), out.detection.spec);
        const EdgeProfiles profiles = edgeProfiles(out.image);
        out.grid.xs = refineCuts(profiles.columns, out.grid.xs, out.image.width());
        out.grid.ys = refineCuts(profiles.rows, out.grid.ys, out.image.height());
        QuantizeOptions snap;
        snap.palette = sprite.palette;
        out.art = quantize(sampleCells(out.image, out.grid, {0.25, 128}), snap);
        return out;
    }

    // A rows-only blur pass of core.test.ts's square-cells case
    RgbaImage blurRows(const RgbaImage& image, int passes) {
        std::vector<uint8_t> data(image.pixels().begin(), image.pixels().end());
        const int w = image.width(), h = image.height();
        for (int pass = 0; pass < passes; pass++) {
            std::vector<uint8_t> out(data.size());
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    for (int c = 0; c < 4; c++) {
                        int sum = 0, count = 0;
                        for (int ny = std::max(0, y - 1); ny <= std::min(h - 1, y + 1); ny++) {
                            sum += data[(size_t(ny) * w + x) * 4 + c];
                            count++;
                        }
                        out[(size_t(y) * w + x) * 4 + c] = uint8_t(std::floor(double(sum) / count + 0.5));
                    }
                }
            }
            data = std::move(out);
        }
        RgbaImage result(w, h);
        std::copy(data.begin(), data.end(), result.editPixels().begin());
        return result;
    }

}

TEST(CleanupGrid, KeepsPartialCellsAtTheEdgesOnlyWhenAtLeastHalfACell) {
    EXPECT_EQ(uniformCuts(20, 4, 1), (std::vector<double>{1, 5, 9, 13, 17, 20}));
    EXPECT_EQ(uniformCuts(18, 4, 1), (std::vector<double>{1, 5, 9, 13, 17}));
    EXPECT_EQ(uniformCuts(20, 4, 3), (std::vector<double>{0, 3, 7, 11, 15, 19}));
    EXPECT_EQ(uniformCuts(10, 2.5, 0), (std::vector<double>{0, 2.5, 5, 7.5, 10}));
    // A picture smaller than one cell is one cell
    EXPECT_EQ(uniformCuts(3, 8, 0), (std::vector<double>{0, 3}));
}

TEST(CleanupGrid, FollowsCellsThatDriftAcrossThePictureOnceRefined) {
    const PixelArt sprite = makeSprite(40, 40, sweetie16(), 5);
    Upscale options;
    options.scale = 8;
    options.offset = 2;
    options.drift = 0.12;
    options.blur = 1;
    options.noise = 4;
    const GridFix refined = gridFixRefined(sprite, options);
    EXPECT_EQ(gridMismatches(sprite, refined.art), 0);
    // Every refined cut lands within a pixel of where the sprite's cells really meet
    const std::vector<double> truth = boundaries(sprite.width, options);
    ASSERT_EQ(refined.grid.xs.size(), truth.size());
    for (size_t i = 1; i + 1 < truth.size(); i++) EXPECT_LE(std::abs(refined.grid.xs[i] - truth[i]), 1.0) << "cut " << i;
}

TEST(CleanupGrid, KeepsCellsSquareWhenBlurAlongOneAxisMakesAMultipleLookCleaner) {
    // Blurred again only up and down, as a resized screenshot can be
    Upscale options;
    options.scale = 9.4;
    options.offset = 3;
    options.blur = 1;
    options.noise = 6;
    options.seed = 1;
    const RgbaImage image = blurRows(upscale(demoSprite(), options), 6);
    const std::optional<Detection> detection = detectGrid(image);
    ASSERT_TRUE(detection);
    EXPECT_LT(std::abs(detection->spec.cellWidth - 9.4), 0.05);
    EXPECT_LT(std::abs(detection->spec.cellHeight - 9.4), 0.05);
}

TEST(CleanupGrid, FindsCellsAScreenshotStretchedOnOneAxisAndKeepsThemNearSquare) {
    // Every row moved to 96.5 % of its height, smoothly: cells 31.5 wide and about 30.4 tall
    Upscale options;
    options.scale = 31.5;
    options.offset = 3;
    options.blur = 1;
    options.noise = 6;
    options.seed = 1;
    const RgbaImage wide = upscale(demoSprite(), options);
    const double squeeze = 0.965;
    const int height = int(std::floor(wide.height() * squeeze));
    RgbaImage image(wide.width(), height);
    std::span<uint8_t> data = image.editPixels();
    for (int y = 0; y < height; y++) {
        const double from = std::min(double(wide.height() - 1), y / squeeze);
        const int top = int(std::floor(from));
        const int bottom = std::min(wide.height() - 1, top + 1);
        const double t = from - top;
        for (int i = 0; i < wide.width() * 4; i++) {
            const double value = wide.row(top)[i] * (1 - t) + wide.row(bottom)[i] * t;
            data[size_t(y) * wide.width() * 4 + size_t(i)] = uint8_t(std::floor(value + 0.5));
        }
    }
    const std::optional<Detection> detection = detectGrid(image);
    ASSERT_TRUE(detection);
    // toBeCloseTo(_, 0): within 0.5
    EXPECT_LT(std::abs(detection->spec.cellWidth - 31.5), 0.5);
    EXPECT_LT(std::abs(detection->spec.cellHeight - 31.5 * squeeze), 0.5);
}

TEST(CleanupGrid, DoesNotInventAGridInABlankPicture) {
    EXPECT_FALSE(detectGrid(RgbaImage(40, 40)));
}

TEST(CleanupGrid, DetectsTheSameGridWhateverTheThreadCount) {
    Upscale options;
    options.scale = 7.6;
    options.offset = 3;
    options.blur = 1;
    options.noise = 6;
    const RgbaImage image = upscale(makeSprite(32, 24, sweetie16(), 11), options);
    const Detection serial = *detectGrid(image);
    // Eight threads, each taking every eighth index: a split unlike the serial one
    setParallelFor([](int count, const std::function<void(int)>& body) {
        std::vector<std::thread> workers;
        for (int t = 0; t < 8; t++) {
            workers.emplace_back([&, t] {
                for (int i = t; i < count; i += 8) body(i);
            });
        }
        for (std::thread& worker : workers) worker.join();
    });
    const Detection parallel = *detectGrid(image);
    setParallelFor(nullptr);
    EXPECT_EQ(serial.spec, parallel.spec);
    EXPECT_EQ(serial.confidence, parallel.confidence);
    EXPECT_EQ(serial.x.confidence, parallel.x.confidence);
    EXPECT_EQ(serial.y.confidence, parallel.y.confidence);
}
