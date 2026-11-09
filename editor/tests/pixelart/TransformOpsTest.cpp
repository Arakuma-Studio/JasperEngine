// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S47 AC5 (R21, R22): the transform ops on a sprite: exact quarter turns, no new colour at any
// angle, Scale2x through the op, shift with wrap, flip and mirror. The rotation algorithms' own
// tests and goldens are SpriteRotateTest.

#include "SpriteDrawTestSupport.h"

#include <set>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::draw;
using jasperengine::editor::pixelart::testing::json;

namespace {

    void paintArrow(draw::SpriteDoc& sprite) {
        S47_APPLY(sprite, json::array({{{"op", "line"}, {"points", {{1, 1}, {6, 1}}}, {"color", 1}, {"pixel_perfect", false}},
                                       {{"op", "line"}, {"points", {{1, 1}, {1, 5}}}, {"color", 2}, {"pixel_perfect", false}},
                                       {{"op", "pixel"}, {"points", {{5, 4}}}, {"color", 3}}}));
    }

}

TEST(TransformOps, QuarterTurnsAreExactPermutations) {
    for (const char* method : {"auto", "cleanedge", "rotxel", "omniscale", "nearest"}) {
        draw::SpriteDoc sprite = testing::makeSprite(8, 8);
        paintArrow(sprite);
        const std::vector<std::string> before = testing::canvasRows(sprite);
        for (int turn = 0; turn < 4; turn++) S47_APPLY(sprite, json::array({{{"op", "rotate"}, {"angle", 90}, {"method", method}}}));
        EXPECT_EQ(testing::canvasRows(sprite), before) << method;
        S47_APPLY(sprite, json::array({{{"op", "rotate"}, {"angle", 180}, {"method", method}}}));
        S47_APPLY(sprite, json::array({{{"op", "rotate"}, {"angle", -180}, {"method", method}}}));
        EXPECT_EQ(testing::canvasRows(sprite), before) << method;
    }
}

TEST(TransformOps, Rotate90MovesAPixelClockwise) {
    draw::SpriteDoc sprite = testing::makeSprite(4, 4);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 1}}, {{"op", "rotate"}, {"angle", 90}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[0], "...1") << "the top-left corner goes to the top right";
}

TEST(TransformOps, RotationsAddNoNewColor) {
    for (const char* method : {"cleanedge", "rotxel", "omniscale", "nearest"}) {
        for (double angle : {30.0, 45.0}) {
            draw::SpriteDoc sprite = testing::makeSprite(16, 16);
            paintArrow(sprite);
            testing::Run run = testing::apply(sprite, json::array({{{"op", "rotate"}, {"angle", angle}, {"method", method}}}));
            ASSERT_TRUE(run.ok) << run.problem.message;
            const PixelImage canvas = draw::celCanvas(sprite.doc, 0, 0);
            ASSERT_TRUE(canvas.hasIndices());
            for (uint8_t i : canvas.indices()) EXPECT_LE(i, 3) << method << " " << angle << " stays in the palette";
        }
    }
}

TEST(TransformOps, RotateAboutACustomPivot) {
    draw::SpriteDoc sprite = testing::makeSprite(8, 8);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{3, 3}}}, {"color", 1}}, {{"op", "rotate"}, {"angle", 90}, {"pivot", {3, 3}}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[3][3], '1') << "the pivot pixel stays";
}

TEST(TransformOps, ScaleByTwoIsCentredAndClipped) {
    draw::SpriteDoc sprite = testing::makeSprite(4, 4);
    S47_APPLY(sprite, json::array({{{"op", "paste"}, {"rows", {"12", "34"}}, {"at", {1, 1}}},
                                   {{"op", "scale"}, {"factor", 2}, {"method", "nearest"}, {"rect", {1, 1, 2, 2}}}}));
    const std::vector<std::string> rows = testing::canvasRows(sprite);
    EXPECT_EQ(rows[0], "1122");
    EXPECT_EQ(rows[1], "1122");
    EXPECT_EQ(rows[2], "3344");
    EXPECT_EQ(rows[3], "3344");
}

TEST(TransformOps, Scale2xRoundsADiagonal) {
    draw::SpriteDoc sprite = testing::makeSprite(4, 4);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{1, 1}, {2, 2}}}, {"color", 1}},
                                   {{"op", "scale"}, {"factor", 2}, {"rect", {1, 1, 2, 2}}}}));
    const std::vector<std::string> rows = testing::canvasRows(sprite);
    // AdvMAME2x fills the two inner corners of a diagonal step
    EXPECT_EQ(rows[1][2], '1');
    EXPECT_EQ(rows[2][1], '1');
}

TEST(TransformOps, MethodMustMatchFactor) {
    draw::SpriteDoc sprite = testing::makeSprite(4, 4);
    testing::Run run = testing::apply(sprite, json::array({{{"op", "scale"}, {"factor", 2}, {"method", "scale3x"}}}));
    ASSERT_FALSE(run.ok);
    EXPECT_EQ(run.problem.kind, "bad_op_field");
}

TEST(TransformOps, ShiftWrapsAndClips) {
    draw::SpriteDoc sprite = testing::makeSprite(3, 1);
    S47_APPLY(sprite, json::array({{{"op", "paste"}, {"rows", {"12."}}}, {{"op", "shift"}, {"by", {2, 0}}, {"wrap", true}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[0], "2.1");
    S47_APPLY(sprite, json::array({{{"op", "shift"}, {"by", {1, 0}}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[0], ".2.") << "1 moved off the canvas";
}

TEST(TransformOps, FlipAndMirror) {
    draw::SpriteDoc sprite = testing::makeSprite(4, 1);
    S47_APPLY(sprite, json::array({{{"op", "paste"}, {"rows", {"12.."}}}, {{"op", "flip"}, {"axis", "x"}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[0], "..21");
    S47_APPLY(sprite, json::array({{{"op", "mirror"}, {"axis", "x"}, {"from_side", "right"}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[0], "1221");
}

TEST(TransformOps, SelectionLimitsWhatMoves) {
    draw::SpriteDoc sprite = testing::makeSprite(4, 2);
    S47_APPLY(sprite, json::array({{{"op", "paste"}, {"rows", {"12..", "34.."}}},
                                   {{"op", "select"}, {"shape", "rect"}, {"rect", {0, 0, 2, 1}}},
                                   {{"op", "shift"}, {"by", {2, 0}}}}));
    const std::vector<std::string> rows = testing::canvasRows(sprite);
    EXPECT_EQ(rows[0], "..12");
    EXPECT_EQ(rows[1], "34..") << "unselected pixels stay";
}
