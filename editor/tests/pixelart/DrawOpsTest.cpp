// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S47 AC2 (R16): lines, ellipses, rounded rects and polygons, against masks a Python port of
// Pixelorama's GDScript printed.

#include "SpriteDrawTestSupport.h"

#include "pixelart/draw/DrawPrimitives.h"
#include "pixelart/lint/PixelLint.h"

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::draw;
using jasperengine::editor::pixelart::s47test::json;

namespace {

    std::vector<std::string> paint(const std::vector<IntPoint>& points, int w, int h) {
        std::vector<std::string> rows(static_cast<size_t>(h), std::string(static_cast<size_t>(w), '.'));
        for (const IntPoint& p : points) {
            if (p.x >= 0 && p.y >= 0 && p.x < w && p.y < h) rows[static_cast<size_t>(p.y)][static_cast<size_t>(p.x)] = '#';
        }
        return rows;
    }

    // a staircase with many corners: every step an L
    json staircase() {
        json points = json::array();
        for (int i = 0; i < 8; i++) {
            points.push_back({2 + i * 2, 2 + i * 2});
            points.push_back({2 + (i + 1) * 2, 2 + i * 2});
        }
        return points;
    }

    lint::LintStatus doublesStatus(const draw::SpriteDoc& sprite) {
        const PixelImage canvas = draw::celCanvas(sprite.doc, 0, 0);
        const std::vector<PixelImage> frames = {canvas};
        lint::LintInput input;
        input.frames = frames;
        lint::LintOptions options;
        options.only = {"doubles"};
        const lint::LintReport report = lint::lint(input, options);
        EXPECT_EQ(report.results.size(), 1u);
        return report.results.empty() ? lint::LintStatus::Skip : report.results[0].status;
    }

}

TEST(DrawOps, BresenhamDrawsBothEnds) {
    std::vector<IntPoint> line;
    bresenham({0, 0}, {3, 1}, line);
    ASSERT_GE(line.size(), 2u);
    EXPECT_EQ(line.front(), (IntPoint{0, 0}));
    EXPECT_EQ(line.back(), (IntPoint{3, 1}));
    EXPECT_EQ(line.size(), 4u);
}

TEST(DrawOps, PolylineDrawsEachJointOnce) {
    const std::vector<IntPoint> points = {{0, 0}, {2, 0}, {2, 2}};
    const std::vector<IntPoint> stroke = polyline(points, false);
    EXPECT_EQ(stroke.size(), 5u);
}

TEST(DrawOps, PixelPerfectRemovesEveryCorner) {
    draw::SpriteDoc plain = s47test::makeSprite(32, 32), perfect = plain;
    S47_APPLY(plain, json::array({{{"op", "line"}, {"points", staircase()}, {"color", 1}, {"pixel_perfect", false}}}));
    S47_APPLY(perfect, json::array({{{"op", "line"}, {"points", staircase()}, {"color", 1}}}));
    EXPECT_EQ(doublesStatus(plain), lint::LintStatus::Warn) << "without pixel_perfect the joints have L-corners";
    EXPECT_EQ(doublesStatus(perfect), lint::LintStatus::Pass);
}

TEST(DrawOps, LShapedLineLosesItsCornerPixel) {
    draw::SpriteDoc sprite = s47test::makeSprite(4, 4);
    S47_APPLY(sprite, json::array({{{"op", "line"}, {"points", {{0, 0}, {3, 0}, {3, 3}}}, {"color", 1}}}));
    const std::vector<std::string> rows = s47test::canvasRows(sprite);
    EXPECT_EQ(rows[0], "111.") << "the corner (3,0) goes";
    EXPECT_EQ(rows[1], "...1");
    EXPECT_EQ(rows[3], "...1");
}

TEST(DrawOps, ThickLinesStampASquareBrush) {
    draw::SpriteDoc sprite = s47test::makeSprite(8, 8);
    S47_APPLY(sprite, json::array({{{"op", "line"}, {"points", {{1, 3}, {5, 3}}}, {"color", 2}, {"thickness", 3}}}));
    const std::vector<std::string> rows = s47test::canvasRows(sprite);
    EXPECT_EQ(rows[2], "2222222.");
    EXPECT_EQ(rows[3], "2222222.");
    EXPECT_EQ(rows[4], "2222222.");
    EXPECT_EQ(rows[1], "........");
    EXPECT_EQ(rows[5], "........");
}

TEST(DrawOps, ZinglEllipsesMatchTheGoldenMasks) {
    struct Case {
        int w, h;
        std::vector<std::string> mask;
    };
    const std::vector<Case> cases = {
        {1, 1, {"#"}},
        {2, 2, {"##", "##"}},
        {3, 3, {".#.", "#.#", ".#."}},
        {4, 3, {".##.", "#..#", ".##."}},
        {5, 5, {".###.", "#...#", "#...#", "#...#", ".###."}},
        {6, 4, {".####.", "#....#", "#....#", ".####."}},
        {7, 7, {"..###..", ".#...#.", "#.....#", "#.....#", "#.....#", ".#...#.", "..###.."}},
        {8, 5, {"..####..", ".#....#.", "#......#", ".#....#.", "..####.."}},
        {9, 7, {"..#####..", ".#.....#.", "#.......#", "#.......#", "#.......#", ".#.....#.", "..#####.."}},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(paint(ellipseBorder({0, 0, c.w, c.h}), c.w, c.h), c.mask) << c.w << "x" << c.h;
    }
}

TEST(DrawOps, FilledEllipseFillsSpans) {
    const std::vector<std::string> rows = paint(ellipseFilled({0, 0, 7, 7}), 7, 7);
    EXPECT_EQ(rows[0], "..###..");
    EXPECT_EQ(rows[1], ".#####.");
    EXPECT_EQ(rows[3], "#######");
}

TEST(DrawOps, RoundedRectsMatchDrawingAlgos) {
    const std::vector<std::string> filled = {"..####..", ".######.", "########", "########", ".######.", "..####.."};
    EXPECT_EQ(paint(roundedRectFilled({0, 0, 8, 6}, 2), 8, 6), filled);
    const std::vector<std::string> outline = {"..####..", ".#....#.", "#......#", "#......#", ".#....#.", "..####.."};
    EXPECT_EQ(paint(roundedRectOutline({0, 0, 8, 6}, 2, 1), 8, 6), outline);
}

TEST(DrawOps, PolygonFillIsEvenOddOnAStar) {
    const std::vector<IntPoint> star = {{15, 0}, {6, 29}, {29, 11}, {1, 11}, {24, 29}};
    const std::vector<std::string> rows = paint(polygonFilled(star, {0, 0, 31, 31}), 31, 31);
    EXPECT_EQ(rows[17][15], '.') << "the centre is covered twice: even-odd leaves it empty";
    EXPECT_EQ(rows[8][15], '#') << "a tip is covered once";
    EXPECT_EQ(rows[20][8], '#');
}

TEST(DrawOps, OpOutsideTheCanvasIsClippedAndReportsNothingDrawn) {
    draw::SpriteDoc sprite = s47test::makeSprite(4, 4);
    s47test::Run run = s47test::apply(sprite, json::array({{{"op", "pixel"}, {"points", {{9, 9}}}, {"color", 1}}}));
    ASSERT_TRUE(run.ok);
    EXPECT_FALSE(run.result.changedAnything());
    ASSERT_EQ(run.result.warnings.size(), 1u);
    EXPECT_EQ(run.result.warnings[0].kind, "nothing_drawn");
    EXPECT_EQ(run.result.warnings[0].opIndex, 0);
}

TEST(DrawOps, ClosedFieldTablesRefuseAMisplacedField) {
    draw::SpriteDoc sprite = s47test::makeSprite(4, 4);
    s47test::Run run = s47test::apply(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", 1}}, {{"op", "line"}, {"points", {{0, 0}, {1, 0}}}, {"rows", {"1"}}, {"color", 1}}}));
    ASSERT_FALSE(run.ok);
    EXPECT_EQ(run.problem.kind, "bad_op_field");
    EXPECT_EQ(run.opIndex, 1);
    EXPECT_NE(run.problem.fix.find("pixel_perfect"), std::string::npos) << run.problem.fix;
}

TEST(DrawOps, HexColorNotInThePaletteNamesTheNearestAndThePaletteOp) {
    draw::SpriteDoc sprite = s47test::makeSprite(4, 4);
    s47test::Run run = s47test::apply(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}}}, {"color", "#fe0101"}}}));
    ASSERT_FALSE(run.ok);
    EXPECT_EQ(run.problem.kind, "color_not_in_palette");
    EXPECT_NE(run.problem.fix.find("index 1"), std::string::npos) << run.problem.fix;
    EXPECT_NE(run.problem.fix.find("\"palette\""), std::string::npos) << run.problem.fix;
}
