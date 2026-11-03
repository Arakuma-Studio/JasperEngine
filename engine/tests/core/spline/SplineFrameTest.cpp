// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Frames (S18 R4, AC3): right = normalize(forward x up), parallel transport where forward is
// vertical, continuity through vertical runs and steep helices, and 2D frames in the XY plane.

#include "SplineTestCommon.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::spline;
using namespace jasperengine::test;

namespace{

    // the smallest dot between the rights of consecutive frames over count samples
    float frameWorstRightDot(const SplineData& s, uint32_t count){
        float worst = 1.0f;
        Vector3 previous = evaluate(s, 0.0f).right;
        for (uint32_t i = 1; i <= count; i++){
            const SplineFrame f = evaluate(s, float(i) / float(count));
            expectOrthonormal(f.forward, f.right, f.up);
            worst = std::min(worst, splineTestDot(previous, f.right));
            previous = f.right;
        }
        return worst;
    }

    std::vector<Vector3> frameHelix(float radius, float pitch, uint32_t perTurn, uint32_t turns){
        std::vector<Vector3> out;
        for (uint32_t i = 0; i <= perTurn * turns; i++){
            const float a = 2.0f * SPLINE_TEST_PI * float(i) / float(perTurn);
            out.push_back(Vector3(radius * std::cos(a), pitch * a / (2.0f * SPLINE_TEST_PI), radius * std::sin(a)));
        }
        return out;
    }

}

TEST(SplineFrameTests, RightIsForwardCrossUp){
    const SplineData s = makeSpline(SplineType::CatmullRom, {Vector3(0, 0, 0), Vector3(5, 2, -3), Vector3(9, 1, 4), Vector3(14, -2, 0)});
    for (int i = 0; i <= 100; i++){
        const SplineFrame f = evaluate(s, float(i) / 100.0f);
        expectOrthonormal(f.forward, f.right, f.up, 1e-5f);
        const Vector3 c = splineTestCross(f.forward, Vector3(0, 1, 0));
        expectNearVector(f.right, c * (1.0f / splineTestLength(c)), 1e-5f);
        expectNearVector(f.up, splineTestCross(f.right, f.forward), 1e-6f);
        // right is level: R4 is roll-free until roll is applied
        EXPECT_NEAR(f.right.y, 0.0f, 1e-6f);
    }
}

TEST(SplineFrameTests, VerticalBezierRunKeepsAContinuousRight){
    // a step up a wall: horizontal, exactly vertical at the middle knot, horizontal again
    SplineData s = makeSpline(SplineType::CubicBezier, {Vector3(0, 0, 0), Vector3(5, 5, 0), Vector3(10, 10, 0)});
    s.setKnotTangentOut(0, Vector3(4, 0, 0));
    s.setKnotTangentOut(1, Vector3(0, 3, 0));
    s.setKnotTangentIn(2, Vector3(-4, 0, 0));
    const SplineFrame middle = evaluate(s, 0.5f);
    expectNearVector(middle.forward, Vector3(0, 1, 0), 1e-6f);
    // forward x up vanishes there: the frame is the one before it, carried over
    expectNearVector(middle.right, Vector3(0, 0, 1), 1e-4f);
    EXPECT_GT(frameWorstRightDot(s, 2000), 0.99f);
}

TEST(SplineFrameTests, SteepHelixFramesAreContinuous){
    // the forward of a narrow helix stays within a fraction of a degree of vertical (outside the
    // 1e-4 cone, so right follows the turning heading) without ever flipping
    for (float radius : {3.0f, 0.5f, 0.01f}){
        const SplineData s = makeSpline(SplineType::CatmullRom, frameHelix(radius, 10.0f, 8, 3));
        EXPECT_GT(frameWorstRightDot(s, 4000), 0.99f) << "radius " << radius;
    }
}

TEST(SplineFrameTests, VerticalLinearRunTransportsTheFrameBeforeIt){
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(10, 0, 0), Vector3(10, 10, 0), Vector3(20, 10, 0)});
    // the vertical segment takes the first segment's right (+Z) all along
    for (int i = 0; i <= 20; i++){
        const SplineFrame f = evaluateSegment(s, 1, float(i) / 20.0f);
        expectNearVector(f.forward, Vector3(0, 1, 0), 1e-6f);
        expectNearVector(f.right, Vector3(0, 0, 1), 1e-5f);
        expectNearVector(f.up, Vector3(-1, 0, 0), 1e-5f);
    }
}

TEST(SplineFrameTests, AnOpenStartUpTheWallTakesTheFirstFrameAfterIt){
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(0, 10, 0), Vector3(0, 10, -10)});
    // the second segment runs -Z: right = -Z x +Y = +X
    const SplineFrame f = evaluate(s, 0.25f);
    expectNearVector(f.right, Vector3(1, 0, 0), 1e-5f);
    expectOrthonormal(f.forward, f.right, f.up);
}

TEST(SplineFrameTests, AFullyVerticalSplineHasOneConsistentFrame){
    for (SplineType type : {SplineType::Linear, SplineType::CatmullRom}){
        const SplineData s = makeSpline(type, {Vector3(2, 0, 3), Vector3(2, 5, 3), Vector3(2, 12, 3)});
        const SplineFrame first = evaluate(s, 0.0f);
        // +X for a run up +Y
        expectNearVector(first.right, Vector3(1, 0, 0), 1e-6f);
        EXPECT_GT(frameWorstRightDot(s, 300), 0.9999f);
    }
    // and straight down, the mirror image
    const SplineData down = makeSpline(SplineType::Linear, {Vector3(0, 10, 0), Vector3(0, 0, 0)});
    expectNearVector(evaluate(down, 0.5f).right, Vector3(-1, 0, 0), 1e-6f);
}

TEST(SplineFrameTests, ClosedVerticalLoopTransportsAroundTheRing){
    // a closed loop standing in the XY plane: forward is vertical at the left and right sides
    SplineData s = makeSpline(SplineType::CubicBezier, {Vector3(0, 0, 0), Vector3(5, 5, 0), Vector3(0, 10, 0), Vector3(-5, 5, 0)}, true);
    s.setKnotTangentOut(0, Vector3(2.76f, 0, 0));
    s.setKnotTangentOut(1, Vector3(0, 2.76f, 0));
    s.setKnotTangentOut(2, Vector3(-2.76f, 0, 0));
    s.setKnotTangentOut(3, Vector3(0, -2.76f, 0));
    for (int i = 0; i <= 400; i++){
        const SplineFrame f = evaluate(s, float(i) / 400.0f);
        expectOrthonormal(f.forward, f.right, f.up);
        // every right of a loop in the XY plane is +Z or -Z
        EXPECT_NEAR(std::fabs(f.right.z), 1.0f, 1e-4f);
    }
}

TEST(SplineFrameTests, TwoDimensionalFramesLieInTheXYPlane){
    for (SplineType type : {SplineType::Linear, SplineType::CatmullRom, SplineType::CubicBezier}){
        SplineData s = makeSpline(type, {Vector3(0, 0, 0), Vector3(4, 3, 0), Vector3(9, -2, 0), Vector3(14, 1, 0)});
        s.setUpAxis(Vector3(0, 0, 1));
        if (type == SplineType::CubicBezier){
            s.setKnotTangentOut(1, Vector3(2, 0, 0));
            s.setKnotTangentOut(2, Vector3(1, 1, 0));
        }
        for (int i = 0; i <= 200; i++){
            const SplineFrame f = evaluate(s, float(i) / 200.0f);
            EXPECT_EQ(f.position.z, 0.0f);
            EXPECT_NEAR(f.forward.z, 0.0f, 1e-6f);
            EXPECT_NEAR(f.right.z, 0.0f, 1e-6f);
            expectNearVector(f.up, Vector3(0, 0, 1), 1e-5f);
            expectOrthonormal(f.forward, f.right, f.up, 1e-5f);
        }
    }
    // right-handed with the camera down -Z: travelling +X, the right of travel is -Y
    SplineData line = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(10, 0, 0)});
    line.setUpAxis(Vector3(0, 0, 1));
    expectNearVector(evaluate(line, 0.5f).right, Vector3(0, -1, 0), 1e-6f);
}

TEST(SplineFrameTests, TwoDimensionalRollTurnsOutOfThePlane){
    SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(10, 0, 0)});
    s.setUpAxis(Vector3(0, 0, 1));
    s.setKnotRoll(0, 90.0f);
    s.setKnotRoll(1, 90.0f);
    const SplineFrame f = evaluate(s, 0.3f);
    expectNearVector(f.right, Vector3(0, 0, 1), 1e-5f);
    expectNearVector(f.up, Vector3(0, 1, 0), 1e-5f);
}

TEST(SplineFrameTests, FramesAreOrthonormalOnRandomSplines){
    SplineTestRandom random(5);
    for (int n = 0; n < 20; n++){
        std::vector<Vector3> knots;
        for (int i = 0; i < 6; i++){
            knots.push_back(Vector3(random.uniform(-10, 10), random.uniform(-10, 10), random.uniform(-10, 10)));
        }
        for (SplineType type : {SplineType::Linear, SplineType::CatmullRom, SplineType::CubicBezier}){
            SplineData s = makeSpline(type, knots, n % 2 == 0);
            for (size_t i = 0; i < knots.size(); i++){
                s.setKnotRoll(i, random.uniform(-180, 180));
                if (type == SplineType::CubicBezier){
                    s.setKnotTangentOut(i, Vector3(random.uniform(-4, 4), random.uniform(-4, 4), random.uniform(-4, 4)));
                }
            }
            for (int i = 0; i <= 50; i++){
                const SplineFrame f = evaluateAtDistance(s, length(s) * float(i) / 50.0f);
                expectOrthonormal(f.forward, f.right, f.up);
            }
        }
    }
}
