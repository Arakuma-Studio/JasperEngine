// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The spline cache: never read stale (S18 R5), within 4 KB + 64 B per knot (R22), allocation
// free on the query paths, and safe to query from several threads.

#include "SplineTestCommon.h"

#include "support/AllocationCounter.h"

#include <thread>

using namespace jasperengine;
using namespace jasperengine::spline;
using namespace jasperengine::test;

namespace{

    std::vector<Vector3> cacheWiggle(uint32_t count){
        std::vector<Vector3> out;
        for (uint32_t i = 0; i < count; i++){
            const float x = float(i) * 3.0f;
            out.push_back(Vector3(x, std::sin(x * 0.3f), 4.0f * std::cos(x * 0.17f)));
        }
        return out;
    }

}

TEST(SplineCacheTests, QueriesOnAnEditedSplineNeverReadTheStaleCache){
    SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(10, 0, 0)});
    EXPECT_NEAR(length(s), 10.0f, 1e-5f);
    s.setKnotPosition(1, Vector3(20, 0, 0));
    // no rebuild call: the query sees the new version
    EXPECT_NEAR(length(s), 20.0f, 1e-5f);
    EXPECT_NEAR(evaluateAtDistance(s, 15.0f).position.x, 15.0f, 1e-4f);
    EXPECT_NEAR(closestPoint(s, Vector3(18, 3, 0)).position.x, 18.0f, 1e-4f);

    SplineKnot k;
    k.position = Vector3(20, 0, 20);
    s.addKnot(k);
    EXPECT_EQ(s.ensureCache().segmentCount, 2u);
    EXPECT_NEAR(length(s), 40.0f, 1e-4f);
    s.setClosed(true);
    EXPECT_EQ(s.ensureCache().segmentCount, 3u);
    EXPECT_EQ(s.ensureCache().bounds.size(), 3u);
    EXPECT_FALSE(s.ensureCache().polygonXZ.empty());
    EXPECT_LT(signedDistanceXZ(s, 15.0f, 5.0f), 0.0f);
    s.setType(SplineType::CatmullRom);
    EXPECT_GT(length(s), 40.0f + std::sqrt(800.0f));

    const uint64_t before = s.version();
    s.markDirty();
    EXPECT_GT(s.version(), before);
}

TEST(SplineCacheTests, TheCacheRebuildsOncePerVersion){
    SplineData s = makeSpline(SplineType::CatmullRom, cacheWiggle(6));
    const SplineCache* first = &s.ensureCache();
    const std::vector<float> arc = first->arc;
    EXPECT_EQ(&s.ensureCache(), first);
    EXPECT_EQ(s.ensureCache().arc, arc);
    s.setKnotPosition(2, Vector3(6, 5, 0));
    EXPECT_NE(s.ensureCache().arc, arc);
}

TEST(SplineCacheTests, CopiesKeepTheirOwnAnswers){
    SplineData original = makeSpline(SplineType::CatmullRom, cacheWiggle(5));
    const float before = length(original);
    const SplineData copy = original;
    EXPECT_EQ(copy.version(), original.version());
    original.setKnotPosition(4, Vector3(100, 0, 0));
    EXPECT_NE(length(original), before);
    EXPECT_EQ(length(copy), before);

    // an assignment is a new definition: the target's old cache is not reused
    SplineData target = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(1, 0, 0)});
    EXPECT_NEAR(length(target), 1.0f, 1e-6f);
    target = copy;
    EXPECT_EQ(length(target), before);
}

TEST(SplineCacheTests, CacheStaysWithinTheBudget){
    for (uint32_t count : {2u, 3u, 4u, 10u, 33u, 100u, 257u, 1000u, 4000u}){
        for (bool closed : {false, true}){
            for (SplineType type : {SplineType::Linear, SplineType::CatmullRom, SplineType::CubicBezier}){
                const SplineData s = makeSpline(type, cacheWiggle(count), closed);
                const size_t budget = 4096 + 64 * size_t(count);
                EXPECT_LE(s.cacheBytes(), budget) << count << " knots, closed " << closed;
                EXPECT_GE(s.ensureCache().arcSteps, 1u);
                EXPECT_LE(s.ensureCache().arcSteps, 32u);
            }
        }
    }
}

TEST(SplineCacheTests, ShortSplinesGetTheFinestArcResolution){
    EXPECT_EQ(makeSpline(SplineType::CatmullRom, cacheWiggle(4)).ensureCache().arcSteps, 32u);
    // 100 knots still sample every segment several times
    EXPECT_GE(makeSpline(SplineType::CatmullRom, cacheWiggle(100)).ensureCache().arcSteps, 8u);
    EXPECT_GE(makeSpline(SplineType::CatmullRom, cacheWiggle(100), true).ensureCache().arcSteps, 4u);
}

TEST(SplineCacheTests, ShrinkingASplineReleasesItsCacheMemory){
    SplineData s = makeSpline(SplineType::CatmullRom, cacheWiggle(2000), true);
    EXPECT_LE(s.cacheBytes(), 4096u + 64u * 2000u);
    s.setKnots({SplineKnot(), SplineKnot(), SplineKnot()});
    s.setKnotPosition(1, Vector3(1, 0, 0));
    s.setKnotPosition(2, Vector3(0, 0, 1));
    EXPECT_LE(s.cacheBytes(), 4096u + 64u * 3u);
}

TEST(SplineCacheTests, QueriesDoNotAllocate){
    const SplineData open = makeSpline(SplineType::CatmullRom, cacheWiggle(40));
    const SplineData ring = makeSpline(SplineType::CubicBezier, circleKnots(12, 9.0f), true);
    const SplineData vertical = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(0, 10, 0)});
    open.ensureCache();
    ring.ensureCache();
    vertical.ensureCache();
    float sink = 0.0f;
    testsupport::ThreadAllocationCount allocations;
    for (int i = 0; i < 100; i++){
        const float f = float(i) / 100.0f;
        for (const SplineData* s : {&open, &ring, &vertical}){
            sink += evaluate(*s, f).position.x;
            sink += evaluateAtDistance(*s, f * length(*s)).right.z;
            sink += evaluatePosition(*s, f).y;
            sink += tToDistance(*s, f) + distanceToT(*s, f * 10.0f);
            sink += closestPoint(*s, Vector3(f * 50.0f, 1.0f, 3.0f)).distance;
            sink += closestPointXZ(*s, f * 50.0f, -2.0f).distance;
            sink += signedDistance(*s, Vector3(f * 20.0f, 0.0f, 1.0f));
            sink += signedDistanceXZ(*s, f * 20.0f, 1.0f);
            sink += signedDistanceBandXZ(*s, f * 20.0f, 1.0f);
        }
    }
    EXPECT_EQ(allocations.count(), 0u);
    EXPECT_TRUE(std::isfinite(sink));
}

TEST(SplineCacheTests, ConcurrentQueriesOnAStaleSplineAgree){
    SplineData s = makeSpline(SplineType::CatmullRom, cacheWiggle(30));
    s.setKnotPosition(3, Vector3(9, 4, 4));
    const SplineData reference = s;
    std::vector<float> expected;
    for (int i = 0; i < 64; i++){
        expected.push_back(evaluateAtDistance(reference, float(i)).position.z);
    }
    // s's cache is stale: every thread races to the first query
    std::vector<std::vector<float>> results(4);
    std::vector<std::thread> threads;
    for (size_t t = 0; t < results.size(); t++){
        threads.emplace_back([&s, &results, t](){
            for (int i = 0; i < 64; i++){
                results[t].push_back(evaluateAtDistance(s, float(i)).position.z);
            }
        });
    }
    for (std::thread& thread : threads){
        thread.join();
    }
    for (const std::vector<float>& r : results){
        EXPECT_EQ(r, expected);
    }
}
