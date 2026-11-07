// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/Spline/Tests/{SplineEvaluatorTests,SplineResampleTests,
// SplinePointRemovalTests,SplineEffectiveClosedTests}.cpp, Copyright (c) 2026 Gamecrafters Guild
// Limited and contributors, MIT License. See NOTICE.md.

// The spline core's evaluation (S18 R1-R3, AC1): the ported OpenEngine cases (positions that were
// exact for their uniform Catmull-Rom re-baselined against a reference centripetal curve, S18 §9;
// their left-handed right vectors mirrored), knot editing, tangent modes, effective closure,
// resampling, and AC1 as written.

#include "SplineTestCommon.h"

#include "spline/SplineUtility.h"

#include <array>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::spline;
using namespace jasperengine::test;

namespace{

    const float EVAL_EPS = 0.01f;

    SplineData evalLinear(const Vector3& a, const Vector3& b, float radius = 2.0f){
        return makeSpline(SplineType::Linear, {a, b}, false, radius);
    }

    SplineData evalCatmullRom4(){
        return makeSpline(SplineType::CatmullRom, {Vector3(0, 0, 0), Vector3(10, 0, 0), Vector3(20, 0, 10), Vector3(30, 0, 10)});
    }

    SplineData evalChain(uint32_t count){
        SplineData s;
        for (uint32_t i = 0; i < count; i++){
            SplineKnot k;
            k.position = Vector3(float(i) * 10.0f, 0.0f, 0.0f);
            k.radius = 1.0f + float(i);
            k.rollDeg = 5.0f * float(i + 1);
            s.addKnot(k);
        }
        return s;
    }

    // The reference centripetal Catmull-Rom (alpha 0.5): the Barry-Goldman pyramid in double,
    // with the same reflected phantom points at open ends. Independent of the Hermite/Bezier
    // form the engine evaluates.
    Vector3 evalReferenceCentripetal(const std::vector<Vector3>& k, bool closed, double t){
        const size_t n = k.size();
        const size_t segments = closed ? n : n - 1;
        double x = std::min(std::max(t, 0.0), 1.0) * double(segments);
        size_t seg = std::min(size_t(x), segments - 1);
        const double u = x - double(seg);
        const auto at = [&](long i) -> std::array<double, 3> {
            const Vector3& p = k[size_t((i % long(n) + long(n)) % long(n))];
            return {double(p.x), double(p.y), double(p.z)};
        };
        std::array<double, 3> p1 = at(long(seg));
        std::array<double, 3> p2 = at(long(seg) + 1);
        std::array<double, 3> p0;
        std::array<double, 3> p3;
        for (int c = 0; c < 3; c++){
            p0[c] = 2.0 * p1[c] - p2[c];
            p3[c] = 2.0 * p2[c] - p1[c];
        }
        if (closed || seg > 0){
            p0 = at(long(seg) - 1);
        }
        if (closed || seg + 2 < n){
            p3 = at(long(seg) + 2);
        }
        const auto knotGap = [](const std::array<double, 3>& a, const std::array<double, 3>& b){
            const double d = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
            return std::sqrt(d);
        };
        const double t0 = 0.0;
        const double t1 = t0 + knotGap(p0, p1);
        const double t2 = t1 + knotGap(p1, p2);
        const double t3 = t2 + knotGap(p2, p3);
        const double tt = t1 + u * (t2 - t1);
        Vector3 out;
        for (int c = 0; c < 3; c++){
            const double a1 = (t1 - tt) / (t1 - t0) * p0[c] + (tt - t0) / (t1 - t0) * p1[c];
            const double a2 = (t2 - tt) / (t2 - t1) * p1[c] + (tt - t1) / (t2 - t1) * p2[c];
            const double a3 = (t3 - tt) / (t3 - t2) * p2[c] + (tt - t2) / (t3 - t2) * p3[c];
            const double b1 = (t2 - tt) / (t2 - t0) * a1 + (tt - t0) / (t2 - t0) * a2;
            const double b2 = (t3 - tt) / (t3 - t1) * a2 + (tt - t1) / (t3 - t1) * a3;
            out[c] = float((t2 - tt) / (t2 - t1) * b1 + (tt - t1) / (t2 - t1) * b2);
        }
        return out;
    }

    void evalExpectFiniteOrthonormalSweep(const SplineData& s){
        for (float t : {0.0f, 0.25f, 1.0f / 3.0f, 0.5f, 0.75f, 1.0f}){
            const SplineFrame f = evaluate(s, t);
            expectOrthonormal(f.forward, f.right, f.up);
        }
    }

    // the largest relative deviation of the gaps between count frames evenly spaced by arc length
    float evalGapSpread(const SplineData& s, uint32_t count){
        std::vector<SplineFrame> frames;
        sampleUniform(s, count, frames);
        std::vector<float> gaps;
        double sum = 0.0;
        for (size_t i = 1; i < frames.size(); i++){
            gaps.push_back(splineTestLength(frames[i].position - frames[i - 1].position));
            sum += gaps.back();
        }
        const float mean = float(sum / double(gaps.size()));
        float worst = 0.0f;
        for (float g : gaps){
            worst = std::max(worst, std::fabs(g - mean) / mean);
        }
        return worst;
    }

}

// ---- Basic evaluation ----

TEST(SplineEvaluatorTests, Linear3PointMidSegment){
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(10, 0, 0), Vector3(10, 0, 10)});
    // t is uniform per segment: 0.25 is the middle of the first segment, 0.75 of the second
    expectNearVector(evaluatePosition(s, 0.25f), Vector3(5, 0, 0), EVAL_EPS);
    expectNearVector(evaluatePosition(s, 0.75f), Vector3(10, 0, 5), EVAL_EPS);
}

TEST(SplineEvaluatorTests, LinearFrameForward){
    const SplineData s = evalLinear(Vector3(0, 0, 0), Vector3(10, 0, 0));
    const SplineFrame f = evaluate(s, 0.5f);
    expectNearVector(f.forward, Vector3(1, 0, 0), EVAL_EPS);
    // right-handed: right = forward x up = +Z for +X travel, up = right x forward = +Y
    expectNearVector(f.right, Vector3(0, 0, 1), EVAL_EPS);
    expectNearVector(f.up, Vector3(0, 1, 0), EVAL_EPS);
}

// ---- CatmullRom ----

TEST(SplineEvaluatorTests, CatmullRomPassesThroughItsKnots){
    // OpenEngine's "near (10, 0, 0) within 1": with t uniform per segment t = k / 3 is knot k
    const SplineData s = evalCatmullRom4();
    expectNearVector(evaluatePosition(s, 1.0f / 3.0f), Vector3(10, 0, 0), 1e-4f);
    expectNearVector(evaluatePosition(s, 2.0f / 3.0f), Vector3(20, 0, 10), 1e-4f);
}

TEST(SplineEvaluatorTests, CatmullRomMatchesTheReferenceCentripetalCurve){
    const std::vector<std::vector<Vector3>> cases = {
        {Vector3(0, 0, 0), Vector3(10, 0, 0), Vector3(20, 0, 10), Vector3(30, 0, 10)},
        // uneven spacing, the case centripetal exists for
        {Vector3(0, 0, 0), Vector3(0.5f, 0, 0.2f), Vector3(12, 1, -3), Vector3(13, 2, -2), Vector3(30, 0, 5)},
    };
    for (const std::vector<Vector3>& knots : cases){
        for (bool closed : {false, true}){
            SplineData s = makeSpline(SplineType::CatmullRom, knots, closed);
            for (int i = 0; i <= 200; i++){
                const float t = float(i) / 200.0f;
                const Vector3 expected = evalReferenceCentripetal(knots, closed, double(t));
                expectNearVector(evaluatePosition(s, t), expected, 2e-4f, closed ? "closed" : "open");
            }
        }
    }
}

TEST(SplineEvaluatorTests, CatmullRomHasNoCuspOnUnevenSpacing){
    // a uniform Catmull-Rom loops back on itself through the tight pair; centripetal stays a
    // forward-moving curve (its tangent never reverses against the chord)
    const SplineData s = makeSpline(SplineType::CatmullRom, {Vector3(0, 0, 0), Vector3(20, 0, 0), Vector3(20.5f, 0, 0.5f), Vector3(40, 0, 1)});
    for (int i = 0; i <= 300; i++){
        const SplineFrame f = evaluate(s, float(i) / 300.0f);
        EXPECT_GT(f.forward.x, -0.2f) << "t " << float(i) / 300.0f;
    }
}

// ---- Arc length ----

TEST(SplineEvaluatorTests, DistanceToTLinear){
    const SplineData s = evalLinear(Vector3(0, 0, 0), Vector3(10, 0, 0));
    EXPECT_NEAR(distanceToT(s, 5.0f), 0.5f, 1e-5f);
    EXPECT_EQ(distanceToT(s, 0.0f), 0.0f);
    EXPECT_EQ(distanceToT(s, 10.0f), 1.0f);
}

TEST(SplineEvaluatorTests, TToDistanceLinear){
    const SplineData s = evalLinear(Vector3(0, 0, 0), Vector3(10, 0, 0));
    EXPECT_NEAR(tToDistance(s, 0.5f), 5.0f, 1e-4f);
    EXPECT_EQ(tToDistance(s, 0.0f), 0.0f);
    EXPECT_EQ(tToDistance(s, 1.0f), length(s));
}

TEST(SplineEvaluatorTests, EvaluateAtDistanceLinear){
    const SplineData s = evalLinear(Vector3(0, 0, 0), Vector3(10, 0, 0));
    expectNearVector(evaluateAtDistance(s, 5.0f).position, Vector3(5, 0, 0), 1e-4f);
}

TEST(SplineEvaluatorTests, DistanceAndParameterRoundTripOnUnevenCurves){
    const SplineData cases[] = {
        makeSpline(SplineType::CatmullRom, {Vector3(0, 0, 0), Vector3(0.5f, 0, 0.2f), Vector3(12, 1, -3), Vector3(13, 2, -2), Vector3(30, 0, 5)}),
        bezierCircle(7.0f),
    };
    for (const SplineData& s : cases){
        const float total = length(s);
        for (int i = 0; i <= 100; i++){
            const float d = total * float(i) / 100.0f;
            EXPECT_NEAR(tToDistance(s, distanceToT(s, d)), d, 1e-4f * total) << "d " << d;
        }
        float previous = -1.0f;
        for (int i = 0; i <= 100; i++){
            const float t = distanceToT(s, total * float(i) / 100.0f);
            EXPECT_GE(t, previous);
            previous = t;
        }
    }
}

TEST(SplineEvaluatorTests, ClosedSplineDistancesWrap){
    const SplineData s = makeSpline(SplineType::CatmullRom, circleKnots(6, 5.0f), true);
    const float total = length(s);
    expectNearVector(evaluateAtDistance(s, total + 1.0f).position, evaluateAtDistance(s, 1.0f).position, 1e-4f);
    expectNearVector(evaluateAtDistance(s, -1.0f).position, evaluateAtDistance(s, total - 1.0f).position, 1e-3f);
    expectNearVector(evaluate(s, 1.25f).position, evaluate(s, 0.25f).position, 1e-4f);
    // the end of the loop is its end, not its start: the distance there is the length
    EXPECT_EQ(tToDistance(s, 1.0f), total);
    // an open spline clamps instead
    const SplineData open = makeSpline(SplineType::CatmullRom, circleKnots(6, 5.0f), false);
    expectNearVector(evaluateAtDistance(open, length(open) + 3.0f).position, open.knots().back().position, 1e-5f);
    expectNearVector(evaluateAtDistance(open, -3.0f).position, open.knots().front().position, 1e-6f);
}

// ---- Closest point ----

TEST(SplineEvaluatorTests, ClosestPointOnLine){
    const SplineData s = evalLinear(Vector3(0, 0, 0), Vector3(10, 0, 0));
    const SplineHit hit = closestPoint(s, Vector3(5, 5, 0));
    expectNearVector(hit.position, Vector3(5, 0, 0), 1e-4f);
    EXPECT_NEAR(hit.distance, 5.0f, 1e-4f);
    EXPECT_NEAR(hit.t, 0.5f, 1e-4f);
    EXPECT_EQ(hit.segment, 0u);
}

TEST(SplineEvaluatorTests, ClosestPointBeyondEnd){
    const SplineData s = evalLinear(Vector3(0, 0, 0), Vector3(10, 0, 0));
    const SplineHit hit = closestPoint(s, Vector3(15, 0, 0));
    expectNearVector(hit.position, Vector3(10, 0, 0), 1e-5f);
    EXPECT_EQ(hit.t, 1.0f);
}

// ---- Signed distance ----

TEST(SplineEvaluatorTests, SignedDistanceInside){
    // (5, 2, 0) is 2 units from the path, inside the 5-unit radius
    const SplineData s = evalLinear(Vector3(0, 0, 0), Vector3(10, 0, 0), 5.0f);
    EXPECT_NEAR(signedDistance(s, Vector3(5, 2, 0)), -3.0f, 1e-4f);
}

TEST(SplineEvaluatorTests, SignedDistanceOutside){
    const SplineData s = evalLinear(Vector3(0, 0, 0), Vector3(10, 0, 0), 5.0f);
    EXPECT_NEAR(signedDistance(s, Vector3(5, 10, 0)), 5.0f, 1e-4f);
}

// ---- Ground-plane (XZ) queries ----

TEST(SplineEvaluatorTests, ClosestPointXZIgnoresAltitudeAndReportsTheCurveY){
    const SplineData s = evalLinear(Vector3(0, 20, 0), Vector3(10, 20, 0), 5.0f);
    const SplineHit xz = closestPointXZ(s, 5.0f, 0.0f);
    EXPECT_NEAR(xz.distance, 0.0f, EVAL_EPS);
    EXPECT_NEAR(xz.position.x, 5.0f, 1e-3f);
    EXPECT_NEAR(xz.position.y, 20.0f, EVAL_EPS) << "the XZ query dropped the curve's height";
    EXPECT_NEAR(closestPoint(s, Vector3(5, 0, 0)).distance, 20.0f, 1e-3f);
}

TEST(SplineEvaluatorTests, SignedDistanceXZIsInsideTheFootprintOfAnElevatedPath){
    const SplineData s = evalLinear(Vector3(0, 20, 0), Vector3(10, 20, 0), 5.0f);
    EXPECT_LT(signedDistanceXZ(s, 5.0f, 2.0f), 0.0f);
    EXPECT_GT(signedDistanceXZ(s, 5.0f, 9.0f), 0.0f);
    EXPECT_GT(signedDistance(s, Vector3(5, 0, 2)), 0.0f);
}

TEST(SplineEvaluatorTests, ClosestPointXZDoesNotCullTheNearestSegmentByAltitude){
    // segment 2 runs at y = 100 and is the XZ-nearest to (15, 0); a broad phase measuring the
    // boxes in 3D would keep the ground segments instead
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(6, 0, 0), Vector3(6, 100, 0), Vector3(20, 100, 0)});
    const SplineHit xz = closestPointXZ(s, 15.0f, 0.0f);
    EXPECT_NEAR(xz.distance, 0.0f, 1e-3f);
    EXPECT_NEAR(xz.position.x, 15.0f, 1e-3f);
    EXPECT_NEAR(xz.position.y, 100.0f, 1e-3f) << "the broad phase culled the XZ-nearest segment";
}

TEST(SplineEvaluatorTests, SignedDistanceXZIsNegativeInsideAnElevatedClosedLoop){
    const SplineData s = makeSpline(SplineType::Linear,
        {Vector3(-20, 50, -20), Vector3(20, 50, -20), Vector3(20, 50, 20), Vector3(-20, 50, 20)}, true, 5.0f);
    EXPECT_LT(signedDistanceXZ(s, 0.0f, 0.0f), 0.0f) << "loop interior read as outside";
    EXPECT_NEAR(signedDistanceXZ(s, 30.0f, 0.0f), 5.0f, 1e-3f);
}

// ---- Edge cases ----

TEST(SplineEvaluatorTests, InvalidSpline){
    SplineData s;
    const SplineFrame f = evaluate(s, 0.5f);
    EXPECT_EQ(f.radius, 0.0f);
    expectOrthonormal(f.forward, f.right, f.up);
    EXPECT_EQ(length(s), 0.0f);
    EXPECT_TRUE(std::isinf(closestPoint(s, Vector3(1, 2, 3)).distance));
}

TEST(SplineEvaluatorTests, SinglePoint){
    SplineData s;
    SplineKnot k;
    k.position = Vector3(5, 0, 0);
    s.addKnot(k);
    EXPECT_FALSE(s.isValid());
    EXPECT_EQ(s.segmentCount(), 0u);
    expectNearVector(evaluate(s, 0.7f).position, Vector3(5, 0, 0), 0.0f);
    EXPECT_NEAR(closestPoint(s, Vector3(5, 3, 4)).distance, 5.0f, 1e-6f);
}

TEST(SplineEvaluatorTests, ClosedSpline){
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(10, 0, 0), Vector3(10, 0, 10)}, true);
    EXPECT_EQ(s.segmentCount(), 3u);
    expectNearVector(evaluatePosition(s, 0.0f), Vector3(0, 0, 0), 0.0f);
    expectNearVector(evaluatePosition(s, 1.0f), Vector3(0, 0, 0), 0.0f);
    EXPECT_NEAR(length(s), 20.0f + std::sqrt(200.0f), 1e-4f);
}

// ---- SampleUniform ----

TEST(SplineEvaluatorTests, SampleUniformCount){
    const SplineData s = evalLinear(Vector3(0, 0, 0), Vector3(10, 0, 0));
    std::vector<SplineFrame> frames;
    sampleUniform(s, 11, frames);
    ASSERT_EQ(frames.size(), 11u);
    expectNearVector(frames.front().position, Vector3(0, 0, 0), 0.0f);
    expectNearVector(frames.back().position, Vector3(10, 0, 0), 0.0f);
    for (size_t i = 0; i < frames.size(); i++){
        EXPECT_NEAR(frames[i].position.x, float(i), 1e-4f);
    }
}

// ---- Channels: linear per segment, never through the curve basis ----

TEST(SplineEvaluatorTests, RadiusInterpolation){
    SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(10, 0, 0)});
    s.setKnotRadius(0, 2.0f);
    s.setKnotRadius(1, 8.0f);
    EXPECT_NEAR(evaluate(s, 0.5f).radius, 5.0f, 1e-6f);
}

TEST(SplineEvaluatorTests, WidthAtKnotsAndMidArc){
    SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(10, 0, 0)});
    s.setKnotRadius(0, 2.0f);
    s.setKnotRadius(1, 8.0f);
    EXPECT_NEAR(evaluateAtDistance(s, 0.0f).radius, 2.0f, 1e-4f);
    EXPECT_NEAR(evaluateAtDistance(s, 5.0f).radius, 5.0f, 1e-4f);
    EXPECT_NEAR(evaluateAtDistance(s, 10.0f).radius, 8.0f, 1e-4f);
    EXPECT_NEAR(evaluateAtDistance(s, 2.5f).radius, 3.5f, 1e-4f);
}

TEST(SplineEvaluatorTests, WidthMultiSegment){
    SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(10, 0, 0), Vector3(20, 0, 0)});
    s.setKnotRadius(0, 2.0f);
    s.setKnotRadius(1, 4.0f);
    s.setKnotRadius(2, 8.0f);
    EXPECT_NEAR(evaluateAtDistance(s, 5.0f).radius, 3.0f, 1e-4f);
    EXPECT_NEAR(evaluateAtDistance(s, 10.0f).radius, 4.0f, 1e-4f);
    EXPECT_NEAR(evaluateAtDistance(s, 15.0f).radius, 6.0f, 1e-4f);
}

TEST(SplineEvaluatorTests, WidthClampsPastTheEnds){
    SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(10, 0, 0)});
    s.setKnotRadius(0, 2.0f);
    s.setKnotRadius(1, 8.0f);
    EXPECT_EQ(evaluateAtDistance(s, -5.0f).radius, 2.0f);
    EXPECT_EQ(evaluateAtDistance(s, 50.0f).radius, 8.0f);
}

TEST(SplineEvaluatorTests, EnvelopeHalfWidthFollowsArcLength){
    {
        SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(12, 0, 0)});
        s.setKnotRadius(0, 1.5f);
        s.setKnotRadius(1, 3.0f);
        EXPECT_NEAR(evaluateAtDistance(s, 0.5f * length(s)).radius, 2.25f, 1e-4f);
    }
    {
        // unequal segments split t from arc distance: mid-arc is 8 m into the second segment
        SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(2, 0, 0), Vector3(20, 0, 0)});
        s.setKnotRadius(0, 1.5f);
        s.setKnotRadius(1, 2.25f);
        s.setKnotRadius(2, 3.0f);
        EXPECT_NEAR(evaluateAtDistance(s, 0.5f * length(s)).radius, 2.25f + 0.75f * (8.0f / 18.0f), 1e-4f);
    }
}

TEST(SplineEvaluatorTests, EnvelopeWidthOnCurvedSplineSamplesByArcLength){
    SplineData s = makeSpline(SplineType::CatmullRom, {Vector3(0, 0, 0), Vector3(10, 0, 10), Vector3(20, 0, 0)});
    s.setKnotRadius(0, 1.5f);
    s.setKnotRadius(1, 2.25f);
    s.setKnotRadius(2, 3.0f);
    // longer than the chords (2 sqrt(200) = 28.28): the curve bows, so the LUT does real work
    EXPECT_GT(length(s), 28.4f);
    // symmetric about x = 10: mid-arc is the middle knot
    EXPECT_NEAR(evaluateAtDistance(s, 0.5f * length(s)).radius, 2.25f, 1e-3f);
    const float quarter = evaluateAtDistance(s, 0.25f * length(s)).radius;
    EXPECT_GT(quarter, 1.5f);
    EXPECT_LT(quarter, 2.25f);
}

TEST(SplineEvaluatorTests, ChannelsNeverOvershootTheKnots){
    SplineData s = makeSpline(SplineType::CatmullRom, {Vector3(0, 0, 0), Vector3(3, 0, 8), Vector3(9, 0, -4), Vector3(15, 0, 2)});
    const float radii[4] = {1.0f, 6.0f, 0.5f, 3.0f};
    const float rolls[4] = {-30.0f, 45.0f, 10.0f, 0.0f};
    for (size_t i = 0; i < 4; i++){
        s.setKnotRadius(i, radii[i]);
        s.setKnotRoll(i, rolls[i]);
    }
    for (int i = 0; i <= 300; i++){
        const float t = float(i) / 300.0f;
        uint32_t seg;
        float u;
        tToSegment(s, t, seg, u);
        const SplineFrame f = evaluate(s, t);
        EXPECT_GE(f.radius, std::min(radii[seg], radii[seg + 1]) - 1e-5f);
        EXPECT_LE(f.radius, std::max(radii[seg], radii[seg + 1]) + 1e-5f);
        EXPECT_GE(f.rollDeg, std::min(rolls[seg], rolls[seg + 1]) - 1e-4f);
        EXPECT_LE(f.rollDeg, std::max(rolls[seg], rolls[seg + 1]) + 1e-4f);
    }
}

TEST(SplineEvaluatorTests, RollTurnsRightAndUpAboutForward){
    SplineData s = evalLinear(Vector3(0, 0, 0), Vector3(10, 0, 0));
    s.setKnotRoll(0, 90.0f);
    s.setKnotRoll(1, 90.0f);
    const SplineFrame f = evaluate(s, 0.5f);
    EXPECT_EQ(f.rollDeg, 90.0f);
    // positive roll raises the right side: right turns onto up
    expectNearVector(f.right, Vector3(0, 1, 0), 1e-5f);
    expectNearVector(f.up, Vector3(0, 0, -1), 1e-5f);
    expectOrthonormal(f.forward, f.right, f.up);
}

// ---- Degenerate inputs (NaN guards) ----

TEST(SplineEvaluatorTests, VerticalTangentLinearFrameIsFinite){
    evalExpectFiniteOrthonormalSweep(evalLinear(Vector3(0, 0, 0), Vector3(0, 10, 0)));
}

TEST(SplineEvaluatorTests, VerticalTangentBezierFrameIsFinite){
    SplineData s = makeSpline(SplineType::CubicBezier, {Vector3(0, 0, 0), Vector3(0, 10, 0)});
    s.setKnotTangents(0, Vector3(0, -3, 0), Vector3(0, 3, 0));
    s.setKnotTangents(1, Vector3(0, -3, 0), Vector3(0, 3, 0));
    evalExpectFiniteOrthonormalSweep(s);
}

TEST(SplineEvaluatorTests, VerticalTangentCatmullRomFrameIsFinite){
    evalExpectFiniteOrthonormalSweep(makeSpline(SplineType::CatmullRom, {Vector3(0, 0, 0), Vector3(0, 5, 0), Vector3(0, 10, 0)}));
}

TEST(SplineEvaluatorTests, CoincidentPointsLinearTangentIsFinite){
    // a double-added knot: segment 0 has no length at all
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(5, 0, 5), Vector3(5, 0, 5), Vector3(15, 0, 5)});
    evalExpectFiniteOrthonormalSweep(s);
    // the zero-length segment takes the direction of the segment after it
    expectNearVector(evaluate(s, 0.0f).forward, Vector3(1, 0, 0), 1e-5f);
}

TEST(SplineEvaluatorTests, NearVerticalTangentKeepsPrimaryFrame){
    // 1 degree off vertical is far outside the 1e-4 cone: right = normalize(forward x up)
    const float tilt = 1.0f * SPLINE_TEST_PI / 180.0f;
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0),
        Vector3(10.0f * std::sin(tilt), 10.0f * std::cos(tilt), 0), Vector3(20.0f * std::sin(tilt), 20.0f * std::cos(tilt), 0)});
    const SplineFrame f = evaluate(s, 0.5f);
    // forward tilted +X from +Y: forward x +Y = +Z (OpenEngine's left-handed -Z, mirrored)
    EXPECT_NEAR(f.right.z, 1.0f, 1e-3f);
    EXPECT_NEAR(splineTestDot(f.right, f.right), 1.0f, 1e-3f);
}

TEST(SplineEvaluatorTests, CoincidentPointsBezierTangentIsFinite){
    evalExpectFiniteOrthonormalSweep(makeSpline(SplineType::CubicBezier, {Vector3(5, 0, 5), Vector3(5, 0, 5), Vector3(15, 0, 5)}));
}

TEST(SplineEvaluatorTests, CoincidentPointsCatmullRomTangentIsFinite){
    evalExpectFiniteOrthonormalSweep(makeSpline(SplineType::CatmullRom, {Vector3(5, 0, 5), Vector3(5, 0, 5), Vector3(15, 0, 5)}));
}

TEST(SplineEvaluatorTests, NonFiniteControlPointsKeepFiniteFrames){
    const float inf = std::numeric_limits<float>::infinity();
    // finite coordinates whose squares overflow
    const float huge = 1.0e30f;
    for (SplineType type : {SplineType::Linear, SplineType::CatmullRom, SplineType::CubicBezier}){
        SplineData a = makeSpline(type, {Vector3(0, 0, 0), Vector3(inf, 0, 0), Vector3(20, 0, 0)});
        SplineData b = makeSpline(type, {Vector3(0, 0, 0), Vector3(huge, 0, 0), Vector3(2.0f * huge, 0, 0)});
        if (type == SplineType::CubicBezier){
            a.setKnotTangents(0, Vector3(-3, 0, 0), Vector3(3, 0, 0));
            a.setKnotTangents(1, Vector3(-3, 0, 0), Vector3(3, 0, 0));
            b.setKnotTangents(0, Vector3(-3, 0, 0), Vector3(huge, 0, 0));
            b.setKnotTangents(1, Vector3(-huge, 0, 0), Vector3(3, 0, 0));
        }
        evalExpectFiniteOrthonormalSweep(a);
        evalExpectFiniteOrthonormalSweep(b);
        // the other queries stay defined (no crash, no undefined conversion)
        (void)evaluateAtDistance(a, 3.0f);
        (void)closestPoint(a, Vector3(1, 1, 1));
        (void)signedDistanceXZ(b, 1.0f, 1.0f);
    }
}

TEST(SplineEvaluatorTests, LargeButFiniteControlPointsKeepTheirFrame){
    const float large = 1.0e10f;
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(large, 0, 0), Vector3(2.0f * large, 0, 0)});
    evalExpectFiniteOrthonormalSweep(s);
    EXPECT_NEAR(evaluate(s, 0.5f).forward.x, 1.0f, 1e-5f);
}

// ---- AC1 ----

TEST(SplineEvaluatorTests, EndpointsAreExactAtT0AndT1){
    const Vector3 first(1.25f, -3.5f, 7.0f);
    const Vector3 last(-4.75f, 2.0f, 11.5f);
    for (SplineType type : {SplineType::Linear, SplineType::CatmullRom, SplineType::CubicBezier}){
        SplineData s = makeSpline(type, {first, Vector3(3, 1, 2), Vector3(-2, 0.5f, 9), last});
        if (type == SplineType::CubicBezier){
            for (size_t i = 0; i < 4; i++){
                s.setKnotTangentOut(i, Vector3(1.5f, 0.25f, -0.5f));
            }
        }
        const Vector3 a = evaluatePosition(s, 0.0f);
        const Vector3 b = evaluatePosition(s, 1.0f);
        EXPECT_EQ(a.x, first.x);
        EXPECT_EQ(a.y, first.y);
        EXPECT_EQ(a.z, first.z);
        EXPECT_EQ(b.x, last.x);
        EXPECT_EQ(b.y, last.y);
        EXPECT_EQ(b.z, last.z);
        const Vector3 d0 = evaluateAtDistance(s, 0.0f).position;
        const Vector3 d1 = evaluateAtDistance(s, length(s)).position;
        EXPECT_EQ(d0.x, first.x);
        EXPECT_EQ(d0.z, first.z);
        EXPECT_EQ(d1.x, last.x);
        EXPECT_EQ(d1.z, last.z);
    }
}

TEST(SplineEvaluatorTests, StraightTwoKnotLength){
    const Vector3 a(1, 2, 3);
    const Vector3 b(7, -1, 5);
    const float expected = std::sqrt(36.0f + 9.0f + 4.0f);
    for (SplineType type : {SplineType::Linear, SplineType::CatmullRom, SplineType::CubicBezier}){
        SplineData s = makeSpline(type, {a, b});
        if (type == SplineType::CubicBezier){
            // collinear handles of unequal length: a straight curve at varying speed
            s.setKnotTangents(0, Vector3(0, 0, 0), (b - a) * 0.4f);
            s.setKnotTangents(1, (a - b) * 0.1f, Vector3(0, 0, 0));
        }
        EXPECT_NEAR(length(s), expected, 1e-5f) << "type " << int(type);
    }
}

TEST(SplineEvaluatorTests, CentripetalClosedCircleLengthIsWithinOnePercent){
    const SplineData s = makeSpline(SplineType::CatmullRom, circleKnots(8, 10.0f), true);
    const float expected = 2.0f * SPLINE_TEST_PI * 10.0f;
    EXPECT_NEAR(length(s), expected, 0.01f * expected);
}

TEST(SplineEvaluatorTests, EvaluateAtDistanceGapsAreUniform){
    const SplineData cases[] = {
        // uneven collinear knots: the LUT, not t, spaces the samples
        makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(1, 0.5f, 0.25f), Vector3(7, 3.5f, 1.75f), Vector3(10, 5, 2.5f)}),
        makeSpline(SplineType::CatmullRom, circleKnots(8, 10.0f), true),
        makeSpline(SplineType::CatmullRom, {Vector3(0, 0, 0), Vector3(0.5f, 0, 0.2f), Vector3(12, 1, -3), Vector3(13, 2, -2), Vector3(30, 0, 5)}),
        bezierCircle(10.0f),
    };
    for (const SplineData& s : cases){
        EXPECT_LT(evalGapSpread(s, 200), 0.005f) << "type " << int(s.type());
    }
    // a Bezier whose handles make the speed vary by an order of magnitude
    SplineData uneven = makeSpline(SplineType::CubicBezier, {Vector3(0, 0, 0), Vector3(10, 0, 5), Vector3(20, 0, 0)});
    uneven.setKnotMode(0, TangentMode::Broken);
    uneven.setKnotMode(1, TangentMode::Broken);
    uneven.setKnotMode(2, TangentMode::Broken);
    uneven.setKnotTangents(0, Vector3(0, 0, 0), Vector3(9, 0, 1));
    uneven.setKnotTangents(1, Vector3(-2, 0, 0), Vector3(6, 0, 0));
    uneven.setKnotTangents(2, Vector3(-1, 0, 1), Vector3(0, 0, 0));
    EXPECT_LT(evalGapSpread(uneven, 200), 0.005f);
}

// ---- Knot editing ----

TEST(SplineEvaluatorTests, TangentModesFollowTheEditedHandle){
    SplineData s = makeSpline(SplineType::CubicBezier, {Vector3(0, 0, 0), Vector3(10, 0, 0)});
    s.setKnotTangents(0, Vector3(-2, 0, 0), Vector3(1, 0, 0));
    s.setKnotTangentOut(0, Vector3(0, 0, 4));
    expectNearVector(s.knot(0).tangentIn, Vector3(0, 0, -4), 0.0f);

    s.setKnotMode(0, TangentMode::Aligned);
    s.setKnotTangents(0, Vector3(-2, 0, 0), Vector3(3, 0, 0));
    s.setKnotTangentOut(0, Vector3(0, 6, 0));
    // opposite direction, its own length kept
    expectNearVector(s.knot(0).tangentIn, Vector3(0, -2, 0), 1e-6f);
    s.setKnotTangentIn(0, Vector3(0, 0, 1));
    expectNearVector(s.knot(0).tangentOut, Vector3(0, 0, -6), 1e-5f);

    s.setKnotMode(0, TangentMode::Broken);
    s.setKnotTangentOut(0, Vector3(5, 5, 0));
    expectNearVector(s.knot(0).tangentIn, Vector3(0, 0, 1), 0.0f);

    // switching to Mirrored applies at once, tangentOut leading
    s.setKnotMode(0, TangentMode::Mirrored);
    expectNearVector(s.knot(0).tangentIn, Vector3(-5, -5, 0), 0.0f);
}

TEST(SplineEvaluatorTests, BezierHandlesShapeTheCurve){
    // the classic quarter circle: handles 0.5523 r give a maximum radial error of 2.7e-4 r
    const SplineData s = bezierCircle(10.0f);
    for (int i = 0; i <= 400; i++){
        const Vector3 p = evaluatePosition(s, float(i) / 400.0f);
        EXPECT_NEAR(std::sqrt(p.x * p.x + p.z * p.z), 10.0f, 4e-3f);
    }
}

TEST(SplineEvaluatorTests, KnotEditsBumpTheVersionAndRefusalsDoNot){
    SplineData s = evalChain(3);
    uint64_t v = s.version();
    s.setKnotPosition(1, Vector3(0, 1, 0));
    EXPECT_GT(s.version(), v);
    v = s.version();
    EXPECT_FALSE(s.setKnotPosition(3, Vector3(0, 1, 0)));
    EXPECT_FALSE(s.setKnotRadius(7, 1.0f));
    EXPECT_FALSE(s.setKnot(3, SplineKnot()));
    EXPECT_EQ(s.version(), v);
    s.setClosed(false);
    s.setType(SplineType::CatmullRom);
    EXPECT_EQ(s.version(), v) << "setting the current value is not an edit";
    s.setUpAxis(Vector3(0, 0, 0));
    EXPECT_EQ(s.version(), v) << "a zero up axis is refused";
    s.setUpAxis(Vector3(0, 0, 2));
    EXPECT_GT(s.version(), v);
    expectNearVector(s.upAxis(), Vector3(0, 0, 1), 0.0f);
    s.setKnotRadius(0, -3.0f);
    EXPECT_EQ(s.knot(0).radius, 0.0f);
    s.setKnotRadius(0, std::numeric_limits<float>::quiet_NaN());
    EXPECT_EQ(s.knot(0).radius, 0.0f);
}

TEST(SplineEvaluatorTests, InsertKnotClampsItsIndex){
    SplineData s = evalChain(2);
    SplineKnot k;
    k.position = Vector3(99, 0, 0);
    s.insertKnot(50, k);
    ASSERT_EQ(s.knotCount(), 3u);
    EXPECT_EQ(s.knots().back().position.x, 99.0f);
    k.position = Vector3(-1, 0, 0);
    s.insertKnot(0, k);
    EXPECT_EQ(s.knots().front().position.x, -1.0f);
}

// ---- Knot removal (ported) ----

TEST(SplineEvaluatorTests, RemovalRefusesToDropBelowTwoKnots){
    SplineData s = evalChain(2);
    EXPECT_FALSE(s.removeKnot(0));
    EXPECT_FALSE(s.removeKnot(1));
    EXPECT_EQ(s.knotCount(), 2u);
    EXPECT_TRUE(s.isValid());
}

TEST(SplineEvaluatorTests, RemovalRefusesAnOutOfRangeIndex){
    SplineData s = evalChain(4);
    EXPECT_FALSE(s.removeKnot(4));
    EXPECT_FALSE(s.removeKnot(99));
    EXPECT_EQ(s.knotCount(), 4u);
}

TEST(SplineEvaluatorTests, RefusedRemovalLeavesTheVersionUntouched){
    SplineData s = evalChain(2);
    const uint64_t before = s.version();
    EXPECT_FALSE(s.removeKnot(0));
    EXPECT_EQ(s.version(), before);
    SplineData four = evalChain(4);
    const uint64_t fourBefore = four.version();
    EXPECT_FALSE(four.removeKnot(4));
    EXPECT_EQ(four.version(), fourBefore);
}

TEST(SplineEvaluatorTests, SuccessfulRemovalBumpsTheVersion){
    SplineData s = evalChain(4);
    (void)length(s);
    const uint64_t before = s.version();
    EXPECT_TRUE(s.removeKnot(1));
    EXPECT_EQ(s.knotCount(), 3u);
    EXPECT_GT(s.version(), before);
}

TEST(SplineEvaluatorTests, RemovalSurvivorsKeepTheirChannels){
    SplineData s = evalChain(4);
    const std::vector<SplineKnot> before = s.knots();
    ASSERT_TRUE(s.removeKnot(1));
    const size_t source[] = {0, 2, 3};
    for (size_t i = 0; i < 3; i++){
        EXPECT_EQ(s.knot(i).position.x, before[source[i]].position.x);
        EXPECT_EQ(s.knot(i).radius, before[source[i]].radius);
        EXPECT_EQ(s.knot(i).rollDeg, before[source[i]].rollDeg);
    }
}

TEST(SplineEvaluatorTests, RemovalKeepsBezierHandlesOnTheSurvivors){
    SplineData s = evalChain(4);
    s.setType(SplineType::CubicBezier);
    for (size_t i = 0; i < 4; i++){
        const float f = float(i + 1);
        s.setKnotTangents(i, Vector3(-f, 0, 0), Vector3(f, 0, 0));
    }
    const std::vector<SplineKnot> before = s.knots();
    ASSERT_TRUE(s.removeKnot(2));
    const size_t source[] = {0, 1, 3};
    for (size_t i = 0; i < 3; i++){
        EXPECT_EQ(s.knot(i).tangentIn.x, before[source[i]].tangentIn.x);
        EXPECT_EQ(s.knot(i).tangentOut.x, before[source[i]].tangentOut.x);
    }
}

TEST(SplineEvaluatorTests, RemovalKeepsAClosedLoopConsistent){
    SplineData s = evalChain(4);
    s.setKnotPosition(1, Vector3(10, 0, 10));
    s.setKnotPosition(2, Vector3(0, 0, 10));
    s.setClosed(true);
    ASSERT_EQ(s.segmentCount(), 4u);
    ASSERT_TRUE(s.removeKnot(0));
    EXPECT_EQ(s.segmentCount(), 3u);
    EXPECT_EQ(s.ensureCache().bounds.size(), 3u);
    EXPECT_GT(length(s), 0.0f);
    ASSERT_TRUE(s.removeKnot(0));
    EXPECT_FALSE(s.removeKnot(0));
    EXPECT_TRUE(s.closed());
    EXPECT_FALSE(s.effectivelyClosed());
    EXPECT_EQ(s.segmentCount(), 1u);
}

TEST(SplineEvaluatorTests, RemovingAnEndpointShortensTheCurve){
    SplineData s = evalChain(4);
    s.setType(SplineType::Linear);
    const float before = length(s);
    ASSERT_TRUE(s.removeKnot(3));
    EXPECT_EQ(s.segmentCount(), 2u);
    EXPECT_LT(length(s), before);
    EXPECT_NEAR(length(s), 20.0f, 1e-5f);
}

// ---- Effective closure (ported) ----

TEST(SplineEvaluatorTests, EffectivelyClosedTruthTable){
    for (uint32_t count = 1; count <= 4; count++){
        SplineData open = evalChain(count);
        SplineData ring = evalChain(count);
        ring.setClosed(true);
        EXPECT_FALSE(open.effectivelyClosed());
        EXPECT_EQ(ring.effectivelyClosed(), count >= 3) << count;
    }
}

TEST(SplineEvaluatorTests, TwoKnotClosedGeneratesOneOpenSegment){
    SplineData s = evalChain(2);
    s.setClosed(true);
    s.setType(SplineType::Linear);
    EXPECT_EQ(s.segmentCount(), 1u);
    EXPECT_TRUE(s.closed());
    std::vector<SplineFrame> frames;
    sampleUniform(s, 32, frames);
    for (size_t i = 1; i < frames.size(); i++){
        EXPECT_GE(frames[i].position.x, frames[i - 1].position.x - 1e-4f) << "sample " << i << " retraced toward the start";
    }
    EXPECT_NEAR(length(s), 10.0f, 1e-5f);
    EXPECT_TRUE(s.ensureCache().polygonXZ.empty());
}

TEST(SplineEvaluatorTests, ReAddingAThirdKnotRestoresTheRing){
    SplineData s = evalChain(3);
    s.setClosed(true);
    ASSERT_EQ(s.segmentCount(), 3u);
    ASSERT_TRUE(s.removeKnot(2));
    EXPECT_FALSE(s.effectivelyClosed());
    EXPECT_EQ(s.segmentCount(), 1u);
    SplineKnot k;
    k.position = Vector3(10, 0, 10);
    s.addKnot(k);
    EXPECT_TRUE(s.effectivelyClosed());
    EXPECT_EQ(s.segmentCount(), 3u);
    EXPECT_FALSE(s.ensureCache().polygonXZ.empty());
}

// ---- Resample (ported) ----

TEST(SplineEvaluatorTests, ResampleChannelsSurvive){
    SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(10, 0, 0), Vector3(10, 0, 10), Vector3(20, 0, 10)});
    const float radii[] = {2.0f, 6.0f, 3.0f, 9.0f};
    const float rolls[] = {5.0f, -20.0f, 50.0f, 15.0f};
    for (size_t i = 0; i < 4; i++){
        s.setKnotRadius(i, radii[i]);
        s.setKnotRoll(i, rolls[i]);
    }
    const SplineData authored = s;
    // 0.1 apart puts a sample on every corner; the straight runs simplify away
    ASSERT_TRUE(resample(s, 0.1f, 0.05f));
    ASSERT_EQ(s.knotCount(), 4u);
    for (size_t i = 0; i < 4; i++){
        expectNearVector(s.knot(i).position, authored.knot(i).position, 1e-3f, "position");
        EXPECT_NEAR(s.knot(i).radius, radii[i], 1e-2f) << i;
        EXPECT_NEAR(s.knot(i).rollDeg, rolls[i], 1e-1f) << i;
    }
}

TEST(SplineEvaluatorTests, ResampleCurvedSplineCarriesTheInterpolatedWidth){
    SplineData s = makeSpline(SplineType::CatmullRom, {Vector3(0, 0, 0), Vector3(10, 0, 10), Vector3(20, 0, 0), Vector3(30, 0, 10)});
    const float radii[] = {1.5f, 4.5f, 2.0f, 6.0f};
    for (size_t i = 0; i < 4; i++){
        s.setKnotRadius(i, radii[i]);
    }
    const SplineData authored = s;
    ASSERT_TRUE(resample(s, 2.0f));
    ASSERT_GE(s.knotCount(), 20u);
    const float spacing = length(authored) / float(s.knotCount() - 1);
    for (size_t i = 0; i < s.knotCount(); i++){
        const SplineFrame expected = evaluateAtDistance(authored, spacing * float(i));
        EXPECT_NEAR(s.knot(i).radius, expected.radius, 1e-3f) << i;
        expectNearVector(s.knot(i).position, expected.position, 1e-3f, "position");
    }
    // the new curve runs through the old one closely
    EXPECT_NEAR(length(s), length(authored), 0.01f * length(authored));
}

TEST(SplineEvaluatorTests, ResampleBezierFabricatesTangents){
    SplineData s = makeSpline(SplineType::CubicBezier, {Vector3(0, 0, 0), Vector3(10, 0, 10), Vector3(20, 0, 0)});
    s.setKnotTangents(0, Vector3(0, 0, -3), Vector3(0, 0, 3));
    s.setKnotTangents(1, Vector3(-3, 0, 0), Vector3(3, 0, 0));
    s.setKnotTangents(2, Vector3(0, 0, 3), Vector3(0, 0, -3));
    ASSERT_TRUE(resample(s, 1.0f, 0.1f));
    ASSERT_GE(s.knotCount(), 3u);
    for (size_t i = 1; i + 1 < s.knotCount(); i++){
        EXPECT_GT(splineTestDot(s.knot(i).tangentOut, s.knot(i).tangentOut), 1e-4f) << i;
        expectNearVector(s.knot(i).tangentIn, -s.knot(i).tangentOut, 0.0f, "Mirrored");
    }
}

TEST(SplineEvaluatorTests, ResampleKeepsAClosedLoopClosed){
    SplineData s = makeSpline(SplineType::CatmullRom, circleKnots(5, 8.0f), true);
    const float before = length(s);
    ASSERT_TRUE(resample(s, 1.0f));
    EXPECT_TRUE(s.effectivelyClosed());
    // no knot repeats the first
    EXPECT_GT(splineTestLength(s.knots().back().position - s.knots().front().position), 0.5f);
    EXPECT_NEAR(length(s), before, 0.01f * before);
    EXPECT_FALSE(resample(s, 0.0f));
}

TEST(SplineEvaluatorTests, SimplifyIndicesMatchesThePositionForm){
    std::vector<Vector3> input;
    for (uint32_t i = 0; i <= 40; i++){
        const float x = float(i) * 0.5f;
        const float z = ((i / 10) % 2 == 0) ? float(i % 10) : float(10 - i % 10);
        input.push_back(Vector3(x, 0.0f, z));
    }
    std::vector<uint32_t> kept;
    simplifyIndices(input, 0.25f, kept);
    ASSERT_GE(kept.size(), 2u);
    EXPECT_EQ(kept.front(), 0u);
    EXPECT_EQ(kept.back(), 40u);
    for (size_t i = 1; i < kept.size(); i++){
        EXPECT_LT(kept[i - 1], kept[i]);
    }
    std::vector<Vector3> simplified;
    simplifyPolyline(input, 0.25f, simplified);
    ASSERT_EQ(simplified.size(), kept.size());
    for (size_t i = 0; i < kept.size(); i++){
        expectNearVector(simplified[i], input[kept[i]], 0.0f, "gathered position");
    }
    // within tolerance of the input everywhere
    for (size_t i = 0; i < input.size(); i++){
        float best = std::numeric_limits<float>::max();
        for (size_t k = 0; k + 1 < simplified.size(); k++){
            best = std::min(best, pointSegmentDistanceSq(input[i], simplified[k], simplified[k + 1]));
        }
        EXPECT_LE(std::sqrt(best), 0.25f + 1e-5f);
    }
}
