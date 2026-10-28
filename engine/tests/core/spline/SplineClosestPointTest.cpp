// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Closest point and signed distances (S18 R3, AC2): seeded random queries per type against a
// brute force over 20k samples, the XZ variants, and the inside of closed splines.

#include "SplineTestCommon.h"

#include "spline/SplineUtility.h"

#include <algorithm>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::spline;
using namespace jasperengine::test;

namespace{

    const uint32_t CLOSEST_BRUTE_SAMPLES = 20000;
    const uint32_t CLOSEST_QUERIES = 1000;

    // The curve as a polyline of CLOSEST_BRUTE_SAMPLES chords (uniform in t). Distances to it are
    // within the chords' sagitta (~1e-5 here) of the curve's.
    std::vector<Vector3> closestDensePolyline(const SplineData& s){
        std::vector<Vector3> out;
        out.reserve(CLOSEST_BRUTE_SAMPLES + 1);
        for (uint32_t i = 0; i <= CLOSEST_BRUTE_SAMPLES; i++){
            out.push_back(evaluatePosition(s, float(i) / float(CLOSEST_BRUTE_SAMPLES)));
        }
        return out;
    }

    float closestBruteForce(const std::vector<Vector3>& dense, Vector3 p, bool xz){
        float best = std::numeric_limits<float>::max();
        for (size_t i = 0; i + 1 < dense.size(); i++){
            Vector3 a = dense[i];
            Vector3 b = dense[i + 1];
            if (xz){
                a.y = 0.0f;
                b.y = 0.0f;
                p.y = 0.0f;
            }
            best = std::min(best, pointSegmentDistanceSq(p, a, b));
        }
        return std::sqrt(best);
    }

    SplineData closestCase(SplineType type, bool closed){
        const std::vector<Vector3> knots = {
            Vector3(0, 0, 0), Vector3(8, 3, 2), Vector3(12, -1, 11), Vector3(4, 2, 16),
            Vector3(-3, 5, 9), Vector3(-9, 0, 14), Vector3(-14, -2, 3), Vector3(-6, 1, -6),
        };
        SplineData s = makeSpline(type, knots, closed);
        if (type == SplineType::CubicBezier){
            // handles of every length and direction, some Broken
            SplineTestRandom random(77);
            for (size_t i = 0; i < knots.size(); i++){
                const Vector3 out(random.uniform(-5, 5), random.uniform(-2, 2), random.uniform(-5, 5));
                if (i % 3 == 0){
                    s.setKnotMode(i, TangentMode::Broken);
                    s.setKnotTangents(i, Vector3(random.uniform(-3, 3), 0, random.uniform(-3, 3)), out);
                }else{
                    s.setKnotTangentOut(i, out);
                }
            }
        }
        return s;
    }

    void closestCheckAgainstBruteForce(SplineType type, bool closed, uint32_t seed){
        const SplineData s = closestCase(type, closed);
        const std::vector<Vector3> dense = closestDensePolyline(s);
        SplineTestRandom random(seed);
        for (uint32_t q = 0; q < CLOSEST_QUERIES; q++){
            const Vector3 p(random.uniform(-20, 18), random.uniform(-6, 9), random.uniform(-10, 21));
            const SplineHit hit = closestPoint(s, p);
            EXPECT_NEAR(hit.distance, closestBruteForce(dense, p, false), 1e-3f) << "query " << q;
            // the hit is a point of the curve at its own t, and the distance is to it
            expectNearVector(evaluatePosition(s, hit.t), hit.position, 1e-3f, "position at t");
            EXPECT_NEAR(splineTestLength(hit.position - p), hit.distance, 1e-4f);
            EXPECT_LT(hit.segment, s.segmentCount());

            const SplineHit xz = closestPointXZ(s, p.x, p.z);
            EXPECT_NEAR(xz.distance, closestBruteForce(dense, p, true), 1e-3f) << "XZ query " << q;
        }
    }

    // a point of the curve offset sideways in XZ by offset (positive toward centre)
    Vector3 closestOffsetToward(const SplineData& s, float t, const Vector3& centre, float offset){
        const SplineFrame f = evaluate(s, t);
        Vector3 toward = centre - f.position;
        toward.y = 0.0f;
        Vector3 side(-f.forward.z, 0.0f, f.forward.x);
        side = side * (1.0f / splineTestLength(side));
        if (splineTestDot(side, toward) < 0.0f){
            side = -side;
        }
        return f.position + side * offset;
    }

}

TEST(SplineClosestPointTests, LinearAgreesWithBruteForce){
    closestCheckAgainstBruteForce(SplineType::Linear, false, 1);
    closestCheckAgainstBruteForce(SplineType::Linear, true, 2);
}

TEST(SplineClosestPointTests, CatmullRomAgreesWithBruteForce){
    closestCheckAgainstBruteForce(SplineType::CatmullRom, false, 3);
    closestCheckAgainstBruteForce(SplineType::CatmullRom, true, 4);
}

TEST(SplineClosestPointTests, CubicBezierAgreesWithBruteForce){
    closestCheckAgainstBruteForce(SplineType::CubicBezier, false, 5);
    closestCheckAgainstBruteForce(SplineType::CubicBezier, true, 6);
}

TEST(SplineClosestPointTests, ManyKnotsAgreeWithBruteForce){
    // the broad phase culls most segments of a 100-knot spline; it must never cull the nearest
    std::vector<Vector3> knots;
    SplineTestRandom random(11);
    for (int i = 0; i < 100; i++){
        const float a = float(i) * 0.31f;
        knots.push_back(Vector3(a * 6.0f * std::cos(a), random.uniform(-1, 1), a * 6.0f * std::sin(a)));
    }
    const SplineData s = makeSpline(SplineType::CatmullRom, knots);
    const std::vector<Vector3> dense = closestDensePolyline(s);
    for (uint32_t q = 0; q < 200; q++){
        const Vector3 p(random.uniform(-200, 200), random.uniform(-5, 5), random.uniform(-200, 200));
        EXPECT_NEAR(closestPoint(s, p).distance, closestBruteForce(dense, p, false), 2e-3f) << "query " << q;
    }
}

TEST(SplineClosestPointTests, XZVariantIgnoresAltitude){
    SplineData s = closestCase(SplineType::CatmullRom, false);
    SplineData raised = s;
    std::vector<SplineKnot> knots = s.knots();
    for (SplineKnot& k : knots){
        k.position.y += 100.0f;
    }
    raised.setKnots(knots);
    SplineTestRandom random(21);
    for (int q = 0; q < 200; q++){
        const float x = random.uniform(-20, 18);
        const float z = random.uniform(-10, 21);
        const SplineHit a = closestPointXZ(s, x, z);
        const SplineHit b = closestPointXZ(raised, x, z);
        EXPECT_NEAR(a.distance, b.distance, 1e-4f);
        EXPECT_NEAR(a.t, b.t, 1e-4f);
        // the reported point is the 3D curve's, carrying its height
        EXPECT_NEAR(b.position.y - a.position.y, 100.0f, 1e-3f);
        EXPECT_NEAR(signedDistanceXZ(s, x, z), signedDistanceXZ(raised, x, z), 1e-4f);
    }
}

TEST(SplineClosestPointTests, ClosedSquareCentreIsInsideTheAreaAndOutsideTheBand){
    for (SplineType type : {SplineType::Linear, SplineType::CatmullRom, SplineType::CubicBezier}){
        const SplineData s = makeSpline(type, {Vector3(-10, 0, -10), Vector3(10, 0, -10), Vector3(10, 0, 10), Vector3(-10, 0, 10)}, true, 2.0f);
        EXPECT_LT(signedDistanceXZ(s, 0.0f, 0.0f), 0.0f) << "type " << int(type);
        EXPECT_GT(signedDistanceBandXZ(s, 0.0f, 0.0f), 0.0f) << "type " << int(type);
        // on an open spline the area and the band agree
        SplineData open = s;
        open.setClosed(false);
        EXPECT_EQ(signedDistanceXZ(open, 0.0f, 0.0f), signedDistanceBandXZ(open, 0.0f, 0.0f));
    }
}

TEST(SplineClosestPointTests, InsideIsNegativeRightUpToTheCurve){
    // radius 0, so the sign is the inside test alone, probed 0.01 either side of the curve where
    // the sampled polygon cannot tell; both orientations, every type
    for (SplineType type : {SplineType::Linear, SplineType::CatmullRom, SplineType::CubicBezier}){
        for (bool reversed : {false, true}){
            std::vector<Vector3> knots = circleKnots(7, 10.0f);
            if (reversed){
                std::reverse(knots.begin(), knots.end());
            }
            SplineData s = makeSpline(type, knots, true, 0.0f);
            if (type == SplineType::CubicBezier){
                for (size_t i = 0; i < knots.size(); i++){
                    const Vector3& prev = knots[(i + knots.size() - 1) % knots.size()];
                    const Vector3& next = knots[(i + 1) % knots.size()];
                    s.setKnotTangentOut(i, (next - prev) * (1.0f / 6.0f));
                }
            }
            for (int i = 0; i < 360; i++){
                const float t = (float(i) + 0.37f) / 360.0f;
                const Vector3 in = closestOffsetToward(s, t, Vector3(0, 0, 0), 0.01f);
                const Vector3 out = closestOffsetToward(s, t, Vector3(0, 0, 0), -0.01f);
                EXPECT_LT(signedDistanceXZ(s, in.x, in.z), 0.0f) << "type " << int(type) << " t " << t;
                EXPECT_GT(signedDistanceXZ(s, out.x, out.z), 0.0f) << "type " << int(type) << " t " << t;
            }
            EXPECT_LT(signedDistanceXZ(s, 0.0f, 0.0f), -9.0f);
            EXPECT_GT(signedDistanceXZ(s, 30.0f, 0.0f), 19.0f);
        }
    }
}

TEST(SplineClosestPointTests, SignedDistanceMeasuresTheSweptTube){
    SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(10, 0, 0)});
    s.setKnotRadius(0, 1.0f);
    s.setKnotRadius(1, 3.0f);
    // radius 2 at the middle
    EXPECT_NEAR(signedDistance(s, Vector3(5, 0, 1)), -1.0f, 1e-4f);
    EXPECT_NEAR(signedDistance(s, Vector3(5, 3, 4)), 3.0f, 1e-4f);
    // a closed spline's 3D tube is still a tube, never filled
    const SplineData ring = makeSpline(SplineType::CatmullRom, circleKnots(8, 10.0f), true, 1.0f);
    EXPECT_GT(signedDistance(ring, Vector3(0, 0, 0)), 8.0f);
    EXPECT_LT(signedDistanceXZ(ring, 0.0f, 0.0f), 0.0f);
}

TEST(SplineClosestPointTests, BandDistanceIsARing){
    const SplineData ring = makeSpline(SplineType::CatmullRom, circleKnots(8, 10.0f), true, 2.0f);
    SplineTestRandom random(31);
    for (int q = 0; q < 300; q++){
        const float x = random.uniform(-15, 15);
        const float z = random.uniform(-15, 15);
        const float band = signedDistanceBandXZ(ring, x, z);
        EXPECT_NEAR(band, closestPointXZ(ring, x, z).distance - 2.0f, 1e-5f);
        const float area = signedDistanceXZ(ring, x, z);
        // inside the band both agree in sign; outside the band they differ only inside the loop
        if (band < 0.0f){
            EXPECT_LT(area, 0.0f);
        }
    }
}
