// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Stations (S18 D5, R9): knots as stations, the spacing bound, the turn and drape subdivisions,
// conform with holds across probe misses, the draped length, 2D, the transform option, the
// half-texel resample and the segment-nearest XZ query (linear and gridded).

#include "SplineTestCommon.h"

#include "spline/SplineStations.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::spline;
using namespace jasperengine::test;

namespace{

    void stationsExpectAscending(const std::vector<SplineStation>& stations){
        for (size_t i = 1; i < stations.size(); i++){
            EXPECT_GT(stations[i].distance, stations[i - 1].distance) << "station " << i;
        }
    }

    void stationsExpectFrames(const std::vector<SplineStation>& stations){
        for (const SplineStation& st : stations){
            expectOrthonormal(st.forward, st.right, st.up, 1e-4f);
        }
    }

    SplineSurfaceProbe stationsTerrain(float (*height)(float x, float z)){
        return [height](const Vector3& p){
            SplineSurfaceSample s;
            s.hit = true;
            s.height = height(p.x, p.z);
            return s;
        };
    }

    float stationsSineHeight(float x, float){
        return 2.0f * std::sin(x) + 1.0f;
    }

    float stationsRampHeight(float x, float){
        return 0.5f * x;
    }

}

TEST(SplineStationsTests, EveryKnotIsAStationAndTheSpacingIsBounded){
    const std::vector<Vector3> knots = {Vector3(0, 0, 0), Vector3(0.4f, 0, 0.1f), Vector3(7, 1, -2), Vector3(15, 0, 3), Vector3(16, 0, 9)};
    const SplineData s = makeSpline(SplineType::CatmullRom, knots);
    std::vector<SplineStation> stations;
    SplineStationOptions options;
    options.maxTurnDeg = 180.0f;
    buildStations(s, options, stations);
    ASSERT_GE(stations.size(), 2u);
    for (size_t k = 0; k < knots.size(); k++){
        const auto it = std::find_if(stations.begin(), stations.end(), [&](const SplineStation& st){ return st.splineT == float(k); });
        ASSERT_NE(it, stations.end()) << "knot " << k;
        expectNearVector(it->position, knots[k], 0.0f, "knot position");
    }
    stationsExpectAscending(stations);
    stationsExpectFrames(stations);
    for (size_t i = 1; i < stations.size(); i++){
        const float chord = splineTestLength(stations[i].position - stations[i - 1].position);
        EXPECT_LE(chord, 1.0f + 1e-4f);
        // undraped, the distance is the 3D polyline length
        EXPECT_NEAR(stations[i].distance - stations[i - 1].distance, chord, 1e-5f);
        EXPECT_GT(stations[i].splineT, stations[i - 1].splineT);
    }
    EXPECT_NEAR(stations.back().distance, length(s), 0.002f * length(s));
    EXPECT_EQ(stations.front().distance, 0.0f);
}

TEST(SplineStationsTests, TurnsAddStations){
    const SplineData s = makeSpline(SplineType::CatmullRom, circleKnots(8, 2.0f), true);
    std::vector<SplineStation> stations;
    buildStations(s, SplineStationOptions(), stations);
    // the 1-unit spacing alone would give about length + 8
    EXPECT_GT(float(stations.size()), 2.0f * length(s));
    for (size_t i = 1; i < stations.size(); i++){
        const float c = std::clamp(splineTestDot(stations[i].forward, stations[i - 1].forward), -1.0f, 1.0f);
        EXPECT_LT(std::acos(c) * 180.0f / SPLINE_TEST_PI, 4.5f) << "station " << i;
    }
    // the loop closes on its first station, with its frame
    expectNearVector(stations.back().position, stations.front().position, 1e-5f);
    expectNearVector(stations.back().right, stations.front().right, 0.0f);
    expectNearVector(stations.back().forward, stations.front().forward, 0.0f);
    EXPECT_NEAR(stations.back().distance, length(s), 0.002f * length(s));
}

TEST(SplineStationsTests, ConformFollowsTheProbeAndSubdividesTheDrape){
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 5, 0), Vector3(20, 5, 0)});
    SplineStationOptions options;
    options.conform = SplineConform::Height;
    options.probe = stationsTerrain(stationsSineHeight);
    options.verticalOffset = 0.25f;
    std::vector<SplineStation> stations;
    const SplineStationReport report = buildStations(s, options, stations);
    EXPECT_EQ(report.probeMisses, 0u);
    EXPECT_FALSE(report.allMissed);
    EXPECT_GT(stations.size(), 21u) << "the drape added stations where the ground bends";
    for (const SplineStation& st : stations){
        EXPECT_NEAR(st.position.y, stationsSineHeight(st.position.x, 0.0f) + 0.25f, 1e-5f);
    }
    // the polyline stays within the height tolerance of the ground between stations
    for (size_t i = 1; i < stations.size(); i++){
        const float x = 0.5f * (stations[i].position.x + stations[i - 1].position.x);
        const float chord = 0.5f * (stations[i].position.y + stations[i - 1].position.y);
        EXPECT_LT(std::fabs(stationsSineHeight(x, 0.0f) + 0.25f - chord), 0.1f + 1e-4f) << "station " << i;
    }
    stationsExpectAscending(stations);
    stationsExpectFrames(stations);
}

TEST(SplineStationsTests, DistanceIsTheDrapedLength){
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(20, 0, 0)});
    SplineStationOptions options;
    options.conform = SplineConform::Height;
    options.probe = stationsTerrain(stationsRampHeight);
    std::vector<SplineStation> stations;
    buildStations(s, options, stations);
    EXPECT_NEAR(stations.back().distance, 20.0f * std::sqrt(1.25f), 1e-3f);
    // forward climbs the ramp; right stays level
    const SplineStation& mid = stations[stations.size() / 2];
    expectNearVector(mid.forward, Vector3(2, 1, 0) * (1.0f / std::sqrt(5.0f)), 1e-5f);
    expectNearVector(mid.right, Vector3(0, 0, 1), 1e-5f);
}

TEST(SplineStationsTests, MissesHoldTheNearestMeasuredHeight){
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 9, 0), Vector3(20, 9, 0)});
    SplineStationOptions options;
    options.conform = SplineConform::Height;
    options.probe = [](const Vector3& p){
        SplineSurfaceSample sample;
        sample.hit = p.x >= 2.5f && !(p.x > 8.5f && p.x < 12.5f);
        sample.height = 0.1f * p.x;
        return sample;
    };
    std::vector<SplineStation> stations;
    const SplineStationReport report = buildStations(s, options, stations);
    EXPECT_GT(report.probeMisses, 0u);
    EXPECT_FALSE(report.allMissed);
    for (const SplineStation& st : stations){
        const float x = st.position.x;
        if (x < 2.5f){
            // a leading gap takes the first measured height after it
            EXPECT_NEAR(st.position.y, 0.3f, 1e-5f) << x;
        }else if (x > 8.5f && x < 12.5f){
            // an inner gap holds the last measured height before it
            EXPECT_NEAR(st.position.y, 0.8f, 1e-5f) << x;
        }else{
            EXPECT_NEAR(st.position.y, 0.1f * x, 1e-5f) << x;
        }
    }
    stationsExpectAscending(stations);
}

TEST(SplineStationsTests, AllMissesKeepTheAuthoredHeights){
    const SplineData s = makeSpline(SplineType::CatmullRom, {Vector3(0, 3, 0), Vector3(5, 4, 1), Vector3(10, 2, 0)});
    SplineStationOptions options;
    options.conform = SplineConform::HeightAndSlope;
    options.probe = [](const Vector3&){ return SplineSurfaceSample(); };
    std::vector<SplineStation> stations;
    const SplineStationReport report = buildStations(s, options, stations);
    EXPECT_TRUE(report.allMissed);
    EXPECT_EQ(report.probeMisses, uint32_t(stations.size()));
    for (const SplineStation& st : stations){
        const uint32_t seg = std::min(uint32_t(st.splineT), s.segmentCount() - 1);
        expectNearVector(st.position, evaluateSegment(s, seg, st.splineT - float(seg)).position, 1e-5f);
    }
}

TEST(SplineStationsTests, HeightAndSlopeTiltsUpTowardTheSurface){
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(10, 0, 0)});
    SplineStationOptions options;
    options.conform = SplineConform::HeightAndSlope;
    options.probe = [](const Vector3& p){
        SplineSurfaceSample sample;
        sample.hit = true;
        sample.height = 0.5f * p.z;
        // pointing down: the up hemisphere is taken
        sample.normal = Vector3(0, -1, 0.5f) * (1.0f / std::sqrt(1.25f));
        return sample;
    };
    std::vector<SplineStation> stations;
    buildStations(s, options, stations);
    const Vector3 normal = Vector3(0, 1, -0.5f) * (1.0f / std::sqrt(1.25f));
    for (const SplineStation& st : stations){
        expectNearVector(st.up, normal, 1e-5f);
        expectNearVector(st.forward, Vector3(1, 0, 0), 1e-6f);
        expectOrthonormal(st.forward, st.right, st.up, 1e-5f);
    }
    options.slopeBlend = 0.0f;
    buildStations(s, options, stations);
    expectNearVector(stations[1].up, Vector3(0, 1, 0), 1e-6f);
}

TEST(SplineStationsTests, ChannelsAndWidthOptions){
    SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(4, 0, 0)});
    s.setKnotRadius(0, 1.0f);
    s.setKnotRadius(1, 3.0f);
    s.setKnotRoll(0, 0.0f);
    s.setKnotRoll(1, 90.0f);
    std::vector<SplineStation> stations;
    buildStations(s, SplineStationOptions(), stations);
    ASSERT_EQ(stations.size(), 5u);
    EXPECT_NEAR(stations[2].halfWidthLeft, 2.0f, 1e-5f);
    EXPECT_EQ(stations[2].halfWidthLeft, stations[2].halfWidthRight);
    EXPECT_NEAR(stations[2].rollRadians, 0.25f * SPLINE_TEST_PI, 1e-5f);
    EXPECT_NEAR(stations[4].rollRadians, 0.5f * SPLINE_TEST_PI, 1e-6f);
    EXPECT_NEAR(stations[2].splineT, 0.5f, 1e-5f);
    // the frames stay roll-free
    expectNearVector(stations[2].right, Vector3(0, 0, 1), 1e-6f);

    SplineStationOptions constant;
    constant.constantWidth = true;
    constant.halfWidth = 0.75f;
    buildStations(s, constant, stations);
    for (const SplineStation& st : stations){
        EXPECT_EQ(st.halfWidthLeft, 0.75f);
        EXPECT_EQ(st.halfWidthRight, 0.75f);
    }
}

TEST(SplineStationsTests, TheTransformAppliesBeforeTheConform){
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(5, 0, 0)}, false, 1.5f);
    const Matrix4 m = Matrix4::translateMatrix(Vector3(100, 7, -3)) * Matrix4::scaleMatrix(Vector3(2, 2, 2));
    SplineStationOptions options;
    options.localToWorld = &m;
    options.conform = SplineConform::Height;
    // the probe sees world points
    options.probe = [](const Vector3& p){
        SplineSurfaceSample sample;
        sample.hit = p.x >= 100.0f;
        sample.height = 1.0f;
        return sample;
    };
    std::vector<SplineStation> stations;
    const SplineStationReport report = buildStations(s, options, stations);
    EXPECT_EQ(report.probeMisses, 0u);
    expectNearVector(stations.front().position, Vector3(100, 1, -3), 1e-5f);
    expectNearVector(stations.back().position, Vector3(110, 1, -3), 1e-4f);
    EXPECT_NEAR(stations.back().distance, 10.0f, 1e-4f);
    EXPECT_NEAR(stations.front().halfWidthLeft, 3.0f, 1e-6f);
    // spacing is along the local curve
    EXPECT_EQ(stations.size(), 6u);
}

TEST(SplineStationsTests, TwoDimensionalStationsUseUpAxisZ){
    SplineData s = makeSpline(SplineType::CatmullRom, {Vector3(0, 0, 0), Vector3(5, 3, 0), Vector3(10, -1, 0), Vector3(14, 2, 0)});
    s.setUpAxis(Vector3(0, 0, 1));
    std::vector<SplineStation> stations;
    buildStations(s, SplineStationOptions(), stations);
    ASSERT_GT(stations.size(), 10u);
    for (const SplineStation& st : stations){
        EXPECT_EQ(st.position.z, 0.0f);
        EXPECT_NEAR(st.forward.z, 0.0f, 1e-6f);
        EXPECT_NEAR(st.right.z, 0.0f, 1e-6f);
        expectNearVector(st.up, Vector3(0, 0, 1), 1e-5f);
    }
    stationsExpectAscending(stations);
}

TEST(SplineStationsTests, VerticalRunsKeepAContinuousRight){
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(10, 0, 0), Vector3(10, 10, 0), Vector3(20, 10, 0)});
    std::vector<SplineStation> stations;
    buildStations(s, SplineStationOptions(), stations);
    stationsExpectFrames(stations);
    for (size_t i = 1; i < stations.size(); i++){
        EXPECT_GT(splineTestDot(stations[i].right, stations[i - 1].right), 0.99f) << "station " << i;
    }
    // straight up from the start: the first defined right carried back
    const SplineData up = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(0, 10, 0), Vector3(10, 10, 0)});
    buildStations(up, SplineStationOptions(), stations);
    stationsExpectFrames(stations);
    expectNearVector(stations.front().right, Vector3(0, 0, 1), 1e-5f);
}

TEST(SplineStationsTests, CoincidentKnotsGiveOneStation){
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 0, 0), Vector3(3, 0, 0), Vector3(3, 0, 0), Vector3(6, 0, 0)});
    std::vector<SplineStation> stations;
    buildStations(s, SplineStationOptions(), stations);
    stationsExpectAscending(stations);
    EXPECT_EQ(stations.size(), 7u);
    // the station at x = 3 is the later knot's
    EXPECT_EQ(stations[3].splineT, 2.0f);
    EXPECT_TRUE(std::isfinite(stations[3].forward.x));
}

TEST(SplineStationsTests, InvalidSplinesGiveNoStations){
    std::vector<SplineStation> stations(3);
    buildStations(SplineData(), SplineStationOptions(), stations);
    EXPECT_TRUE(stations.empty());
}

TEST(SplineStationsTests, ResampleBoundsTheSpacingAndKeepsTheStations){
    const SplineData s = makeSpline(SplineType::CatmullRom, {Vector3(0, 0, 0), Vector3(6, 1, 3), Vector3(12, 0, -2)});
    std::vector<SplineStation> stations;
    buildStations(s, SplineStationOptions(), stations);
    std::vector<SplineStation> fine;
    resampleStations(stations, 0.25f, fine);
    stationsExpectAscending(fine);
    stationsExpectFrames(fine);
    for (size_t i = 1; i < fine.size(); i++){
        EXPECT_LE(fine[i].distance - fine[i - 1].distance, 0.25f + 1e-5f);
    }
    size_t found = 0;
    for (const SplineStation& st : fine){
        if (st.splineT >= 0.0f){
            found++;
        }
    }
    EXPECT_EQ(found, stations.size()) << "every original station is kept, the inserted ones carry splineT -1";
    EXPECT_EQ(fine.back().distance, stations.back().distance);

    // the spacing never goes below SPLINE_MIN_STATION_SPACING
    resampleStations(stations, 0.001f, fine);
    const float total = stations.back().distance;
    EXPECT_LE(float(fine.size()), total / SPLINE_MIN_STATION_SPACING + float(stations.size()) + 1.0f);
}

TEST(SplineStationsTests, ClosestSegmentReportsLateralHeightAndDistance){
    const SplineData s = makeSpline(SplineType::Linear, {Vector3(0, 2, 0), Vector3(10, 4, 0)});
    std::vector<SplineStation> stations;
    buildStations(s, SplineStationOptions(), stations);
    const SplineStationHitXZ right = closestStationSegmentXZ(stations, 5.0f, 2.0f);
    ASSERT_TRUE(right.valid);
    EXPECT_NEAR(right.distanceXZ, 2.0f, 1e-5f);
    // +Z is the right of +X travel
    EXPECT_NEAR(right.lateral, 2.0f, 1e-5f);
    EXPECT_NEAR(right.height, 3.0f, 1e-5f);
    EXPECT_NEAR(right.distance, 0.5f * stations.back().distance, 1e-4f);
    EXPECT_NEAR(right.halfWidthLeft, 2.0f, 1e-6f);
    const SplineStationHitXZ left = closestStationSegmentXZ(stations, 5.0f, -1.0f);
    EXPECT_NEAR(left.lateral, -1.0f, 1e-5f);
    // past the end the projection clamps
    const SplineStationHitXZ past = closestStationSegmentXZ(stations, 13.0f, 4.0f);
    EXPECT_NEAR(past.distanceXZ, 5.0f, 1e-5f);
    EXPECT_NEAR(past.height, 4.0f, 1e-5f);
    EXPECT_FALSE(closestStationSegmentXZ(std::span<const SplineStation>(), 0.0f, 0.0f).valid);
}

TEST(SplineStationsTests, TheGridAgreesWithTheLinearQuery){
    std::vector<Vector3> knots;
    for (int i = 0; i < 12; i++){
        const float a = float(i) * 0.9f;
        knots.push_back(Vector3(a * 8.0f * std::cos(a), std::sin(a), a * 8.0f * std::sin(a)));
    }
    const SplineData s = makeSpline(SplineType::CatmullRom, knots);
    std::vector<SplineStation> stations;
    buildStations(s, SplineStationOptions(), stations);
    for (float cellSize : {0.0f, 0.5f, 7.0f}){
        SplineStationGridXZ grid;
        grid.build(stations, cellSize);
        SplineTestRandom random(91);
        for (int q = 0; q < 2000; q++){
            const float x = random.uniform(-150, 150);
            const float z = random.uniform(-150, 150);
            const SplineStationHitXZ a = closestStationSegmentXZ(stations, x, z);
            const SplineStationHitXZ b = grid.closest(x, z);
            ASSERT_TRUE(b.valid);
            EXPECT_EQ(a.segment, b.segment) << "query " << q;
            EXPECT_EQ(a.distanceXZ, b.distanceXZ);
            EXPECT_EQ(a.height, b.height);
            EXPECT_EQ(a.lateral, b.lateral);
        }
        // far queries still find the polyline; a search radius can rule it out
        EXPECT_TRUE(grid.closest(5000.0f, -4000.0f).valid);
        EXPECT_FALSE(grid.closest(5000.0f, -4000.0f, 10.0f).valid);
    }
    SplineStationGridXZ empty;
    empty.build(std::span<const SplineStation>());
    EXPECT_FALSE(empty.closest(0.0f, 0.0f).valid);
}
