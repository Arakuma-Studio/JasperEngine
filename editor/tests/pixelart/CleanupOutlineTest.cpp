// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/test/outline.test.ts, "outlines" and "bent grids"):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// S46 AC4 (R4, R11): one-cell strokes detected, a split stroke given back, a real 2 px stroke kept,
// the staircase whole, a warped grid recovered with bend but not without, every bent cut within
// about a pixel of the truth.

#include "pixelart/cleanup/CleanupGrid.h"
#include "pixelart/cleanup/CleanupOutline.h"
#include "pixelart/cleanup/CleanupPipeline.h"
#include "pixelart/cleanup/CleanupSample.h"
#include "pixelart/cleanup/CleanupSynthetic.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

using namespace jasperengine::editor::pixelart::cleanup;

namespace {

    const Rgba8 outlineW{250, 246, 236, 255};
    const Rgba8 outlineK{12, 10, 24, 255};
    const Rgba8 outlineB{90, 120, 220, 255};
    const std::string outlineKeys = "WKB";

    std::vector<Rgba8> outlinePalette() { return {outlineW, outlineK, outlineB}; }

    PixelArt outlineArt(const std::vector<std::string>& rows) {
        PixelArt art;
        art.width = int(rows[0].size());
        art.height = int(rows.size());
        art.palette = outlinePalette();
        for (const std::string& row : rows) {
            for (char letter : row) art.indices.push_back(letter == '.' ? TRANSPARENT : int32_t(outlineKeys.find(letter)));
        }
        return art;
    }

    std::vector<std::string> outlineLetters(const PixelArt& art) {
        std::vector<std::string> out;
        for (int y = 0; y < art.height; y++) {
            std::string row;
            for (int x = 0; x < art.width; x++) {
                const int32_t index = art.indices[size_t(y) * art.width + x];
                row += index == TRANSPARENT ? '.' : outlineKeys[size_t(index)];
            }
            out.push_back(row);
        }
        return out;
    }

    // A light 120 x 120 picture (12 x 12 cells of 10 px) with a dark vertical stroke from `from` to `to`, rows 10 to 110
    RgbaImage stroked(int from, int to) {
        RgbaImage image(120, 120);
        for (int y = 0; y < 120; y++) {
            for (int x = 0; x < 120; x++) {
                const Rgba8 color = x >= from && x < to && y >= 10 && y < 110 ? outlineK : outlineW;
                image.set(x, y, {color.r, color.g, color.b, 255});
            }
        }
        return image;
    }

    const GridSpec outlineCells{10, 10, 0, 0};

    Recipe outlineRecipe(Outlines outlines) {
        Recipe recipe;
        recipe.grid.kind = GridKind::Cells;
        recipe.grid.spec = outlineCells;
        RecipePalette palette;
        palette.name = "Test";
        palette.colors = outlinePalette();
        recipe.palette = palette;
        recipe.outlines = outlines;
        return recipe;
    }

    // The columns holding outline pixels, in row 5 of the fixed picture
    std::vector<int> darkColumns(const PixelArt& picture) {
        std::vector<int> out;
        for (int x = 0; x < picture.width; x++) {
            if (picture.indices[size_t(5) * picture.width + x] == 1) out.push_back(x);
        }
        return out;
    }

    std::vector<Rgba8> sweetie6() {
        std::vector<Rgba8> colors = sweetie16();
        colors.resize(6);
        return colors;
    }

}

TEST(CleanupOutline, JudgesDarkStrokesOneCellWideAndFindsNoneInAPictureWithout) {
    EXPECT_TRUE(hasOutlines(outlineMask(stroked(44, 56)), 10));
    EXPECT_FALSE(hasOutlines(outlineMask(stroked(30, 90)), 10));
    EXPECT_FALSE(hasOutlines(outlineMask(stroked(0, 0)), 10));
}

TEST(CleanupOutline, GivesAStrokeSplitByAGridLineBackToOnePixel) {
    // 12 px wide across cells 4 and 5: the middle of both reads as dark
    const RgbaImage image = stroked(44, 56);
    EXPECT_EQ(darkColumns(runRecipe(image, outlineRecipe(Outlines::Off))->art), (std::vector<int>{4, 5}));
    const Outcome fixed = *runRecipe(image, outlineRecipe(Outlines::Auto));
    EXPECT_TRUE(fixed.outlines);
    const std::vector<int> columns = darkColumns(fixed.art);
    ASSERT_EQ(columns.size(), 1u);
    // The whole stroke, top to bottom, stays one unbroken column
    for (int y = 1; y < 11; y++) EXPECT_EQ(fixed.art.indices[size_t(y) * 12 + columns[0]], 1) << "row " << y;
}

TEST(CleanupOutline, KeepsAStrokeReallyTwoPixelsThick) {
    EXPECT_EQ(darkColumns(runRecipe(stroked(40, 60), outlineRecipe(Outlines::On))->art), (std::vector<int>{4, 5}));
}

TEST(CleanupOutline, LeavesAStaircaseOutlineWhole) {
    const PixelArt stairs = outlineArt({"WWWWWW", "KKWWWW", "WKKWWW", "WWKKWW", "WWWKKW", "WWWWKK"});
    Upscale options;
    options.scale = 10;
    options.blur = 1;
    const RgbaImage image = upscale(stairs, options);
    const Grid grid = uniformGrid(image.width(), image.height(), outlineCells);
    QuantizeOptions snap;
    snap.palette = outlinePalette();
    snap.ditherStrength = 0;
    const PixelArt fixed = quantize(sampleCells(image, grid, {0.25, 128}), snap);
    EXPECT_EQ(outlineLetters(thinOutlines(fixed, grid, outlineMask(image))), outlineLetters(stairs));
}

TEST(CleanupBentGrid, FollowsAWarpedGridThatStraightLinesCannot) {
    for (uint32_t seed : {1u, 2u, 3u}) {
        const PixelArt sprite = makeSprite(24, 24, sweetie6(), seed);
        const RgbaImage image = warped(sprite, 10, 0, 0.45, 12);
        auto wrong = [&](Follow follow) {
            Recipe recipe;
            recipe.follow = follow;
            recipe.outlines = Outlines::Off;
            recipe.grid.kind = GridKind::Cells;
            recipe.grid.spec = outlineCells;
            RecipePalette palette;
            palette.name = "Sweetie";
            palette.colors = sprite.palette;
            recipe.palette = palette;
            const PixelArt fixed = runRecipe(image, recipe)->art;
            int count = 0;
            for (size_t i = 0; i < fixed.indices.size(); i++) count += fixed.indices[i] != sprite.indices[i] ? 1 : 0;
            return count;
        };
        EXPECT_GT(wrong(Follow::Off), 0) << "seed " << seed;
        // AC4: straight lines cannot follow it either
        EXPECT_GT(wrong(Follow::Straight), 0) << "seed " << seed;
        EXPECT_EQ(wrong(Follow::Bend), 0) << "seed " << seed;
    }
}

TEST(CleanupBentGrid, PutsEachBentLineWithinAPixelOrSoOfWhereTheCellsMeet) {
    const PixelArt sprite = makeSprite(24, 24, sweetie6(), 4);
    const RgbaImage image = warped(sprite, 10, 0, 0.3, 12);
    RefineOptions options;
    options.bend = true;
    const Grid grid = refineGrid(image, uniformGrid(image.width(), image.height(), outlineCells), options);
    ASSERT_TRUE(grid.bend);
    double total = 0;
    int count = 0;
    for (size_t i = 1; i + 1 < grid.xs.size(); i++) {
        for (size_t j = 0; j + 1 < grid.ys.size(); j++) {
            const double y = (grid.ys[j] + grid.ys[j + 1]) / 2;
            total += std::abs(grid.bend->xs[i][j] - (double(i) * 10 + 3 * std::sin((2 * std::numbers::pi * y) / 120)));
            count++;
        }
    }
    EXPECT_LT(total / count, 1.0);
    // Straight, each line sits where its bends average out
    EXPECT_LT(std::abs(grid.xs[12] - 120), 1.0);
}
