// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S47 AC3 (R18): the scanline fill against a breadth-first reference, tolerance, global fills, and
// a fill decided on the composite.

#include "SpriteDrawTestSupport.h"

#include "pixelart/draw/DrawPrimitives.h"

#include <deque>
#include <random>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::draw;
using jasperengine::editor::pixelart::testing::json;

namespace {

    std::vector<uint8_t> referenceFill(const std::vector<uint8_t>& open, int w, int h, IntPoint seed, bool eight) {
        std::vector<uint8_t> mask(open.size(), 0);
        if (!open[static_cast<size_t>(seed.y) * w + seed.x]) return mask;
        std::deque<IntPoint> queue{seed};
        mask[static_cast<size_t>(seed.y) * w + seed.x] = 1;
        while (!queue.empty()) {
            const IntPoint p = queue.front();
            queue.pop_front();
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if ((dx == 0 && dy == 0) || (!eight && dx != 0 && dy != 0)) continue;
                    const int nx = p.x + dx, ny = p.y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const size_t i = static_cast<size_t>(ny) * w + nx;
                    if (!open[i] || mask[i]) continue;
                    mask[i] = 1;
                    queue.push_back({nx, ny});
                }
            }
        }
        return mask;
    }

}

TEST(FillOps, ScanlineEqualsReferenceOn200RandomMasks) {
    std::mt19937 random(47);
    const int w = 24, h = 18;
    for (int round = 0; round < 200; round++) {
        std::vector<uint8_t> open(static_cast<size_t>(w) * h);
        std::bernoulli_distribution bit(round % 5 == 0 ? 0.9 : 0.62);
        for (uint8_t& v : open) v = bit(random) ? 1 : 0;
        const IntPoint seed{std::uniform_int_distribution<int>(0, w - 1)(random), std::uniform_int_distribution<int>(0, h - 1)(random)};
        for (bool eight : {false, true}) {
            const std::vector<uint8_t> scan = floodFill(w, h, seed, eight, [&](int x, int y) { return open[static_cast<size_t>(y) * w + x] != 0; });
            ASSERT_EQ(scan, referenceFill(open, w, h, seed, eight)) << "round " << round << " eight " << eight;
        }
    }
}

TEST(FillOps, ToleranceIsPerChannelMaximum) {
    draw::SpriteDoc sprite = testing::makeSprite(3, 1, aseprite::ColorMode::Rgba);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", "#646464"}},
                                   {{"op", "pixel"}, {"points", {{1, 0}}}, {"color", "#6e6e6e"}},
                                   {{"op", "pixel"}, {"points", {{2, 0}}}, {"color", "#b4b4b4"}}}));
    // 0x6e-0x64 = 10: inside tolerance 10; 0xb4 is 70 away from 0x6e and 80 from the seed
    S47_APPLY(sprite, json::array({{{"op", "fill"}, {"at", {0, 0}}, {"color", "#ff0000"}, {"tolerance", 10}}}));
    const PixelImage canvas = draw::celCanvas(sprite.doc, 0, 0);
    EXPECT_EQ(canvas.at(0, 0), (PixelRgba{255, 0, 0, 255}));
    EXPECT_EQ(canvas.at(1, 0), (PixelRgba{255, 0, 0, 255}));
    EXPECT_EQ(canvas.at(2, 0), (PixelRgba{180, 180, 180, 255}));
}

TEST(FillOps, NotContiguousReplacesDisconnectedAreas) {
    draw::SpriteDoc sprite = testing::makeSprite(5, 1);
    S47_APPLY(sprite, json::array({{{"op", "paste"}, {"rows", {"12121"}}}}));
    S47_APPLY(sprite, json::array({{{"op", "fill"}, {"at", {0, 0}}, {"color", 3}, {"contiguous", false}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[0], "32323");
}

TEST(FillOps, CompositeSampleFillsWhatTheFrameShowsIntoTheTargetLayer) {
    draw::SpriteDoc sprite = testing::makeSprite(4, 1, aseprite::ColorMode::Indexed, 1, {"bottom", "top"});
    // a wall on the top layer splits the row as the frame shows it
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{2, 0}}}, {"color", 1}, {"layer", "top"}}}));
    S47_APPLY(sprite, json::array({{{"op", "fill"}, {"at", {0, 0}}, {"color", 2}, {"sample", "composite"}, {"layer", "bottom"}}}));
    EXPECT_EQ(testing::canvasRows(sprite, 0)[0], "22..") << "stops at the top layer's wall, paints only the bottom layer";
    EXPECT_EQ(testing::canvasRows(sprite, 1)[0], "..1.");
}

TEST(FillOps, IndexedToleranceZeroComparesIndices) {
    draw::SpriteDoc sprite = testing::makeSprite(2, 1);
    // two palette entries of one colour stay apart at tolerance 0
    sprite.doc.palette[2].color = sprite.doc.palette[1].color;
    S47_APPLY(sprite, json::array({{{"op", "paste"}, {"rows", {"12"}}}}));
    S47_APPLY(sprite, json::array({{{"op", "fill"}, {"at", {0, 0}}, {"color", 3}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[0], "32");
}

TEST(FillOps, FillStaysInsideTheSelection) {
    draw::SpriteDoc sprite = testing::makeSprite(6, 1);
    S47_APPLY(sprite, json::array({{{"op", "select"}, {"shape", "rect"}, {"rect", {0, 0, 3, 1}}},
                                   {{"op", "fill"}, {"at", {0, 0}}, {"color", 2}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[0], "222...");
}

TEST(FillOps, DitherUsesCanvasCoordinatesAndTheBayerMatrix) {
    const std::vector<int> m2 = bayerMatrix(2);
    EXPECT_EQ(m2, (std::vector<int>{0, 2, 3, 1}));
    draw::SpriteDoc sprite = testing::makeSprite(4, 2);
    S47_APPLY(sprite, json::array({{{"op", "dither"}, {"colors", {1, 2}}, {"pattern", "checker"}, {"level", 0.5}}}));
    const std::vector<std::string> rows = testing::canvasRows(sprite);
    EXPECT_EQ(rows[0], "2121");
    EXPECT_EQ(rows[1], "1212");
}

TEST(FillOps, DitherGradientRamps) {
    draw::SpriteDoc sprite = testing::makeSprite(16, 1);
    S47_APPLY(sprite, json::array({{{"op", "dither"}, {"colors", {1, 2}}, {"pattern", "bayer4"}, {"gradient", {{"from", {0, 0}}, {"to", {15, 0}}}}}}));
    const std::string row = testing::canvasRows(sprite)[0];
    EXPECT_EQ(row[0], '1');
    EXPECT_EQ(row[15], '2');
    EXPECT_GT(std::count(row.begin(), row.end(), '2'), 3);
    EXPECT_GT(std::count(row.begin(), row.end(), '1'), 3);
}

TEST(FillOps, ClearAndReplace) {
    draw::SpriteDoc sprite = testing::makeSprite(4, 1);
    S47_APPLY(sprite, json::array({{{"op", "paste"}, {"rows", {"1212"}}},
                                   {{"op", "replace"}, {"from", 1}, {"to", 3}},
                                   {{"op", "clear"}, {"rect", {3, 0, 1, 1}}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[0], "323.");
}
