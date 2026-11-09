// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S47 AC6 (R14, R15, R23): the symmetry axes, a mirrored pixel-perfect stroke, selections and copy.

#include "SpriteDrawTestSupport.h"

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::draw;
using jasperengine::editor::pixelart::testing::json;

TEST(Symmetry, XMirrorsAboutTheCentreOnEvenAndOddSizes) {
    draw::SpriteDoc even = testing::makeSprite(6, 1), odd = testing::makeSprite(5, 1);
    S47_APPLY(even, json::array({{{"op", "pixel"}, {"points", {{1, 0}}}, {"color", 1}}}), json{{"x", true}});
    EXPECT_EQ(testing::canvasRows(even)[0], ".1..1.");
    S47_APPLY(odd, json::array({{{"op", "pixel"}, {"points", {{1, 0}}}, {"color", 1}}}), json{{"x", true}});
    EXPECT_EQ(testing::canvasRows(odd)[0], ".1.1.");
}

TEST(Symmetry, CustomAxisAndY) {
    draw::SpriteDoc sprite = testing::makeSprite(32, 4);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{10, 0}}}, {"color", 1}}}), json{{"x", 15.5}, {"y", true}});
    const std::vector<std::string> rows = testing::canvasRows(sprite);
    EXPECT_EQ(rows[0][10], '1');
    EXPECT_EQ(rows[0][21], '1') << "31 - 10";
    EXPECT_EQ(rows[3][10], '1');
    EXPECT_EQ(rows[3][21], '1');
}

TEST(Symmetry, DiagonalsReflectAboutPixelCentres) {
    draw::SpriteDoc sprite = testing::makeSprite(4, 4);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 1}}}, {"color", 1}}}), json{{"diagonal", "x_minus_y"}});
    std::vector<std::string> rows = testing::canvasRows(sprite);
    EXPECT_EQ(rows[1][0], '1');
    EXPECT_EQ(rows[0][1], '1') << "(x, y) to (y, x)";

    draw::SpriteDoc anti = testing::makeSprite(4, 4);
    S47_APPLY(anti, json::array({{{"op", "pixel"}, {"points", {{0, 1}}}, {"color", 1}}}), json{{"diagonal", "xy"}});
    rows = testing::canvasRows(anti);
    EXPECT_EQ(rows[1][0], '1');
    EXPECT_EQ(rows[3][2], '1') << "reflected across the anti-diagonal";
}

TEST(Symmetry, MirroredStrokeHasNoCornersOnEitherCopy) {
    draw::SpriteDoc sprite = testing::makeSprite(16, 8);
    S47_APPLY(sprite, json::array({{{"op", "line"}, {"points", {{1, 1}, {4, 1}, {4, 4}}}, {"color", 1}}}), json{{"x", true}});
    const std::vector<std::string> rows = testing::canvasRows(sprite);
    EXPECT_EQ(rows[1].substr(0, 5), ".111.");
    EXPECT_EQ(rows[2].substr(0, 5), "....1");
    EXPECT_EQ(rows[1].substr(11), ".111.");
    EXPECT_EQ(rows[2].substr(11, 1), "1");
}

TEST(Symmetry, FillSeedsMirror) {
    draw::SpriteDoc sprite = testing::makeSprite(5, 1);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{2, 0}}}, {"color", 1}}}));
    S47_APPLY(sprite, json::array({{{"op", "fill"}, {"at", {0, 0}}, {"color", 2}}}), json{{"x", true}});
    EXPECT_EQ(testing::canvasRows(sprite)[0], "22122");
}

TEST(Selection, ModesAddSubtractIntersect) {
    draw::SpriteDoc sprite = testing::makeSprite(4, 1);
    S47_APPLY(sprite, json::array({{{"op", "select"}, {"shape", "rect"}, {"rect", {0, 0, 2, 1}}},
                                   {{"op", "select"}, {"shape", "rect"}, {"rect", {3, 0, 1, 1}}, {"select_mode", "add"}},
                                   {{"op", "select"}, {"shape", "rect"}, {"rect", {1, 0, 1, 1}}, {"select_mode", "subtract"}},
                                   {{"op", "rect"}, {"rect", {0, 0, 4, 1}}, {"color", 1}, {"fill", true}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[0], "1..1");

    draw::SpriteDoc inter = testing::makeSprite(4, 1);
    S47_APPLY(inter, json::array({{{"op", "select"}, {"shape", "rect"}, {"rect", {0, 0, 3, 1}}},
                                  {{"op", "select"}, {"shape", "rect"}, {"rect", {2, 0, 2, 1}}, {"select_mode", "intersect"}},
                                  {{"op", "rect"}, {"rect", {0, 0, 4, 1}}, {"color", 1}, {"fill", true}}}));
    EXPECT_EQ(testing::canvasRows(inter)[0], "..1.");
}

TEST(Selection, IgnoreSelectionAndNoPersistenceAcrossCalls) {
    draw::SpriteDoc sprite = testing::makeSprite(3, 1);
    S47_APPLY(sprite, json::array({{{"op", "select"}, {"shape", "rect"}, {"rect", {0, 0, 1, 1}}},
                                   {{"op", "pixel"}, {"points", {{1, 0}}}, {"color", 1}, {"ignore_selection", true}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[0], ".1.");
    // a new batch has no selection left
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{2, 0}}}, {"color", 2}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[0], ".12");
}

TEST(Selection, ColorSelectIsAMagicWand) {
    draw::SpriteDoc sprite = testing::makeSprite(5, 1);
    S47_APPLY(sprite, json::array({{{"op", "paste"}, {"rows", {"11211"}}},
                                   {{"op", "select"}, {"shape", "color"}, {"at", {0, 0}}},
                                   {{"op", "clear"}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[0], "..211") << "contiguous stops at the 2";
}

TEST(Copy, BetweenFramesAndLayersWithCut) {
    draw::SpriteDoc sprite = testing::makeSprite(4, 1, aseprite::ColorMode::Indexed, 3, {"A", "B"});
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 1}, {"layer", "A"}, {"frame", 1}}}));
    S47_APPLY(sprite, json::array({{{"op", "copy"}, {"source", {{"frame", 1}, {"layer", "A"}, {"rect", {0, 0, 2, 1}}}}, {"at", {2, 0}}, {"to_frame", 3}, {"to_layer", "B"}, {"cut", true}}}));
    EXPECT_EQ(testing::canvasRows(sprite, 0, 0)[0], "....") << "cut clears the source";
    EXPECT_EQ(testing::canvasRows(sprite, 1, 2)[0], "..1.") << "frame 3 layer B";
}

TEST(Copy, TakesTheSelectionWhenNoRectIsGiven) {
    draw::SpriteDoc sprite = testing::makeSprite(4, 1);
    S47_APPLY(sprite, json::array({{{"op", "paste"}, {"rows", {"12.."}}},
                                   {{"op", "select"}, {"shape", "rect"}, {"rect", {1, 0, 1, 1}}},
                                   {{"op", "copy"}, {"at", {3, 0}}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[0], "12.2") << "only the selected pixel, placed outside the selection";
}
