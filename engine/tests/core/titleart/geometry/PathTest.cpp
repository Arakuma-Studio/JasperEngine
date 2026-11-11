// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "titleart/geometry/Path.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;

namespace {

    class PathTest : public ::testing::Test {};

    Path square(double x, double y, double side) {
        Path p;
        p.moveTo(x, y);
        p.lineTo(x + side, y);
        p.lineTo(x + side, y + side);
        p.lineTo(x, y + side);
        p.close();
        return p;
    }

}

TEST_F(PathTest, BoundsFollowTheCurveNotItsControlPoints) {
    Path p;
    p.moveTo(0, 0);
    p.cubicTo(0, 10, 10, 10, 10, 0);
    const TitleRect control = p.controlBounds();
    const TitleRect tight = p.bounds();
    EXPECT_DOUBLE_EQ(control.height, 10);
    EXPECT_NEAR(tight.height, 7.5, 1e-9) << "a symmetric cubic peaks at 3/4 of its control height";
    EXPECT_DOUBLE_EQ(tight.width, 10);
    Path q;
    q.moveTo(0, 0);
    q.quadTo(5, 10, 10, 0);
    EXPECT_NEAR(q.bounds().height, 5, 1e-9);
    EXPECT_TRUE(Path().bounds().empty());
}

TEST_F(PathTest, FlattenStaysOnTheCurveAndAreaHasTheWinding) {
    Path circle;
    // four cubic quarter arcs of radius 10, counter-clockwise in a y-up space
    const double k = 4.0 / 3.0 * std::tan(3.14159265358979323846 / 8.0) * 10.0;
    circle.moveTo(10, 0);
    circle.cubicTo(10, k, k, 10, 0, 10);
    circle.cubicTo(-k, 10, -10, k, -10, 0);
    circle.cubicTo(-10, -k, -k, -10, 0, -10);
    circle.cubicTo(k, -10, 10, -k, 10, 0);
    circle.close();
    for (const auto& contour : circle.flatten(0.01)) {
        for (const TitleVec2& point : contour) EXPECT_NEAR(std::hypot(point.x, point.y), 10.0, 0.03);
    }
    EXPECT_NEAR(circle.signedArea(0.01), 3.14159265358979323846 * 100.0, 0.5);
    EXPECT_NEAR(square(0, 0, 4).signedArea(0.1), 16.0, 1e-9);
}

TEST_F(PathTest, TransformAndAppend) {
    Path p = square(0, 0, 2);
    p.append(square(10, 10, 1));
    EXPECT_EQ(p.contourCount(), 2u);
    p.transform(Affine::translate(5, 0).after(Affine::scale(2, 2)));
    const TitleRect b = p.bounds();
    EXPECT_DOUBLE_EQ(b.x, 5);
    EXPECT_DOUBLE_EQ(b.width, 22);
    const TitleVec2 turned = Affine::rotateDegrees(90).apply({1, 0});
    EXPECT_NEAR(turned.x, 0, 1e-12);
    EXPECT_NEAR(turned.y, 1, 1e-12) << "counter-clockwise in a y-up space";
}
