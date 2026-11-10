// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/anim/AnimSvgPath.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;

TEST(AnimeSvgPath, LineLengthAndClampedPoints){
    const auto line = AnimSvgPath::fromAttributes("line", {{"x1", "10"}, {"y1", "20"}, {"x2", "40"}, {"y2", "60"}});
    ASSERT_TRUE(line.isValid());
    EXPECT_DOUBLE_EQ(line.length(), 50);
    EXPECT_DOUBLE_EQ(line.pointAtLength(25).x, 25);
    EXPECT_DOUBLE_EQ(line.pointAtLength(25).y, 40);
    EXPECT_DOUBLE_EQ(line.pointAtLength(-1).x, 10);
    EXPECT_DOUBLE_EQ(line.pointAtLength(51).y, 60);
}

TEST(AnimeSvgPath, PolylineAndPolygonIncludeTheirOwnSegments){
    const auto open = AnimSvgPath::fromAttributes("polyline", {{"points", "0,0 30,0 30,40"}});
    const auto closed = AnimSvgPath::fromAttributes("polygon", {{"points", "0,0 30,0 30,40"}});
    EXPECT_DOUBLE_EQ(open.length(), 70);
    EXPECT_DOUBLE_EQ(closed.length(), 120);
    EXPECT_DOUBLE_EQ(open.pointAtLength(50).x, 30);
    EXPECT_DOUBLE_EQ(open.pointAtLength(50).y, 20);
    EXPECT_NEAR(closed.pointAtLength(95).x, 15, 1e-12);
    EXPECT_NEAR(closed.pointAtLength(95).y, 20, 1e-12);
}

TEST(AnimeSvgPath, CircleUsesAnalyticArclength){
    const double pi = std::acos(-1.0);
    const auto circle = AnimSvgPath::fromAttributes("circle", {{"cx", "20"}, {"cy", "30"}, {"r", "10"}});
    EXPECT_NEAR(circle.length(), 20 * pi, 1e-12);
    EXPECT_NEAR(circle.pointAtLength(5 * pi).x, 20, 1e-10);
    EXPECT_NEAR(circle.pointAtLength(5 * pi).y, 40, 1e-10);
    EXPECT_NEAR(circle.pointAtLength(10 * pi).x, 10, 1e-10);
    EXPECT_NEAR(circle.pointAtLength(10 * pi).y, 30, 1e-10);
}

TEST(AnimeSvgPath, CubicArclengthAndSymmetry){
    // This cubic has an analytic total length of 200. Browser getPointAtLength gives (50,75)
    // halfway along it, not halfway along its flattened control polygon.
    const auto curve = AnimSvgPath::fromAttributes("path", {{"d", "M0 0 C0 100 100 100 100 0"}});
    ASSERT_TRUE(curve.isValid());
    EXPECT_NEAR(curve.length(), 200, 0.002);
    const auto half = curve.pointAtLength(curve.length() / 2);
    EXPECT_NEAR(half.x, 50, 0.001);
    EXPECT_NEAR(half.y, 75, 0.001);
}

TEST(AnimeSvgPath, SvgCommandsAndDisjointSubpaths){
    const auto line = AnimSvgPath::fromAttributes("path", {{"d", "m10 20 h30 v40 z M100 100 l0 10"}});
    EXPECT_NEAR(line.length(), 130, 1e-9);
    EXPECT_NEAR(line.pointAtLength(125).x, 100, 1e-9);
    EXPECT_NEAR(line.pointAtLength(125).y, 105, 1e-9);
    const auto quadratic = AnimSvgPath::fromAttributes("path", {{"d", "M0 0 Q50 100 100 0"}});
    EXPECT_NEAR(quadratic.pointAtLength(quadratic.length() / 2).x, 50, 0.001);
    EXPECT_NEAR(quadratic.pointAtLength(quadratic.length() / 2).y, 50, 0.001);
    const auto arc = AnimSvgPath::fromAttributes("path", {{"d", "M10 0 A10 10 0 0 1 -10 0"}});
    EXPECT_NEAR(arc.length(), 10 * std::acos(-1.0), 0.01);
    EXPECT_NEAR(arc.pointAtLength(arc.length() / 2).x, 0, 0.001);
    EXPECT_NEAR(arc.pointAtLength(arc.length() / 2).y, 10, 0.001);
}

TEST(AnimeSvgPath, DegenerateAndInvalidGeometry){
    EXPECT_FALSE(AnimSvgPath::fromAttributes("div", {}).isValid());
    EXPECT_FALSE(AnimSvgPath::fromAttributes("polyline", {{"points", "0 1 2"}}).isValid());
    const auto point = AnimSvgPath::fromAttributes("path", {{"d", "M12 34"}});
    EXPECT_DOUBLE_EQ(point.length(), 0);
    EXPECT_DOUBLE_EQ(point.pointAtLength(0).x, 12);
    EXPECT_DOUBLE_EQ(point.pointAtLength(0).y, 34);
}
