// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Tests/SplineStepTests.cpp, Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

// The Stepped grade of a wall (S18 R6): level tops per run, risers at the steps, the welded loop's
// seam, plumb rings and the step height. OpenEngine's walls mirrored z -> -z.

#include "SplineGeometryTestSupport.h"
#include "spline/SplineStep.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::spline;
using namespace jasperengine::spline::geometrytest;

namespace{

    constexpr float STEP_WALL_HEIGHT = 3.0f;
    constexpr float STEP_HALF_WIDTH = 0.3f;

    // a wall up a slope: 10 m north (-Z) climbing 1 m to a Linear corner, then 10 m east climbing
    // another metre; its two runs stand level at 1 m and 2 m, their tops at 4 m and 5 m
    const std::vector<Vector3> STEP_CLIMBING = {Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, -10.0f), Vector3(10.0f, 2.0f, -10.0f)};

    struct SteppedWall{
        std::vector<SplineStation> stations;
        SplineStripMesh mesh;
        float cornerDistance = 0.0f;
    };

    SteppedWall stepBuildWall(const std::vector<Vector3>& points, SplineCornerStyle style, float stepHeight = 0.0f){
        SteppedWall wall;
        Walk walk = walkPolyline(points, 0.5f, STEP_HALF_WIDTH);
        wall.cornerDistance = walk.stations[walk.corners[0].stationIndex].distance;
        (void)applyCorners(walk.stations, walk.corners, style, UP, false);
        const float pointDistances[] = {wall.cornerDistance};
        stepTopsAtPoints(walk.stations, pointDistances, UP, false, stepHeight);
        wall.stations = walk.stations;

        SplineStripParams params;
        params.uOriginMetres = 0.0f;
        params.generateCaps = false;
        params.heightVDatumMetres = 0.0f;
        wall.mesh = buildSplineStrip(wallProfile(2.0f * STEP_HALF_WIDTH, STEP_WALL_HEIGHT), wall.stations, params);
        return wall;
    }

    float stepTopOf(const SplineStripMesh& mesh, uint32_t r){
        float top = std::numeric_limits<float>::lowest();
        for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
            top = std::max(top, mesh.vertices[r * mesh.ringVertexCount + k].position.y);
        }
        return top;
    }

    float stepBottomOf(const SplineStripMesh& mesh, uint32_t r){
        float bottom = std::numeric_limits<float>::max();
        for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
            bottom = std::min(bottom, mesh.vertices[r * mesh.ringVertexCount + k].position.y);
        }
        return bottom;
    }

    // the riser vertices: whatever follows the rings when the caps are off
    std::vector<SplineVertex> stepRiserVertices(const SplineStripMesh& mesh){
        return {mesh.vertices.begin() + static_cast<std::ptrdiff_t>(mesh.ringCount) * mesh.ringVertexCount, mesh.vertices.end()};
    }

    // triangles among the riser vertices whose winding disagrees with their normal
    uint32_t stepMiswoundRiserTriangles(const SplineStripMesh& mesh){
        const uint32_t ringVertices = mesh.ringCount * mesh.ringVertexCount;
        uint32_t miswound = 0;
        for (size_t t = 0; t + 2u < mesh.indices.size(); t += 3u){
            if (mesh.indices[t] < ringVertices){
                continue;
            }
            const SplineVertex& a = mesh.vertices[mesh.indices[t]];
            const SplineVertex& b = mesh.vertices[mesh.indices[t + 1u]];
            const SplineVertex& c = mesh.vertices[mesh.indices[t + 2u]];
            if (dotOf(crossOf(b.position - a.position, c.position - a.position), a.normal) <= 0.0f){
                miswound++;
            }
        }
        return miswound;
    }

}

// Each run keeps a level top at the wall's height above its highest station; the mitre's Crease
// ring carries the step, which one riser closes, facing back toward the lower run, with V the
// height above the datum as on the side faces.
TEST(SplineStepTests, AMitredWallUpASlopeStepsItsLevelTopAtTheCorner){
    const SteppedWall wall = stepBuildWall(STEP_CLIMBING, SplineCornerStyle::Mitre);
    ASSERT_TRUE(wall.mesh.isValid());
    uint32_t creases = 0;
    for (uint32_t r = 0; r < wall.mesh.ringCount; r++){
        const SplineStation& station = wall.stations[r];
        const bool secondRun = station.distance > wall.cornerDistance + 1.0e-4f ||
                               (station.distance > wall.cornerDistance - 1.0e-4f && station.join == SplineStationJoin::Crease);
        creases += station.join == SplineStationJoin::Crease ? 1u : 0u;
        EXPECT_NEAR(stepTopOf(wall.mesh, r), secondRun ? 5.0f : 4.0f, 1.0e-4f) << "ring " << r;
        EXPECT_GE(stepTopOf(wall.mesh, r) - stepBottomOf(wall.mesh, r), STEP_WALL_HEIGHT - 1.0e-4f) << "ring " << r;
    }
    EXPECT_EQ(creases, 1u);

    const std::vector<SplineVertex> riser = stepRiserVertices(wall.mesh);
    ASSERT_EQ(riser.size(), 4u);
    uint32_t low = 0;
    uint32_t high = 0;
    for (const SplineVertex& v : riser){
        low += std::abs(v.position.y - 4.0f) < 1.0e-4f ? 1u : 0u;
        high += std::abs(v.position.y - 5.0f) < 1.0e-4f ? 1u : 0u;
        EXPECT_NEAR(v.uv.y, v.position.y, 1.0e-4f) << "riser V is the height above the datum";
        // back toward the first run, which runs north (-Z) into the corner
        EXPECT_GT(v.normal.z, 0.5f);
    }
    EXPECT_EQ(low, 2u);
    EXPECT_EQ(high, 2u);
    EXPECT_EQ(stepMiswoundRiserTriangles(wall.mesh), 0u);
    expectWellFormed(wall.mesh, "stepped mitred wall");
}

// walking the same wall downhill, the first run is the higher and the riser faces ahead, down onto
// the lower run
TEST(SplineStepTests, AWallDownASlopeStepsDownFacingTheLowerRun){
    const std::vector<Vector3> descending(STEP_CLIMBING.rbegin(), STEP_CLIMBING.rend());
    const SteppedWall wall = stepBuildWall(descending, SplineCornerStyle::Mitre);
    ASSERT_TRUE(wall.mesh.isValid());
    EXPECT_NEAR(stepTopOf(wall.mesh, 0u), 5.0f, 1.0e-4f);
    EXPECT_NEAR(stepTopOf(wall.mesh, wall.mesh.ringCount - 1u), 4.0f, 1.0e-4f);
    const std::vector<SplineVertex> riser = stepRiserVertices(wall.mesh);
    ASSERT_EQ(riser.size(), 4u);
    // the second run runs south (+Z) away from the corner
    for (const SplineVertex& v : riser){
        EXPECT_GT(v.normal.z, 0.5f);
    }
    EXPECT_EQ(stepMiswoundRiserTriangles(wall.mesh), 0u);
    expectWellFormed(wall.mesh, "stepped wall downhill");
}

// a round corner's fan stands at the higher level, and a Crease copy of its first ring at the lower
// level carries the step
TEST(SplineStepTests, ARoundCornersStepStandsOnACreaseCopyOfTheFan){
    const SteppedWall wall = stepBuildWall(STEP_CLIMBING, SplineCornerStyle::Round);
    ASSERT_TRUE(wall.mesh.isValid());
    uint32_t creases = 0;
    uint32_t fanRings = 0;
    for (uint32_t r = 0; r < wall.mesh.ringCount; r++){
        const SplineStation& station = wall.stations[r];
        creases += station.join == SplineStationJoin::Crease ? 1u : 0u;
        if (station.join == SplineStationJoin::Pivot){
            fanRings++;
            EXPECT_NEAR(stepTopOf(wall.mesh, r), 5.0f, 1.0e-4f) << "ring " << r;
        }
    }
    EXPECT_EQ(creases, 1u);
    EXPECT_GT(fanRings, 0u);
    EXPECT_EQ(stepRiserVertices(wall.mesh).size(), 4u);
    EXPECT_EQ(stepMiswoundRiserTriangles(wall.mesh), 0u);
}

// A welded loop whose first and last runs stand apart steps at its seam: a Crease copy of the last
// ring at the first run's level closes it, meeting the first ring exactly. This loop's first run is
// level at 0 m and the other three at 1 m: two risers.
TEST(SplineStepTests, AWeldedLoopStepsAtItsSeam){
    Walk walk = walkPolyline({Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, -10.0f), Vector3(10.0f, 1.0f, -10.0f),
                              Vector3(10.0f, 1.0f, 0.0f), Vector3(0.0f, 0.0f, 0.0f)}, 0.5f, STEP_HALF_WIDTH);
    std::vector<float> pointDistances;
    for (const SplineCornerSite& site : walk.corners){
        pointDistances.push_back(walk.stations[site.stationIndex].distance);
    }
    walk.corners.insert(walk.corners.begin(), SplineCornerSite{0u, 0u});
    closeStationLoop(walk.stations, UP);
    (void)applyCorners(walk.stations, walk.corners, SplineCornerStyle::Mitre, UP, true);
    stepTopsAtPoints(walk.stations, pointDistances, UP, true);

    const SplineStation& seam = walk.stations.back();
    EXPECT_EQ(seam.join, SplineStationJoin::Crease);
    SplineStripParams params;
    params.uOriginMetres = 0.0f;
    params.generateCaps = false;
    const SplineStripMesh mesh = buildSplineStrip(wallProfile(2.0f * STEP_HALF_WIDTH, STEP_WALL_HEIGHT), walk.stations, params);
    ASSERT_TRUE(mesh.isValid());
    const uint32_t lastRing = (mesh.ringCount - 1u) * mesh.ringVertexCount;
    for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
        EXPECT_TRUE(bitsEqual(mesh.vertices[k].position, mesh.vertices[lastRing + k].position)) << "slot " << k;
    }
    EXPECT_NEAR(stepTopOf(mesh, 0u), STEP_WALL_HEIGHT, 1.0e-4f);
    EXPECT_NEAR(stepTopOf(mesh, mesh.ringCount - 2u), 1.0f + STEP_WALL_HEIGHT, 1.0e-4f);
    EXPECT_EQ(stepRiserVertices(mesh).size(), 8u);
    EXPECT_EQ(stepMiswoundRiserTriangles(mesh), 0u);
    expectWellFormed(mesh, "stepped loop");
}

// a stepped wall stands plumb: on a steep climb every top vertex stands straight above a base vertex
TEST(SplineStepTests, ASteppedWallOnASteepClimbStandsPlumb){
    const SteppedWall wall = stepBuildWall({Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 3.0f, -5.0f), Vector3(5.0f, 5.0f, -5.0f)},
                                           SplineCornerStyle::Mitre);
    ASSERT_TRUE(wall.mesh.isValid());
    float worstShift = 0.0f;
    for (uint32_t r = 0; r < wall.mesh.ringCount; r++){
        const float bottom = stepBottomOf(wall.mesh, r);
        for (uint32_t k = 0; k < wall.mesh.ringVertexCount; k++){
            const Vector3& top = wall.mesh.vertices[r * wall.mesh.ringVertexCount + k].position;
            if (top.y - bottom < 1.0f){
                continue;
            }
            float nearest = 1.0e9f;
            for (uint32_t m = 0; m < wall.mesh.ringVertexCount; m++){
                const Vector3& base = wall.mesh.vertices[r * wall.mesh.ringVertexCount + m].position;
                if (top.y - base.y < 1.0f){
                    continue;
                }
                nearest = std::min(nearest, std::hypot(top.x - base.x, top.z - base.z));
            }
            worstShift = std::max(worstShift, nearest);
        }
    }
    EXPECT_LT(worstShift, 1.0e-3f);
    EXPECT_EQ(stepMiswoundRiserTriangles(wall.mesh), 0u);
}

// A step height rounds each run's level up to whole courses: levels 1 m and 2 m in courses of
// 0.75 m stand at 1.5 m and 2.25 m, so the tops are 4.5 m and 5.25 m and the step is one course.
TEST(SplineStepTests, AStepHeightRoundsEachRunsLevelUpToWholeCourses){
    const SteppedWall wall = stepBuildWall(STEP_CLIMBING, SplineCornerStyle::Mitre, 0.75f);
    ASSERT_TRUE(wall.mesh.isValid());
    EXPECT_NEAR(stepTopOf(wall.mesh, 0u), 4.5f, 1.0e-4f);
    EXPECT_NEAR(stepTopOf(wall.mesh, wall.mesh.ringCount - 1u), 5.25f, 1.0e-4f);
    for (uint32_t r = 0; r < wall.mesh.ringCount; r++){
        // no stretch of either run stands lower than the wall's height above its ground
        EXPECT_GE(stepTopOf(wall.mesh, r) - stepBottomOf(wall.mesh, r), STEP_WALL_HEIGHT - 1.0e-4f) << "ring " << r;
    }
    EXPECT_EQ(stepRiserVertices(wall.mesh).size(), 4u);

    // two runs within one course of each other share a level: no step, no riser
    const SteppedWall gentle = stepBuildWall({Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.1f, -10.0f), Vector3(10.0f, 0.3f, -10.0f)},
                                             SplineCornerStyle::Mitre, 0.75f);
    ASSERT_TRUE(gentle.mesh.isValid());
    EXPECT_TRUE(stepRiserVertices(gentle.mesh).empty());
    EXPECT_NEAR(stepTopOf(gentle.mesh, 0u), 0.75f + STEP_WALL_HEIGHT, 1.0e-4f);
    EXPECT_NEAR(stepTopOf(gentle.mesh, gentle.mesh.ringCount - 1u), 0.75f + STEP_WALL_HEIGHT, 1.0e-4f);
}
