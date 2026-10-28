// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Freehand strokes (S18 R26, AC4): simplify, smooth, snap, close, and extend an existing spline
// from one of its endpoints without touching its knots.

#include "SplineTestCommon.h"

#include "spline/SplineStroke.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::spline;
using namespace jasperengine::test;

namespace{

    // count points around a circle of radius r in XZ with Gaussian noise of sigma on x and z
    std::vector<Vector3> strokeNoisyCircle(uint32_t count, float r, float sigma, uint32_t seed){
        SplineTestRandom random(seed);
        std::vector<Vector3> out;
        for (uint32_t i = 0; i < count; i++){
            const float a = 2.0f * SPLINE_TEST_PI * float(i) / float(count);
            out.push_back(Vector3(r * std::cos(a) + sigma * random.gaussian(), 0.0f, r * std::sin(a) + sigma * random.gaussian()));
        }
        return out;
    }

    bool strokeSameKnot(const SplineKnot& a, const SplineKnot& b){
        return std::memcmp(&a.position, &b.position, sizeof(Vector3)) == 0 &&
               std::memcmp(&a.tangentIn, &b.tangentIn, sizeof(Vector3)) == 0 &&
               std::memcmp(&a.tangentOut, &b.tangentOut, sizeof(Vector3)) == 0 &&
               std::memcmp(&a.radius, &b.radius, sizeof(float)) == 0 &&
               std::memcmp(&a.rollDeg, &b.rollDeg, sizeof(float)) == 0 && a.mode == b.mode;
    }

}

TEST(SplineStrokeTests, NoisyCircleBecomesAFewKnotsCloseToTheCircle){
    const std::vector<Vector3> stroke = strokeNoisyCircle(500, 10.0f, 0.1f, 1234);
    SplineData s;
    ASSERT_TRUE(strokeToSpline(stroke, SplineStrokeOptions(), nullptr, s));
    EXPECT_EQ(s.type(), SplineType::CatmullRom);
    EXPECT_TRUE(s.closed()) << "the stroke ended on its start";
    EXPECT_LE(s.knotCount(), 24u);
    EXPECT_GE(s.knotCount(), 3u);
    float worst = 0.0f;
    for (int i = 0; i <= 2000; i++){
        const Vector3 p = evaluatePosition(s, float(i) / 2000.0f);
        worst = std::max(worst, std::fabs(std::sqrt(p.x * p.x + p.z * p.z) - 10.0f));
    }
    EXPECT_LE(worst, 0.6f);
    for (const SplineKnot& k : s.knots()){
        EXPECT_EQ(k.radius, 2.0f);
    }
}

TEST(SplineStrokeTests, OpenStrokesStayOpen){
    std::vector<Vector3> stroke;
    for (int i = 0; i <= 200; i++){
        const float x = float(i) * 0.1f;
        stroke.push_back(Vector3(x, 0.0f, 3.0f * std::sin(x * 0.4f)));
    }
    SplineStrokeShape shape;
    ASSERT_TRUE(shapeStroke(stroke, SplineStrokeOptions(), nullptr, shape));
    EXPECT_FALSE(shape.closed);
    // the ends are where the stroke started and stopped
    expectNearVector(shape.points.front(), stroke.front(), 0.0f);
    expectNearVector(shape.points.back(), stroke.back(), 0.0f);
    EXPECT_LT(shape.points.size(), 20u);
}

TEST(SplineStrokeTests, AShortScribbleAtTheStartDoesNotClose){
    // ends on its start but never leaves the close circle
    const std::vector<Vector3> stroke = {Vector3(0, 0, 0), Vector3(0.3f, 0, 0.1f), Vector3(0.5f, 0, 0.4f), Vector3(0.2f, 0, 0.3f), Vector3(0.05f, 0, 0.05f)};
    SplineStrokeShape shape;
    ASSERT_TRUE(shapeStroke(stroke, SplineStrokeOptions(), nullptr, shape));
    EXPECT_FALSE(shape.closed);
}

TEST(SplineStrokeTests, TooShortStrokesAreRefused){
    SplineData s;
    const std::vector<Vector3> one = {Vector3(1, 2, 3)};
    const std::vector<Vector3> repeated = {Vector3(1, 2, 3), Vector3(1, 2, 3), Vector3(1, 2, 3)};
    EXPECT_FALSE(strokeToSpline(one, SplineStrokeOptions(), nullptr, s));
    EXPECT_FALSE(strokeToSpline(repeated, SplineStrokeOptions(), nullptr, s));
    EXPECT_EQ(s.knotCount(), 0u);
}

TEST(SplineStrokeTests, SmoothingKeepsTheEndpointsAndDamps){
    std::vector<Vector3> zigzag;
    for (int i = 0; i <= 20; i++){
        zigzag.push_back(Vector3(float(i), 0.0f, (i % 2 == 0) ? 1.0f : -1.0f));
    }
    const std::vector<Vector3> before = zigzag;
    smoothPolyline(zigzag, 2);
    expectNearVector(zigzag.front(), before.front(), 0.0f);
    expectNearVector(zigzag.back(), before.back(), 0.0f);
    for (size_t i = 2; i + 2 < zigzag.size(); i++){
        // each iteration multiplies the alternating component by (1 - 2 * 0.5) = 0
        EXPECT_NEAR(zigzag[i].z, 0.0f, 1e-6f);
        EXPECT_NEAR(zigzag[i].x, before[i].x, 1e-6f);
    }
    smoothPolyline(zigzag, 0);
    std::vector<Vector3> two = {Vector3(0, 0, 0), Vector3(1, 1, 1)};
    smoothPolyline(two, 5);
    expectNearVector(two[1], Vector3(1, 1, 1), 0.0f);
}

TEST(SplineStrokeTests, SnapPutsPointsOnTheSurfaceAndKeepsMisses){
    std::vector<Vector3> points = {Vector3(-5, 7, 0), Vector3(0, 7, 0), Vector3(5, 7, 0)};
    const SplineSurfaceProbe probe = [](const Vector3& p){
        SplineSurfaceSample s;
        s.hit = p.x < 3.0f;
        s.height = 0.5f * p.x;
        return s;
    };
    EXPECT_EQ(snapPoints(points, probe, Vector3(0, 1, 0)), 2u);
    expectNearVector(points[0], Vector3(-5, -2.5f, 0), 0.0f);
    expectNearVector(points[1], Vector3(0, 0, 0), 0.0f);
    expectNearVector(points[2], Vector3(5, 7, 0), 0.0f);

    // a 2D spline measures heights along +Z
    std::vector<Vector3> flat = {Vector3(1, 2, 9)};
    const SplineSurfaceProbe ground = [](const Vector3&){
        SplineSurfaceSample s;
        s.hit = true;
        s.height = 0.0f;
        return s;
    };
    snapPoints(flat, ground, Vector3(0, 0, 1));
    expectNearVector(flat[0], Vector3(1, 2, 0), 0.0f);
}

TEST(SplineStrokeTests, StrokeSnapsItsKnotsWithAProbe){
    std::vector<Vector3> stroke;
    for (int i = 0; i <= 100; i++){
        stroke.push_back(Vector3(float(i) * 0.2f, 30.0f, std::sin(float(i) * 0.1f) * 4.0f));
    }
    const SplineSurfaceProbe terrain = [](const Vector3& p){
        SplineSurfaceSample s;
        s.hit = true;
        s.height = 2.0f + 0.1f * p.z;
        return s;
    };
    SplineData s;
    ASSERT_TRUE(strokeToSpline(stroke, SplineStrokeOptions(), &terrain, s));
    for (const SplineKnot& k : s.knots()){
        EXPECT_NEAR(k.position.y, 2.0f + 0.1f * k.position.z, 1e-5f);
    }
}

TEST(SplineStrokeTests, ExtendFromTheEndAppendsAndKeepsTheKnotsBitIdentical){
    for (SplineType type : {SplineType::CatmullRom, SplineType::CubicBezier, SplineType::Linear}){
        SplineData s = makeSpline(type, {Vector3(0, 0, 0), Vector3(5, 1, 2), Vector3(10, 0, 0)});
        s.setKnotRadius(2, 3.5f);
        s.setKnotRoll(2, 12.0f);
        if (type == SplineType::CubicBezier){
            s.setKnotTangentOut(0, Vector3(1, 0, 1));
            s.setKnotTangentOut(1, Vector3(2, 0, -1));
            s.setKnotTangentOut(2, Vector3(1.5f, 0, 0.5f));
        }
        const std::vector<SplineKnot> original = s.knots();
        const uint64_t version = s.version();
        const std::vector<Vector3> stroke = {Vector3(10.3f, 0, 0.2f), Vector3(14, 0, 1), Vector3(18, 0, 3)};
        EXPECT_EQ(extendWithStroke(s, stroke, 1.0f), SplineStrokeExtension::Appended);
        EXPECT_GT(s.version(), version);
        ASSERT_EQ(s.knotCount(), original.size() + 2);
        for (size_t i = 0; i < original.size(); i++){
            EXPECT_TRUE(strokeSameKnot(s.knot(i), original[i])) << "knot " << i;
        }
        expectNearVector(s.knot(3).position, Vector3(14, 0, 1), 0.0f);
        expectNearVector(s.knot(4).position, Vector3(18, 0, 3), 0.0f);
        // the new knots continue the endpoint's channels
        EXPECT_EQ(s.knot(4).radius, 3.5f);
        EXPECT_EQ(s.knot(4).rollDeg, 12.0f);
        if (type == SplineType::CubicBezier){
            EXPECT_GT(s.knot(3).tangentOut.x, 0.0f) << "the handles point along the extension";
        }
    }
}

TEST(SplineStrokeTests, ExtendFromTheStartPrependsReversed){
    SplineData s = makeSpline(SplineType::CubicBezier, {Vector3(0, 0, 0), Vector3(5, 0, 0)});
    s.setKnotTangentOut(0, Vector3(1, 0, 0));
    s.setKnotTangentOut(1, Vector3(1, 0, 0));
    const std::vector<SplineKnot> original = s.knots();
    const std::vector<Vector3> stroke = {Vector3(-0.2f, 0, 0.1f), Vector3(-4, 0, 0), Vector3(-8, 0, 2)};
    EXPECT_EQ(extendWithStroke(s, stroke, 0.5f), SplineStrokeExtension::Prepended);
    ASSERT_EQ(s.knotCount(), 4u);
    // the curve still runs one way: the farthest new knot first
    expectNearVector(s.knot(0).position, Vector3(-8, 0, 2), 0.0f);
    expectNearVector(s.knot(1).position, Vector3(-4, 0, 0), 0.0f);
    EXPECT_TRUE(strokeSameKnot(s.knot(2), original[0]));
    EXPECT_TRUE(strokeSameKnot(s.knot(3), original[1]));
    // handles along the spline's direction (toward the old start)
    EXPECT_GT(s.knot(1).tangentOut.x, 0.0f);
}

TEST(SplineStrokeTests, ExtendRefusesClosedSplinesAndFarStrokes){
    SplineData s = makeSpline(SplineType::CatmullRom, {Vector3(0, 0, 0), Vector3(5, 0, 0), Vector3(5, 0, 5)});
    const std::vector<Vector3> far = {Vector3(2, 0, 2), Vector3(3, 0, 3)};
    const uint64_t version = s.version();
    EXPECT_EQ(extendWithStroke(s, far, 1.0f), SplineStrokeExtension::None);
    EXPECT_EQ(s.version(), version);
    s.setClosed(true);
    const std::vector<Vector3> fromEnd = {Vector3(5, 0, 5), Vector3(8, 0, 8)};
    EXPECT_EQ(extendWithStroke(s, fromEnd, 1.0f), SplineStrokeExtension::None);
    EXPECT_EQ(s.knotCount(), 3u);
}

TEST(SplineStrokeTests, ExtendPicksTheNearerEndpoint){
    // both ends within the pick radius: the nearer wins
    SplineData s = makeSpline(SplineType::CatmullRom, {Vector3(0, 0, 0), Vector3(1, 0, 0)});
    const std::vector<Vector3> stroke = {Vector3(0.1f, 0, 0), Vector3(-3, 0, 0)};
    EXPECT_EQ(extendWithStroke(s, stroke, 2.0f), SplineStrokeExtension::Prepended);
    expectNearVector(s.knot(0).position, Vector3(-3, 0, 0), 0.0f);
}
