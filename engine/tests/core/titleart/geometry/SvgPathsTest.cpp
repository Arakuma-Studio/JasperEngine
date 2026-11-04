// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S80 R25 (AC22): the SVG geometry of Vector layers in shape mode (tinyxml2): the path-data
// grammar, basic shapes, transforms, use, styles and class rules, and what is ignored.

#include "gtest/gtest.h"

#include "titleart/geometry/PathUtil.h"
#include "titleart/geometry/SvgPaths.h"

#include <cmath>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;

namespace {

    SvgGeometry svg(const std::string& body, const std::string& head = "") {
        return parseSvgGeometry("<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' viewBox='0 0 200 100'>" + head + body + "</svg>");
    }

    void expectPoint(const TitleVec2& p, double x, double y) {
        EXPECT_NEAR(p.x, x, 1e-9);
        EXPECT_NEAR(p.y, y, 1e-9);
    }

}

TEST(SvgPathsTest, PathDataGrammar) {
    Path p;
    // absolute and relative commands, implicit line-tos after a move, H / V, compact numbers
    ASSERT_TRUE(parseSvgPathData("M10,20 30 40l5-5h10v-10H0V0z m1.5.5 1e1 0", p));
    const std::vector<TitleVec2>& pts = p.pointList();
    ASSERT_GE(pts.size(), 9u);
    expectPoint(pts[0], 10, 20);
    expectPoint(pts[1], 30, 40);
    expectPoint(pts[2], 35, 35);
    expectPoint(pts[3], 45, 35);
    expectPoint(pts[4], 45, 25);
    expectPoint(pts[5], 0, 25);
    expectPoint(pts[6], 0, 0);
    // after z the current point is the subpath's start: m1.5.5 is relative to (10, 20)
    expectPoint(pts[7], 11.5, 20.5);
    expectPoint(pts[8], 21.5, 20.5);
    EXPECT_EQ(p.contourCount(), 2u);
    // smooth curves reflect the previous control point
    Path c;
    ASSERT_TRUE(parseSvgPathData("M0 0 C10 0 20 10 20 20 S30 40 40 40 Q50 40 50 50 T50 70", c));
    const std::vector<TitleVec2>& cp = c.pointList();
    expectPoint(cp[4], 20, 30);         // S: the reflection of (20, 10) about (20, 20)
    expectPoint(cp[9], 50, 60);         // T: the reflection of (50, 40) about (50, 50)
    // a syntax error keeps what parsed before it
    Path broken;
    EXPECT_FALSE(parseSvgPathData("M0 0 L10 10 X 5", broken));
    EXPECT_EQ(broken.pointList().size(), 2u);
}

TEST(SvgPathsTest, ArcsBecomeCubics) {
    Path p;
    // a half circle of radius 50 from (0, 50) to (100, 50) through (50, 0), the sweep flag on
    ASSERT_TRUE(parseSvgPathData("M0 50 A50 50 0 0 1 100 50", p));
    for (size_t i = 1; i < p.verbList().size(); i++) EXPECT_EQ(p.verbList()[i], PathVerb::Cubic);
    EXPECT_EQ(p.verbList().size(), 3u) << "two quarter circles";
    const TitleRect b = p.bounds();
    EXPECT_NEAR(b.y, 0, 0.05);
    EXPECT_NEAR(b.x + b.width, 100, 1e-9);
    // every point of the arc is 50 from the centre
    for (const std::vector<TitleVec2>& contour : p.flatten(0.01)) {
        for (const TitleVec2& q : contour) EXPECT_NEAR(std::hypot(q.x - 50, q.y - 50), 50, 0.03);
    }
    // radii too small scale up; flags packed against the next number still parse
    Path tight;
    ASSERT_TRUE(parseSvgPathData("M0 0a1 1 0 1010 0", tight));
    EXPECT_NEAR(tight.pointList().back().x, 10, 1e-9);
}

TEST(SvgPathsTest, ShapesTransformsAndUse) {
    const SvgGeometry g = svg(
        "<defs><rect id='r' width='10' height='10'/><symbol id='s'><circle cx='5' cy='5' r='5'/></symbol></defs>"
        "<g transform='translate(100 0) scale(2)'><rect x='1' y='2' width='3' height='4'/></g>"
        "<use href='#r' x='50' y='50'/><use xlink:href='#s' transform='rotate(90)'/>"
        "<ellipse cx='20' cy='20' rx='10' ry='5'/><polygon points='0,0 10,0 10,10'/><line x1='0' y1='0' x2='5' y2='5' stroke='black'/>");
    ASSERT_TRUE(g.ok);
    EXPECT_DOUBLE_EQ(g.width, 200);
    EXPECT_DOUBLE_EQ(g.height, 100);
    ASSERT_EQ(g.fills.size(), 5u) << "rect, used rect, used circle, ellipse, polygon (a line has no fill)";
    const TitleRect a = g.fills[0].controlBounds();
    EXPECT_DOUBLE_EQ(a.x, 102);
    EXPECT_DOUBLE_EQ(a.y, 4);
    EXPECT_DOUBLE_EQ(a.width, 6);
    const TitleRect used = g.fills[1].controlBounds();
    EXPECT_DOUBLE_EQ(used.x, 50);
    EXPECT_DOUBLE_EQ(used.y, 50);
    // rotate(90) turns (x, y) into (-y, x)
    const TitleRect turned = g.fills[2].controlBounds();
    EXPECT_NEAR(turned.x, -10, 1e-9);
    EXPECT_NEAR(turned.y, 0, 1e-9);
    ASSERT_EQ(g.strokes.size(), 1u);
    EXPECT_FALSE(g.strokes[0].path.verbList().back() == PathVerb::Close) << "a line's stroke stays open";
    EXPECT_TRUE(g.ignored.empty());
}

TEST(SvgPathsTest, StylesClassRulesAndInheritance) {
    const SvgGeometry g = svg(
        "<g fill='none' stroke='#000' stroke-width='4' stroke-linejoin='round'>"
        "  <path class='solid' d='M0 0 H10 V10 Z'/>"
        "  <path style='stroke-width:2; stroke-linecap: square' d='M20 0 H30'/>"
        "  <path d='M0 0 H10' display='none'/>"
        "  <g visibility='hidden'><path d='M0 0 H10 V10 Z' fill='red'/></g>"
        "  <path fill-rule='evenodd' fill='red' d='M0 0 H50 V50 H0 Z M10 10 H40 V40 H10 Z' transform='scale(3)'/>"
        "</g>",
        "<style>/* a rule */ .solid { fill: #fff; stroke: none } rect.x, #y { fill: red }</style>");
    ASSERT_TRUE(g.ok);
    ASSERT_EQ(g.fills.size(), 2u);
    EXPECT_FALSE(g.fills[0].evenOdd);
    EXPECT_TRUE(g.fills[1].evenOdd);
    ASSERT_EQ(g.strokes.size(), 2u) << "the class rule removed the solid path's stroke";
    EXPECT_DOUBLE_EQ(g.strokes[0].width, 2);
    EXPECT_EQ(g.strokes[0].join, OffsetJoin::Round) << "inherited";
    EXPECT_EQ(g.strokes[0].cap, OffsetCap::Square);
    EXPECT_DOUBLE_EQ(g.strokes[1].width, 12) << "stroke widths scale with the transform";
    // what this reader cannot draw is named
    EXPECT_EQ(g.ignored, (std::vector<std::string>{"css selector '#y'", "css selector 'rect.x'"}));
}

TEST(SvgPathsTest, UnsupportedFeaturesAreReported) {
    const SvgGeometry g = svg(
        "<text x='0' y='0'>Hi</text><image href='a.png' width='5' height='5'/>"
        "<rect width='10' height='10' filter='url(#f)' clip-path='url(#c)'/><rect width='50%' height='10'/>"
        "<use href='other.svg#a'/>");
    ASSERT_TRUE(g.ok);
    EXPECT_EQ(g.ignored, (std::vector<std::string>{"clip-path", "filter", "image", "relative length (width)", "text",
                                                   "use (external reference)"}));
    EXPECT_EQ(g.fills.size(), 1u) << "the rect with a filter still gives its outline";
}

TEST(SvgPathsTest, NotAnSvg) {
    EXPECT_FALSE(parseSvgGeometry("<html></html>").ok);
    EXPECT_FALSE(parseSvgGeometry("not xml <").ok);
    EXPECT_FALSE(parseSvgGeometry("<html></html>").error.empty());
}
