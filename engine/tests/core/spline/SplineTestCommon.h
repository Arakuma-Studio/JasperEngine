// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Helpers shared by the spline core unit tests: spline builders, a seeded random source that
// draws the same numbers on every standard library, and vector comparisons.

#ifndef SPLINE_TEST_COMMON_H
#define SPLINE_TEST_COMMON_H

#include "spline/SplineData.h"
#include "spline/SplineEvaluator.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <vector>

namespace jasperengine{ namespace test{

    inline constexpr float SPLINE_TEST_PI = 3.14159265358979323846f;

    // knots at positions, the other fields default (radius 2)
    inline spline::SplineData makeSpline(spline::SplineType type, std::initializer_list<Vector3> positions,
                                         bool closed = false, float radius = 2.0f){
        std::vector<spline::SplineKnot> knots;
        for (const Vector3& p : positions){
            spline::SplineKnot k;
            k.position = p;
            k.radius = radius;
            knots.push_back(k);
        }
        spline::SplineData s;
        s.setType(type);
        s.setClosed(closed);
        s.setKnots(knots);
        return s;
    }

    inline spline::SplineData makeSpline(spline::SplineType type, const std::vector<Vector3>& positions,
                                         bool closed = false, float radius = 2.0f){
        std::vector<spline::SplineKnot> knots;
        for (const Vector3& p : positions){
            spline::SplineKnot k;
            k.position = p;
            k.radius = radius;
            knots.push_back(k);
        }
        spline::SplineData s;
        s.setType(type);
        s.setClosed(closed);
        s.setKnots(knots);
        return s;
    }

    // count knots on a circle of radius r around the origin in XZ (counter-clockwise seen from +Y)
    inline std::vector<Vector3> circleKnots(uint32_t count, float r, float y = 0.0f){
        std::vector<Vector3> out;
        for (uint32_t i = 0; i < count; i++){
            const float a = 2.0f * SPLINE_TEST_PI * float(i) / float(count);
            out.push_back(Vector3(r * std::cos(a), y, -r * std::sin(a)));
        }
        return out;
    }

    // A closed Bezier circle of four Aligned knots (handle 0.5523 r).
    inline spline::SplineData bezierCircle(float r){
        const float h = 0.5522847498f * r;
        spline::SplineData s;
        s.setType(spline::SplineType::CubicBezier);
        s.setClosed(true);
        std::vector<spline::SplineKnot> knots(4);
        const Vector3 positions[4] = {Vector3(r, 0, 0), Vector3(0, 0, -r), Vector3(-r, 0, 0), Vector3(0, 0, r)};
        const Vector3 directions[4] = {Vector3(0, 0, -1), Vector3(-1, 0, 0), Vector3(0, 0, 1), Vector3(1, 0, 0)};
        for (int i = 0; i < 4; i++){
            knots[i].position = positions[i];
            knots[i].tangentOut = directions[i] * h;
            knots[i].tangentIn = directions[i] * -h;
            knots[i].mode = spline::TangentMode::Aligned;
        }
        s.setKnots(knots);
        return s;
    }

    // std::mt19937's sequence is fixed by the standard, its distributions are not: floats are
    // made from the raw 32-bit draws here
    class SplineTestRandom{
    public:
        explicit SplineTestRandom(uint32_t seed): engine(seed) {}

        // in [lo, hi)
        float uniform(float lo, float hi){
            return lo + (hi - lo) * (float(engine() >> 8) * (1.0f / 16777216.0f));
        }

        // standard normal (Box-Muller)
        float gaussian(){
            const float u1 = (float(engine() >> 8) + 1.0f) * (1.0f / 16777217.0f);
            const float u2 = float(engine() >> 8) * (1.0f / 16777216.0f);
            return std::sqrt(-2.0f * std::log(u1)) * std::cos(2.0f * SPLINE_TEST_PI * u2);
        }

    private:
        std::mt19937 engine;
    };

    inline float splineTestDot(const Vector3& a, const Vector3& b){
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    inline Vector3 splineTestCross(const Vector3& a, const Vector3& b){
        return Vector3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    }

    inline float splineTestLength(const Vector3& v){
        return std::sqrt(splineTestDot(v, v));
    }

    inline void expectNearVector(const Vector3& a, const Vector3& b, float tolerance, const char* what = ""){
        EXPECT_NEAR(a.x, b.x, tolerance) << what;
        EXPECT_NEAR(a.y, b.y, tolerance) << what;
        EXPECT_NEAR(a.z, b.z, tolerance) << what;
    }

    inline void expectOrthonormal(const Vector3& f, const Vector3& r, const Vector3& u, float tolerance = 1e-3f){
        EXPECT_TRUE(std::isfinite(f.x) && std::isfinite(f.y) && std::isfinite(f.z));
        EXPECT_TRUE(std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.z));
        EXPECT_TRUE(std::isfinite(u.x) && std::isfinite(u.y) && std::isfinite(u.z));
        EXPECT_NEAR(splineTestDot(f, f), 1.0f, tolerance);
        EXPECT_NEAR(splineTestDot(r, r), 1.0f, tolerance);
        EXPECT_NEAR(splineTestDot(u, u), 1.0f, tolerance);
        EXPECT_NEAR(splineTestDot(f, r), 0.0f, tolerance);
        EXPECT_NEAR(splineTestDot(f, u), 0.0f, tolerance);
        EXPECT_NEAR(splineTestDot(r, u), 0.0f, tolerance);
    }

}}

#endif // SPLINE_TEST_COMMON_H
