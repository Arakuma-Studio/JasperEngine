// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/test/outline.test.ts "shrinking with outlines" and
// features.test.ts "reduce"): MIT License, Copyright (c) 2026 Sandro Maglione.

// S46 AC6 (R12): a one-pixel ring shrunk stays closed and one pixel thick, a one-pixel step becomes
// a diagonal, a real corner stays, and Common takes the majority colour of each block.

#include "pixelart/cleanup/CleanupReduce.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine::editor::pixelart::cleanup;

namespace {

    using Rows = std::vector<std::string>;

    // outline.test.ts's colours; K is index 1
    PixelArt strokeArt(const Rows& rows) {
        const std::string keys = "WKB";
        PixelArt art;
        art.width = int(rows[0].size());
        art.height = int(rows.size());
        art.palette = {Rgba8{250, 246, 236, 255}, Rgba8{12, 10, 24, 255}, Rgba8{90, 120, 220, 255}};
        for (const std::string& row : rows) {
            for (char letter : row) art.indices.push_back(letter == '.' ? TRANSPARENT : int32_t(keys.find(letter)));
        }
        return art;
    }

    // features.test.ts's colours
    const std::string reduceKeys = "WPKBSG";

    PixelArt reduceArt(const Rows& rows) {
        PixelArt art;
        art.width = int(rows[0].size());
        art.height = int(rows.size());
        art.palette = {Rgba8{255, 255, 255, 255}, Rgba8{246, 214, 236, 255}, Rgba8{10, 8, 20, 255},
                       Rgba8{120, 150, 230, 255}, Rgba8{240, 200, 170, 255}, Rgba8{200, 200, 200, 255}};
        for (const std::string& row : rows) {
            for (char letter : row) art.indices.push_back(letter == '.' ? TRANSPARENT : int32_t(reduceKeys.find(letter)));
        }
        return art;
    }

    Rows lettersOf(const PixelArt& art, const std::string& keys) {
        Rows out;
        for (int y = 0; y < art.height; y++) {
            std::string row;
            for (int x = 0; x < art.width; x++) {
                const int32_t index = art.indices[size_t(y) * art.width + x];
                row += index == TRANSPARENT ? '.' : keys[size_t(index)];
            }
            out.push_back(row);
        }
        return out;
    }

    // Groups of outline pixels (index 1) touching, diagonals included
    int strokeCount(const PixelArt& picture) {
        std::vector<uint8_t> seen(picture.indices.size(), 0);
        int groups = 0;
        for (size_t start = 0; start < picture.indices.size(); start++) {
            if (picture.indices[start] != 1 || seen[start]) continue;
            groups++;
            std::vector<int> stack{int(start)};
            seen[start] = 1;
            while (!stack.empty()) {
                const int i = stack.back();
                stack.pop_back();
                const int x = i % picture.width;
                const int y = (i - x) / picture.width;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= picture.width || ny >= picture.height) continue;
                        const size_t n = size_t(ny) * picture.width + nx;
                        if (picture.indices[n] == 1 && !seen[n]) {
                            seen[n] = 1;
                            stack.push_back(int(n));
                        }
                    }
                }
            }
        }
        return groups;
    }

    bool outlineIsOne(int32_t index) { return index == 1; }

}

TEST(CleanupReduce, KeepsAOnePixelRingWholeWhereTheMostCoveringColourLosesItsSides) {
    const PixelArt ring = strokeArt({
        "................", "....KKKKKKKK....", "..KKKBBBBBBKKK..", "..KBBBBBBBBBBK..", ".KKBBBBBBBBBBKK.", ".KBBBBBBBBBBBBK.",
        ".KBBBBBBBBBBBBK.", ".KBBBBBBBBBBBBK.", ".KBBBBBBBBBBBBK.", ".KBBBBBBBBBBBBK.", ".KBBBBBBBBBBBBK.", ".KKBBBBBBBBBBKK.",
        "..KBBBBBBBBBBK..", "..KKKBBBBBBKKK..", "....KKKKKKKK....", "................",
    });
    const PixelArt reduced = reduce(ring, 11, 11, ReduceMode::Outlines);
    EXPECT_EQ(lettersOf(reduced, "WKB"), (Rows{"...........", "..KKKKKKK..", ".KBBBBBBBK.", ".KBBBBBBBK.", ".KBBBBBBBK.", ".KBBBBBBBK.",
                                               ".KBBBBBBBK.", ".KBBBBBBBK.", ".KBBBBBBBK.", "..KKKKKKK..", "..........."}));
    EXPECT_EQ(strokeCount(reduced), 1);
}

TEST(CleanupReduce, ShrinksARingByTwoClosedAndOnePixelThick) {
    // AC6's case: a 16 x 16 one-pixel ring, halved
    const PixelArt ring = strokeArt({
        "................", ".....KKKKKK.....", "...KKBBBBBBKK...", "..KBBBBBBBBBBK..", "..KBBBBBBBBBBK..", ".KBBBBBBBBBBBBK.",
        ".KBBBBBBBBBBBBK.", ".KBBBBBBBBBBBBK.", ".KBBBBBBBBBBBBK.", ".KBBBBBBBBBBBBK.", ".KBBBBBBBBBBBBK.", "..KBBBBBBBBBBK..",
        "..KBBBBBBBBBBK..", "...KKBBBBBBKK...", ".....KKKKKK.....", "................",
    });
    const PixelArt reduced = reduce(ring, 8, 8, ReduceMode::Outlines);
    EXPECT_EQ(strokeCount(reduced), 1) << ::testing::PrintToString(lettersOf(reduced, "WKB"));
    // One pixel thick: no 2 x 2 block of outline
    for (int y = 0; y + 1 < reduced.height; y++) {
        for (int x = 0; x + 1 < reduced.width; x++) {
            const int w = reduced.width;
            const bool block = reduced.indices[size_t(y) * w + x] == 1 && reduced.indices[size_t(y) * w + x + 1] == 1 &&
                               reduced.indices[size_t(y + 1) * w + x] == 1 && reduced.indices[size_t(y + 1) * w + x + 1] == 1;
            EXPECT_FALSE(block) << "at " << x << "," << y << " " << ::testing::PrintToString(lettersOf(reduced, "WKB"));
        }
    }
    // Closed: the transparent outside cannot reach the fill through 4-connected steps
    std::vector<uint8_t> outside(reduced.indices.size(), 0);
    std::vector<int> stack{0};
    outside[0] = 1;
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const int x = i % reduced.width;
        const int around[4] = {x > 0 ? i - 1 : -1, x + 1 < reduced.width ? i + 1 : -1, i - reduced.width, i + reduced.width};
        for (int n : around) {
            if (n < 0 || n >= int(reduced.indices.size()) || outside[size_t(n)] || reduced.indices[size_t(n)] != TRANSPARENT) continue;
            outside[size_t(n)] = 1;
            stack.push_back(n);
        }
    }
    for (size_t i = 0; i < reduced.indices.size(); i++) {
        EXPECT_FALSE(outside[i] && reduced.indices[i] == 2) << "the outside reached the fill at " << i;
    }
}

TEST(CleanupReduce, TurnsAOnePixelStepInAStrokeIntoADiagonal) {
    PixelArt picture = strokeArt({"WKWWW", "WKKKK", "WWWWW"});
    smoothStairs(picture.indices, picture.width, picture.height, outlineIsOne);
    EXPECT_EQ(lettersOf(picture, "WKB"), (Rows{"WKWWW", "WWKKK", "WWWWW"}));
}

TEST(CleanupReduce, KeepsARealCorner) {
    PixelArt picture = strokeArt({"KWWW", "KWWW", "KKKK"});
    const Rows before = lettersOf(picture, "WKB");
    smoothStairs(picture.indices, picture.width, picture.height, outlineIsOne);
    EXPECT_EQ(lettersOf(picture, "WKB"), before);
}

TEST(CleanupReduce, HalvesAPictureByTheMostCommonColourOfEachBlock) {
    EXPECT_EQ(lettersOf(reduce(reduceArt({"BBSS", "BWSS", "KKWW", "KKWP"}), 2, 2, ReduceMode::Common), reduceKeys), (Rows{"BS", "KW"}));
}

TEST(CleanupReduce, KeepsAOnePixelOutlineThatTheMostCommonColourWouldDrop) {
    const PixelArt picture = reduceArt({"KKKKKK", "WWWWWW", "WWWWWW", "WWWWWW", "WWWWWW", "WWWWWW"});
    EXPECT_EQ(lettersOf(reduce(picture, 3, 2, ReduceMode::Common), reduceKeys), (Rows{"WWW", "WWW"}));
    EXPECT_EQ(lettersOf(reduce(picture, 3, 2, ReduceMode::Outlines), reduceKeys), (Rows{"KKK", "WWW"}));
}

TEST(CleanupReduce, DoesNotGrowALoneDarkPixelIntoABlock) {
    EXPECT_EQ(lettersOf(reduce(reduceArt({"WWWW", "WKWW", "WWWW", "WWWW"}), 2, 2, ReduceMode::Outlines), reduceKeys), (Rows{"WW", "WW"}));
}

TEST(CleanupReduce, MergesBlocksOfAnyRatioTransparentOnesIncluded) {
    const PixelArt picture = reduceArt({"..KKKK", "..KBBK", "..KKKK"});
    const PixelArt reduced = reduce(picture, 4, 2, ReduceMode::Common);
    EXPECT_EQ(reduced.width, 4);
    EXPECT_EQ(reduced.height, 2);
    EXPECT_EQ(lettersOf(reduced, reduceKeys)[0][0], '.');
    EXPECT_EQ(reduce(picture, 6, 3, ReduceMode::Common), picture);
}
