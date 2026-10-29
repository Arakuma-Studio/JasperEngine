// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Tests/SplineStripBuilderTests.cpp,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

// The swept strip (S18 R6, AC5): OpenEngine's cases mirrored z -> -z (SplineGeometryTestSupport.h),
// plus the well-formedness of every mesh, world-metre U, the closed-loop seam, chunking (R30), the
// flow encoding of a Channel river (D9), offsets and the tube.

#include "SplineGeometryTestSupport.h"
#include "AllocationCounter.h"

#include <chrono>
#include <cstdio>

#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::spline;
using namespace jasperengine::spline::geometrytest;

namespace{

    using StripHeightField = std::function<float(float x, float along)>;

    // Walks a draped centreline by TRUE 3D length, one roll-free station per sample, the run going
    // -Z (`along` metres from the start is z = -along). The frame is racked: forward follows the
    // draped grade and right is level, forward x up.
    std::vector<SplineStation> stripDrapedStations(const StripHeightField& height, float fromAlong, float toAlong,
                                                   uint32_t count, float halfWidth){
        std::vector<Vector3> points(count);
        for (uint32_t i = 0; i < count; i++){
            const float t = static_cast<float>(i) / static_cast<float>(count - 1u);
            const float along = fromAlong + (toAlong - fromAlong) * t;
            points[i] = Vector3(0.0f, height(0.0f, along), -along);
        }

        std::vector<SplineStation> stations(count);
        float travelled = 0.0f;
        for (uint32_t i = 0; i < count; i++){
            if (i > 0u){
                travelled += lengthOf(points[i] - points[i - 1u]);
            }
            const Vector3 ahead = points[std::min(i + 1u, count - 1u)];
            const Vector3 behind = points[i == 0u ? 0u : i - 1u];
            const Vector3 forward = unit(ahead - behind);
            SplineStation& s = stations[i];
            s.position = points[i];
            s.forward = forward;
            s.right = rightOf(forward);
            s.up = crossOf(s.right, forward);
            s.halfWidthLeft = halfWidth;
            s.halfWidthRight = halfWidth;
            s.distance = travelled;
        }
        return stations;
    }

    std::vector<SplineStation> stripStraightStations(uint32_t count, float step, float halfWidth){
        return stripDrapedStations([](float, float){ return 0.0f; }, 0.0f, step * static_cast<float>(count - 1u), count, halfWidth);
    }

    // stations around a level circle of `radius` about the origin, starting at (radius, 0, 0) and
    // turning right all the way round, closed: the last station is the first again
    std::vector<SplineStation> stripCircleStations(float radius, uint32_t segments, float halfWidth){
        std::vector<SplineStation> stations(segments + 1u);
        for (uint32_t i = 0; i <= segments; i++){
            const double angle = 2.0 * static_cast<double>(PI) * static_cast<double>(i) / static_cast<double>(segments);
            SplineStation& s = stations[i];
            s.position = Vector3(static_cast<float>(radius * std::cos(angle)), 0.0f, static_cast<float>(radius * std::sin(angle)));
            s.forward = Vector3(static_cast<float>(-std::sin(angle)), 0.0f, static_cast<float>(std::cos(angle)));
            s.right = rightOf(s.forward);
            s.up = crossOf(s.right, s.forward);
            s.halfWidthLeft = halfWidth;
            s.halfWidthRight = halfWidth;
            s.distance = static_cast<float>(radius * angle);
        }
        closeStationLoop(stations, UP);
        return stations;
    }

    SplineProfile stripWall(float thickness, float height){
        return wallProfile(thickness, height);
    }

    SplineProfile stripCrownRoad(){
        SplineProfileParams p;
        p.shape = SplineProfileShape::Crown;
        p.width = 7.0f;
        return buildProfile(p);
    }

    Vector3 stripTriangleNormal(const SplineStripMesh& mesh, size_t triangle){
        const Vector3& a = mesh.vertices[mesh.indices[triangle * 3u + 0u]].position;
        const Vector3& b = mesh.vertices[mesh.indices[triangle * 3u + 1u]].position;
        const Vector3& c = mesh.vertices[mesh.indices[triangle * 3u + 2u]].position;
        return crossOf(b - a, c - a);
    }

#if defined(__x86_64__) || defined(_M_X64)
    // FNV-1a over the mesh's raw bytes: one ULP anywhere changes it
    uint64_t stripMeshDigest(const SplineStripMesh& mesh){
        uint64_t value = 1469598103934665603ull;
        const auto fold = [&value](const void* data, size_t size){
            const auto* p = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < size; i++){
                value ^= static_cast<uint64_t>(p[i]);
                value *= 1099511628211ull;
            }
        };
        fold(mesh.vertices.data(), mesh.vertices.size() * sizeof(SplineVertex));
        fold(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));
        const Vector3 minimum = mesh.bounds.getMinimum();
        const Vector3 maximum = mesh.bounds.getMaximum();
        fold(&minimum, sizeof(minimum));
        fold(&maximum, sizeof(maximum));
        return value;
    }

    // A capped wall and a crowned road over rolling ground with a width that changes. Polynomial
    // ground and no roll, so the input carries no libm rounding and the digest is the builder's
    // arithmetic alone.
    uint64_t stripFrozenRunDigest(SplineProfileShape shape){
        SplineProfileParams profileParams;
        profileParams.shape = shape;
        profileParams.width = 1.3f;
        profileParams.thickness = 1.3f;
        profileParams.height = 2.7f;
        const SplineProfile profile = buildProfile(profileParams);

        std::vector<SplineStation> stations = stripDrapedStations([](float x, float along){
            const float t = along / 23.0f;
            return 1.6f * t * (1.0f - t) * (0.5f - t) + 0.05f * x;
        }, 0.0f, 23.0f, 47u, 0.65f);
        for (size_t i = 0; i < stations.size(); i++){
            const float t = static_cast<float>(i) / static_cast<float>(stations.size() - 1u);
            stations[i].halfWidthLeft = 0.65f + 0.2f * t;
            stations[i].halfWidthRight = 0.65f + 0.2f * t;
        }

        SplineStripParams params;
        params.tilesPerMetreU = 0.7f;
        params.tilesPerMetreV = 1.3f;
        return stripMeshDigest(buildSplineStrip(profile, stations, params));
    }
#endif

    // every chunk of a run, built the way the extrude system builds them
    std::vector<SplineStripMesh> stripBuildChunks(const SplineProfile& profile, const std::vector<SplineStation>& stations,
                                                  const SplineStripParams& params,
                                                  const std::vector<SplineStripChunkRange>& chunks){
        std::vector<SplineStripMesh> meshes(chunks.size());
        for (size_t c = 0; c < chunks.size(); c++){
            buildSplineStripChunk(profile, stations, chunks[c], params, meshes[c]);
        }
        return meshes;
    }

    // a winding road: polynomial curves in plan and in height, stations about every 0.8 m
    std::vector<SplineStation> stripWindingRoad(float length, float step){
        const uint32_t count = static_cast<uint32_t>(length / step) + 1u;
        std::vector<Vector3> points(count);
        for (uint32_t i = 0; i < count; i++){
            const float along = step * static_cast<float>(i);
            const float t = along / length;
            points[i] = Vector3(40.0f * t * t * (1.0f - t) * 3.0f, 6.0f * t * (1.0f - t), -along);
        }
        std::vector<SplineStation> stations(count);
        float travelled = 0.0f;
        for (uint32_t i = 0; i < count; i++){
            if (i > 0u){
                travelled += lengthOf(points[i] - points[i - 1u]);
            }
            const Vector3 forward = unit(points[std::min(i + 1u, count - 1u)] - points[i == 0u ? 0u : i - 1u]);
            stations[i].position = points[i];
            stations[i].forward = forward;
            stations[i].right = rightOf(forward);
            stations[i].up = crossOf(stations[i].right, forward);
            stations[i].halfWidthLeft = 4.0f;
            stations[i].halfWidthRight = 4.0f;
            stations[i].distance = travelled;
        }
        return stations;
    }

}

TEST(SplineStripBuilderTests, AdjacentStationsShareTheirVerticesSoThereIsNoSeamToMisalign){
    const SplineProfile profile = flatRibbon(2.0f);
    const auto stations = stripStraightStations(8u, 1.0f, 1.0f);

    const SplineStripMesh mesh = buildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.isValid());
    EXPECT_EQ(mesh.ringCount, 8u);
    // an open two-point profile splits both ends (one adjacent edge each): two vertices a ring
    EXPECT_EQ(mesh.ringVertexCount, 2u);
    // no cap vertices for an open profile: the vertex buffer is rings and nothing else
    EXPECT_EQ(mesh.vertices.size(), static_cast<size_t>(mesh.ringCount) * mesh.ringVertexCount);

    // every interior ring is referenced by triangles on BOTH sides: that is the weld
    std::vector<int> behind(mesh.ringCount, 0);
    std::vector<int> ahead(mesh.ringCount, 0);
    for (size_t t = 0; t < mesh.indices.size() / 3u; t++){
        uint32_t lowRing = std::numeric_limits<uint32_t>::max();
        uint32_t highRing = 0u;
        for (uint32_t k = 0; k < 3u; k++){
            const uint32_t ring = mesh.indices[t * 3u + k] / mesh.ringVertexCount;
            lowRing = std::min(lowRing, ring);
            highRing = std::max(highRing, ring);
        }
        ASSERT_EQ(highRing, lowRing + 1u) << "a triangle spans more than one station interval";
        ahead[lowRing] += 1;
        behind[highRing] += 1;
    }
    for (uint32_t r = 1u; r + 1u < mesh.ringCount; r++){
        EXPECT_GT(behind[r], 0) << "ring " << r << " is not welded to the ring behind it";
        EXPECT_GT(ahead[r], 0) << "ring " << r << " is not welded to the ring ahead of it";
    }
    expectWellFormed(mesh, "ribbon");
}

TEST(SplineStripBuilderTests, AFlatRibbonOnLevelGroundFacesTheSky){
    const SplineProfile profile = flatRibbon(3.0f);
    const auto stations = stripStraightStations(5u, 2.0f, 1.5f);

    const SplineStripMesh mesh = buildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.isValid());
    ASSERT_GT(mesh.indices.size(), 0u);
    for (size_t t = 0; t < mesh.indices.size() / 3u; t++){
        EXPECT_GT(stripTriangleNormal(mesh, t).y, 0.0f) << "triangle " << t << " faces the ground";
    }
    for (const SplineVertex& v : mesh.vertices){
        EXPECT_NEAR(v.normal.y, 1.0f, 1e-5f);
    }
    expectWellFormed(mesh, "level ribbon");
}

TEST(SplineStripBuilderTests, AWallFacesOutwardOnEverySide){
    const SplineProfile profile = stripWall(1.0f, 3.0f);
    const auto stations = stripStraightStations(4u, 2.0f, 0.5f);

    SplineStripParams params;
    params.generateCaps = false; // side faces only, so "outward" means lateral
    const SplineStripMesh mesh = buildSplineStrip(profile, stations, params);

    ASSERT_TRUE(mesh.isValid());
    const Vector3 centre(0.0f, 1.5f, 0.0f);
    for (size_t t = 0; t < mesh.indices.size() / 3u; t++){
        const Vector3 n = stripTriangleNormal(mesh, t);
        const Vector3& a = mesh.vertices[mesh.indices[t * 3u + 0u]].position;
        const Vector3& b = mesh.vertices[mesh.indices[t * 3u + 1u]].position;
        const Vector3& c = mesh.vertices[mesh.indices[t * 3u + 2u]].position;
        const Vector3 mid = (a + b + c) * (1.0f / 3.0f);
        // compared in the cross-section plane only: the wall is long in z
        EXPECT_GT(n.x * (mid.x - centre.x) + n.y * (mid.y - centre.y), 0.0f) << "wall triangle " << t << " faces into the solid";
    }
    expectWellFormed(mesh, "wall without caps");
}

TEST(SplineStripBuilderTests, UIsArcLengthInMetresAndStrictlyIncreasesAlongTheRun){
    const SplineProfile profile = flatRibbon(2.0f);
    // a descent makes authored arc length and draped length differ
    const auto stations = stripDrapedStations([](float, float along){ return -0.5f * along; }, 0.0f, 20.0f, 21u, 1.0f);

    SplineStripParams params;
    params.tilesPerMetreU = 0.25f;
    const SplineStripMesh mesh = buildSplineStrip(profile, stations, params);

    ASSERT_TRUE(mesh.isValid());
    ASSERT_EQ(mesh.ringCount, 21u);
    for (uint32_t r = 1u; r < mesh.ringCount; r++){
        const float previous = mesh.vertices[(r - 1u) * mesh.ringVertexCount].uv.x;
        const float current = mesh.vertices[r * mesh.ringVertexCount].uv.x;
        EXPECT_GT(current, previous) << "U went backwards at ring " << r;
    }

    // U is the DRAPED distance: over a 20 m run at a 1:2 grade the ground distance is 20 sqrt(1.25)
    const float expectedLength = 20.0f * std::sqrt(1.25f);
    const float finalU = mesh.vertices[(mesh.ringCount - 1u) * mesh.ringVertexCount].uv.x;
    EXPECT_NEAR(finalU, expectedLength * 0.25f, 1e-3f);
    EXPECT_NEAR(mesh.vertices[0].uv.x, 0.0f, 1e-6f);
}

TEST(SplineStripBuilderTests, UIsTheDrapedDistanceTimesTheTileDensityWithinATenThousandth){
    // the draped length measured independently, in double, along an uneven slope
    const uint32_t count = 61u;
    std::vector<Vector3> points(count);
    for (uint32_t i = 0; i < count; i++){
        const float along = 0.5f * static_cast<float>(i);
        points[i] = Vector3(0.0f, 0.02f * along * along - 0.3f * along, -along);
    }
    std::vector<double> draped(count, 0.0);
    for (uint32_t i = 1; i < count; i++){
        const double dy = static_cast<double>(points[i].y) - points[i - 1u].y;
        const double dz = static_cast<double>(points[i].z) - points[i - 1u].z;
        draped[i] = draped[i - 1u] + std::sqrt(dy * dy + dz * dz);
    }
    std::vector<SplineStation> stations(count);
    for (uint32_t i = 0; i < count; i++){
        const Vector3 forward = unit(points[std::min(i + 1u, count - 1u)] - points[i == 0u ? 0u : i - 1u]);
        stations[i].position = points[i];
        stations[i].forward = forward;
        stations[i].right = rightOf(forward);
        stations[i].up = crossOf(stations[i].right, forward);
        stations[i].distance = static_cast<float>(draped[i]);
    }

    SplineStripParams params;
    params.tilesPerMetreU = 0.37f;
    const SplineStripMesh mesh = buildSplineStrip(stripCrownRoad(), stations, params);
    ASSERT_TRUE(mesh.isValid());
    ASSERT_EQ(mesh.ringCount, count);
    for (uint32_t r = 0; r < mesh.ringCount; r++){
        for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
            EXPECT_NEAR(mesh.vertices[r * mesh.ringVertexCount + k].uv.x, draped[r] * 0.37, 1e-4) << "ring " << r << " slot " << k;
        }
    }
    expectWellFormed(mesh, "crown road on a slope");
}

TEST(SplineStripBuilderTests, VSpansTheProfilePerimeterInMetresAcrossTheStrip){
    const SplineProfile profile = flatRibbon(3.0f);
    const auto stations = stripStraightStations(3u, 1.0f, 1.5f);

    SplineStripParams params;
    params.tilesPerMetreV = 0.5f;
    const SplineStripMesh mesh = buildSplineStrip(profile, stations, params);

    ASSERT_TRUE(mesh.isValid());
    ASSERT_EQ(mesh.ringVertexCount, 2u);
    EXPECT_NEAR(mesh.vertices[0].uv.y, 0.0f, 1e-6f);
    // a 3 m wide ribbon crosses 3 m of perimeter, tiled at 0.5 per metre
    EXPECT_NEAR(mesh.vertices[1].uv.y, 1.5f, 1e-5f);
}

TEST(SplineStripBuilderTests, VFollowsTheWidthChannelSoATaperKeepsItsTexelDensity){
    const SplineProfile profile = flatRibbon(2.0f); // nominal half-width 1 m
    auto stations = stripStraightStations(2u, 4.0f, 1.0f);
    stations[1].halfWidthLeft = 3.0f; // taper out to a 6 m ribbon
    stations[1].halfWidthRight = 3.0f;

    const SplineStripMesh mesh = buildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.isValid());
    EXPECT_NEAR(mesh.vertices[1].uv.y, 2.0f, 1e-5f);
    EXPECT_NEAR(mesh.vertices[mesh.ringVertexCount + 1u].uv.y, 6.0f, 1e-5f);
}

TEST(SplineStripBuilderTests, TheWidthChannelScalesLaterallyWithoutChangingHeight){
    const SplineProfile profile = stripWall(2.0f, 4.0f); // nominal half-width 1 m
    auto stations = stripStraightStations(2u, 2.0f, 1.0f);
    stations[1].halfWidthLeft = 2.5f;
    stations[1].halfWidthRight = 2.5f;

    SplineStripParams params;
    params.widthScale = SplineProfileScale::LateralOnly;
    const SplineStripMesh mesh = buildSplineStrip(profile, stations, params);

    ASSERT_TRUE(mesh.isValid());
    const auto ringExtent = [&](uint32_t ring, bool vertical){
        float lo = std::numeric_limits<float>::max();
        float hi = std::numeric_limits<float>::lowest();
        for (uint32_t s = 0; s < mesh.ringVertexCount; s++){
            const Vector3& p = mesh.vertices[ring * mesh.ringVertexCount + s].position;
            const float value = vertical ? p.y : p.x;
            lo = std::min(lo, value);
            hi = std::max(hi, value);
        }
        return hi - lo;
    };

    EXPECT_NEAR(ringExtent(0u, false), 2.0f, 1e-5f);
    EXPECT_NEAR(ringExtent(1u, false), 5.0f, 1e-5f); // a road that widens...
    EXPECT_NEAR(ringExtent(0u, true), 4.0f, 1e-5f);
    EXPECT_NEAR(ringExtent(1u, true), 4.0f, 1e-5f);  // ...does not get taller
}

TEST(SplineStripBuilderTests, UniformScaleRaisesARampartThatWidens){
    const SplineProfile profile = stripWall(2.0f, 4.0f);
    auto stations = stripStraightStations(2u, 2.0f, 1.0f);
    stations[1].halfWidthLeft = 2.0f;
    stations[1].halfWidthRight = 2.0f;

    SplineStripParams params;
    params.widthScale = SplineProfileScale::Uniform;
    const SplineStripMesh mesh = buildSplineStrip(profile, stations, params);

    ASSERT_TRUE(mesh.isValid());
    float tallest = 0.0f;
    for (uint32_t s = 0; s < mesh.ringVertexCount; s++){
        tallest = std::max(tallest, mesh.vertices[mesh.ringVertexCount + s].position.y);
    }
    EXPECT_NEAR(tallest, 8.0f, 1e-5f);
    expectWellFormed(mesh, "widening rampart");
}

TEST(SplineStripBuilderTests, RollBanksTheCrossSectionAboutTravel){
    const SplineProfile profile = flatRibbon(2.0f);
    auto stations = stripStraightStations(2u, 2.0f, 1.0f);
    stations[0].rollRadians = PI * 0.5f;
    stations[1].rollRadians = PI * 0.5f;

    const SplineStripMesh mesh = buildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.isValid());
    // a quarter turn about forward stands the ribbon on edge, its right edge raised (the
    // evaluator's convention: right * cos + up * sin)
    EXPECT_NEAR(mesh.vertices[0].position.x, 0.0f, 1e-5f);
    EXPECT_NEAR(mesh.vertices[0].position.y, -1.0f, 1e-5f);
    EXPECT_NEAR(mesh.vertices[1].position.y, 1.0f, 1e-5f);
    expectWellFormed(mesh, "banked ribbon");
}

TEST(SplineStripBuilderTests, ClosedProfilesCapAndOpenOnesDoNot){
    const auto stations = stripStraightStations(3u, 1.0f, 0.5f);

    const SplineStripMesh wall = buildSplineStrip(stripWall(1.0f, 2.0f), stations, {});
    const SplineStripMesh path = buildSplineStrip(flatRibbon(2.0f), stations, {});

    ASSERT_TRUE(wall.isValid());
    ASSERT_TRUE(path.isValid());
    // caps are DERIVED from the profile: capping an open strip would emit a degenerate sliver
    EXPECT_GT(wall.vertices.size(), static_cast<size_t>(wall.ringCount) * wall.ringVertexCount);
    EXPECT_EQ(path.vertices.size(), static_cast<size_t>(path.ringCount) * path.ringVertexCount);

    // the two caps face opposite ways, and away from the strip
    bool sawBack = false;
    bool sawFront = false;
    for (const SplineVertex& v : wall.vertices){
        sawBack = sawBack || v.normal.z < -0.99f;
        sawFront = sawFront || v.normal.z > 0.99f;
    }
    EXPECT_TRUE(sawBack);
    EXPECT_TRUE(sawFront);
    expectWellFormed(wall, "capped wall");
}

TEST(SplineStripBuilderTests, EveryRingVertexOfAClosedProfileIsReferencedByTheStitch){
    // the wrap edge's V seam rides point 0's incoming slot when the point is hard: a duplicate slot
    // there would be an orphaned vertex per ring
    SplineStripParams params;
    params.generateCaps = false;
    const SplineStripMesh wall = buildSplineStrip(stripWall(1.0f, 2.0f), stripStraightStations(3u, 1.0f, 0.5f), params);

    ASSERT_TRUE(wall.isValid());
    // four hard corners split into exactly eight slots; the seam adds no ninth
    EXPECT_EQ(wall.ringVertexCount, 8u);
    std::vector<bool> referenced(wall.vertices.size(), false);
    for (uint32_t index : wall.indices){
        referenced[index] = true;
    }
    for (size_t v = 0; v < referenced.size(); v++){
        EXPECT_TRUE(referenced[v]) << "vertex " << v << " is orphaned";
    }
}

TEST(SplineStripBuilderTests, CapUVsTileAtTheSidesDensityOnTheScaledCrossSection){
    // a wall authored at nominal half-width 1 m on stations of half-width 2 m: the built
    // cross-section is twice the authored one
    const auto stations = stripStraightStations(3u, 1.0f, 2.0f);
    SplineStripParams params;
    params.tilesPerMetreV = 0.5f;
    const SplineStripMesh wall = buildSplineStrip(stripWall(2.0f, 4.0f), stations, params);

    ASSERT_TRUE(wall.isValid());
    const size_t ringVerts = static_cast<size_t>(wall.ringCount) * wall.ringVertexCount;
    ASSERT_GT(wall.vertices.size(), ringVerts) << "no cap vertices were generated";
    float maxU = 0.0f;
    for (size_t v = ringVerts; v < wall.vertices.size(); v++){
        maxU = std::max(maxU, std::abs(wall.vertices[v].uv.x));
    }
    // the scaled lateral extent is +/-2 m, tiled at 0.5 tiles per metre
    EXPECT_NEAR(maxU, 1.0f, 1e-5f);
}

TEST(SplineStripBuilderTests, TheClosedLoopSnapRoundsTheRunToAWholeNumberOfTiles){
    const SplineProfile profile = flatRibbon(2.0f);
    const auto stations = stripStraightStations(11u, 1.03f, 1.0f); // 10.3 m run

    SplineStripParams params;
    params.tilesPerMetreU = 1.0f;
    params.snapUForClosedLoop = true;
    const SplineStripMesh mesh = buildSplineStrip(profile, stations, params);

    ASSERT_TRUE(mesh.isValid());
    const float finalU = mesh.vertices[(mesh.ringCount - 1u) * mesh.ringVertexCount].uv.x;
    EXPECT_NEAR(finalU, std::round(finalU), 1e-4f);
    EXPECT_NEAR(finalU, 10.0f, 1e-4f);
}

TEST(SplineStripBuilderTests, ChunksSplitFromOneRunShareTheirBoundaryU){
    // one run cut into spans that SHARE their boundary station, each carrying its GLOBAL distance
    const SplineProfile profile = flatRibbon(2.0f);
    const auto stations = stripStraightStations(11u, 1.03f, 1.0f);
    const std::span<const SplineStation> frontSpan(stations.data(), 6u);
    const std::span<const SplineStation> backSpan(stations.data() + 5u, 6u);

    SplineStripParams params;
    params.tilesPerMetreU = 1.0f;
    params.uOriginMetres = 0.0f; // one U axis across every chunk of the run

    const SplineStripMesh front = buildSplineStrip(profile, frontSpan, params);
    const SplineStripMesh back = buildSplineStrip(profile, backSpan, params);

    ASSERT_TRUE(front.isValid());
    ASSERT_TRUE(back.isValid());
    const float frontBoundaryU = front.vertices[(front.ringCount - 1u) * front.ringVertexCount].uv.x;
    const float backBoundaryU = back.vertices[0].uv.x;
    EXPECT_FLOAT_EQ(frontBoundaryU, backBoundaryU);
    EXPECT_GT(backBoundaryU, 0.0f) << "the back chunk restarted its U axis";

    // the default keeps the standalone behaviour: a span starting mid-run still starts at U 0
    const SplineStripMesh lone = buildSplineStrip(profile, backSpan, SplineStripParams{});
    ASSERT_TRUE(lone.isValid());
    EXPECT_FLOAT_EQ(lone.vertices[0].uv.x, 0.0f);
}

TEST(SplineStripBuilderTests, AChunkedClosedLoopSnapsItsTileDensityOnceOverTheWholeRun){
    const SplineProfile profile = flatRibbon(2.0f);
    const auto stations = stripStraightStations(11u, 1.03f, 1.0f); // 10.3 m run
    const float runLength = stations.back().distance;

    SplineStripParams params;
    params.tilesPerMetreU = std::round(runLength * 1.0f) / runLength; // 10 whole tiles
    params.snapUForClosedLoop = false;
    params.uOriginMetres = 0.0f;

    // deliberately UNEVEN chunks, so a per-chunk computation cannot hide in symmetry
    const std::span<const SplineStation> frontSpan(stations.data(), 4u);
    const std::span<const SplineStation> backSpan(stations.data() + 3u, 8u);
    const SplineStripMesh front = buildSplineStrip(profile, frontSpan, params);
    const SplineStripMesh back = buildSplineStrip(profile, backSpan, params);
    ASSERT_TRUE(front.isValid());
    ASSERT_TRUE(back.isValid());

    const float finalU = back.vertices[(back.ringCount - 1u) * back.ringVertexCount].uv.x;
    EXPECT_NEAR(finalU, 10.0f, 1e-4f);
    EXPECT_FLOAT_EQ(front.vertices[(front.ringCount - 1u) * front.ringVertexCount].uv.x, back.vertices[0].uv.x);
    const auto ringUStep = [](const SplineStripMesh& mesh, uint32_t r){
        return mesh.vertices[r * mesh.ringVertexCount].uv.x - mesh.vertices[(r - 1u) * mesh.ringVertexCount].uv.x;
    };
    for (uint32_t r = 1u; r < front.ringCount; r++){
        EXPECT_NEAR(ringUStep(front, r), 1.0f, 1e-3f) << "front chunk ring " << r;
    }
    for (uint32_t r = 1u; r < back.ringCount; r++){
        EXPECT_NEAR(ringUStep(back, r), 1.0f, 1e-3f) << "back chunk ring " << r;
    }
}

TEST(SplineStripBuilderTests, StationsThatDoNotAdvanceAreDroppedRatherThanWelded){
    const SplineProfile profile = flatRibbon(2.0f);
    auto stations = stripStraightStations(4u, 1.0f, 1.0f);
    stations[2].distance = stations[1].distance; // a doubled-back drape sample

    const SplineStripMesh mesh = buildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.isValid());
    EXPECT_EQ(mesh.droppedStations, 1u);
    EXPECT_EQ(mesh.ringCount, 3u);
}

TEST(SplineStripBuilderTests, TooFewUsableStationsProduceNoMeshRatherThanADegenerateOne){
    const SplineProfile profile = flatRibbon(2.0f);
    auto stations = stripStraightStations(2u, 1.0f, 1.0f);
    stations[1].position.y = std::nanf("");

    EXPECT_FALSE(buildSplineStrip(profile, stations, {}).isValid());
    EXPECT_FALSE(buildSplineStrip(profile, {}, {}).isValid());
    EXPECT_FALSE(buildSplineStrip(SplineProfile{}, stripStraightStations(4u, 1.0f, 1.0f), {}).isValid());
}

TEST(SplineStripBuilderTests, BoundsCoverEveryVertexAndStayFinite){
    const SplineProfile profile = stripWall(1.0f, 3.0f);
    const auto stations = stripDrapedStations([](float, float along){ return 0.2f * along; }, 0.0f, 10.0f, 11u, 0.5f);

    const SplineStripMesh mesh = buildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.isValid());
    const Vector3 minimum = mesh.bounds.getMinimum();
    const Vector3 maximum = mesh.bounds.getMaximum();
    for (const SplineVertex& v : mesh.vertices){
        EXPECT_GE(v.position.x, minimum.x);
        EXPECT_GE(v.position.y, minimum.y);
        EXPECT_GE(v.position.z, minimum.z);
        EXPECT_LE(v.position.x, maximum.x);
        EXPECT_LE(v.position.y, maximum.y);
        EXPECT_LE(v.position.z, maximum.z);
    }
    EXPECT_TRUE(std::isfinite(minimum.y));
    EXPECT_TRUE(std::isfinite(maximum.y));
    expectWellFormed(mesh, "wall up a slope");
}

TEST(SplineStripBuilderTests, TheStripFollowsAGroundStepWithinTolerance){
    // the strip tracks a step at its sampled stations rather than bridging it
    const auto step = [](float, float along){ return along < 10.0f ? 0.0f : 2.0f; };
    const SplineProfile profile = flatRibbon(2.0f);
    const auto stations = stripDrapedStations(step, 0.0f, 20.0f, 201u, 1.0f);

    const SplineStripMesh mesh = buildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.isValid());
    uint32_t offSurface = 0;
    for (const SplineVertex& v : mesh.vertices){
        if (std::abs(v.position.y - step(v.position.x, -v.position.z)) > 1e-4f){
            offSurface++;
        }
    }
    // only the interval straddling the riser can disagree, at its two rings
    EXPECT_LE(offSurface, 2u * mesh.ringVertexCount);
}

TEST(SplineStripBuilderTests, OnASteepDescentNoPartOfTheStripBuriesItselfInTheGround){
    // a rigid tile pinned flatter than 45 degree ground drives its downhill end under the surface;
    // a strip has no tile ends to drive under anything
    const auto slope = [](float, float along){ return -along; };
    const SplineProfile profile = flatRibbon(2.5f);
    const auto stations = stripDrapedStations(slope, 0.0f, 30.0f, 61u, 1.25f);

    const SplineStripMesh mesh = buildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.isValid());
    for (const SplineVertex& v : mesh.vertices){
        const float ground = slope(v.position.x, -v.position.z);
        EXPECT_GE(v.position.y, ground - 1e-3f) << "a vertex sank " << (ground - v.position.y) << " m into the ground";
        EXPECT_LE(v.position.y - ground, 1e-3f) << "a vertex floats above the ground";
    }

    // the contrast: a 2.4 m tile held to a 35 degree ceiling on 45 degree ground
    const float tileHalfLength = 1.2f;
    const float ceilingError = tileHalfLength * (std::tan(45.0f * PI / 180.0f) - std::tan(35.0f * PI / 180.0f)) *
                               std::cos(45.0f * PI / 180.0f);
    EXPECT_GT(ceilingError, 0.1f) << "the contrast this test exists to draw has vanished";
    expectWellFormed(mesh, "ribbon on a 45 degree descent");
}

TEST(SplineStripBuilderTests, TheStripNeverDoublesBackOnItselfSoNoTwoPartsOverlap){
    const auto slope = [](float, float along){ return -along; };
    const SplineProfile profile = flatRibbon(2.5f);
    const auto stations = stripDrapedStations(slope, 0.0f, 30.0f, 61u, 1.25f);

    const SplineStripMesh mesh = buildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.isValid());
    for (uint32_t r = 1u; r < mesh.ringCount; r++){
        for (uint32_t s = 0; s < mesh.ringVertexCount; s++){
            const Vector3& previous = mesh.vertices[(r - 1u) * mesh.ringVertexCount + s].position;
            const Vector3& current = mesh.vertices[r * mesh.ringVertexCount + s].position;
            // travel is -Z here; every ring must advance along it
            EXPECT_LT(current.z, previous.z) << "ring " << r << " doubled back";
        }
    }
}

TEST(SplineStripBuilderTests, AdjacentSectionsDoNotLeanOppositeWaysOnBumpyGround){
    // frames derived from one shared polyline cannot alternate their lean the way per-tile normal
    // sampling does on a ripple
    const auto ripple = [](float, float along){ return 0.15f * std::sin(along * 2.0f); };
    const SplineProfile profile = flatRibbon(2.5f);
    const auto stations = stripDrapedStations(ripple, 0.0f, 30.0f, 121u, 1.25f);

    const SplineStripMesh mesh = buildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.isValid());
    const auto countSignFlips = [](const std::vector<float>& heights){
        uint32_t flips = 0;
        float previousPitch = 0.0f;
        for (size_t i = 1; i < heights.size(); i++){
            const float pitch = heights[i] - heights[i - 1];
            if (i > 1u && ((pitch > 0.0f) != (previousPitch > 0.0f))){
                flips++;
            }
            previousPitch = pitch;
        }
        return flips;
    };

    std::vector<float> stripHeights;
    std::vector<float> groundHeights;
    for (uint32_t r = 0; r < mesh.ringCount; r++){
        const Vector3& p = mesh.vertices[r * mesh.ringVertexCount].position;
        stripHeights.push_back(p.y);
        groundHeights.push_back(ripple(p.x, -p.z));
    }

    // the bound is the GROUND's own turning-point count: the strip may follow every rise and fall
    // the terrain has and must not invent one
    const uint32_t signFlips = countSignFlips(stripHeights);
    EXPECT_LE(signFlips, countSignFlips(groundHeights));
    EXPECT_LT(signFlips, mesh.ringCount / 4u);
}

// ---- Per-side half-widths ----

// Equal half-widths reproduce the single-scale sweep BIT FOR BIT: any change to the arithmetic (a
// blend of the two sides, a different multiply order) moves the last mantissa bits of every vertex
// and re-textures existing scenes. The reference is the single-scale formula written out by hand.
TEST(SplineStripBuilderTests, SymmetricHalfWidthsReproduceTheSingleScaleSweepBitForBit){
    // not a power of two on either side of the ratio, so a reciprocal multiply cannot slip past
    const SplineProfile profile = buildProfile([]{
        SplineProfileParams p;
        p.shape = SplineProfileShape::Bevel;
        p.width = 4.5f;
        p.edgeDrop = 0.0f;
        p.edgeInset = 0.0f;
        return p;
    }());
    ASSERT_TRUE(profile.isValid());
    ASSERT_FLOAT_EQ(profile.nominalHalfWidth, 2.25f);

    constexpr float halfWidth = 3.0f;
    const std::vector<SplineStation> stations = stripStraightStations(8u, 2.0f, halfWidth);

    SplineStripParams params;
    params.widthScale = SplineProfileScale::LateralOnly;
    params.uOriginMetres = 0.0f;
    const SplineStripMesh mesh = buildSplineStrip(profile, stations, params);
    ASSERT_TRUE(mesh.isValid());
    ASSERT_EQ(mesh.ringCount, stations.size());

    const float scale = halfWidth / profile.nominalHalfWidth;
    const uint32_t pointCount = static_cast<uint32_t>(profile.points.size());
    ASSERT_EQ(mesh.ringVertexCount, pointCount);

    for (uint32_t r = 0; r < mesh.ringCount; r++){
        const SplineStation& s = stations[r];
        for (uint32_t j = 0; j < pointCount; j++){
            const Vector3 expected = s.position + s.right * (profile.points[j].position.x * scale) +
                                     s.up * (profile.points[j].position.y * 1.0f);
            const Vector3& actual = mesh.vertices[r * mesh.ringVertexCount + j].position;
            EXPECT_TRUE(bitsEqual(expected.x, actual.x)) << "ring " << r << " point " << j << " x drifted";
            EXPECT_TRUE(bitsEqual(expected.y, actual.y)) << "ring " << r << " point " << j << " y drifted";
            EXPECT_TRUE(bitsEqual(expected.z, actual.z)) << "ring " << r << " point " << j << " z drifted";
        }
    }
}

TEST(SplineStripBuilderTests, AsymmetricHalfWidthsPlaceEachEdgeIndependently){
    const SplineProfile profile = flatRibbon(4.0f);
    ASSERT_TRUE(profile.isValid());

    constexpr float left = 2.0f;
    constexpr float right = 6.0f;
    std::vector<SplineStation> stations = stripStraightStations(4u, 2.0f, 1.0f);
    for (SplineStation& s : stations){
        s.halfWidthLeft = left;
        s.halfWidthRight = right;
    }

    SplineStripParams params;
    params.widthScale = SplineProfileScale::LateralOnly;
    params.uOriginMetres = 0.0f;
    const SplineStripMesh mesh = buildSplineStrip(profile, stations, params);
    ASSERT_TRUE(mesh.isValid());

    // the run travels -Z, so right is +X and the two half-widths land straight on the bounds
    EXPECT_NEAR(mesh.bounds.getMinimum().x, -left, 1.0e-5f) << "the left edge must sit at its own width";
    EXPECT_NEAR(mesh.bounds.getMaximum().x, right, 1.0e-5f) << "the right edge must sit at its own width";

    // widening one side must not shift the centreline
    for (uint32_t r = 0; r < mesh.ringCount; r++){
        EXPECT_NEAR(mesh.vertices[r * mesh.ringVertexCount].position.z, stations[r].position.z, 1.0e-5f);
    }
}

TEST(SplineStripBuilderTests, AZeroHalfWidthCollapsesOnlyItsOwnSide){
    const SplineProfile profile = flatRibbon(4.0f);
    std::vector<SplineStation> stations = stripStraightStations(4u, 2.0f, 1.0f);
    for (SplineStation& s : stations){
        s.halfWidthLeft = 0.0f;
        s.halfWidthRight = 5.0f;
    }

    SplineStripParams params;
    params.widthScale = SplineProfileScale::LateralOnly;
    params.uOriginMetres = 0.0f;
    const SplineStripMesh mesh = buildSplineStrip(profile, stations, params);
    ASSERT_TRUE(mesh.isValid());

    EXPECT_NEAR(mesh.bounds.getMinimum().x, 0.0f, 1.0e-5f);
    EXPECT_NEAR(mesh.bounds.getMaximum().x, 5.0f, 1.0e-5f);
}

// ---- Frozen output ----

// A station that asks for no corner, crease or ground offset sweeps exactly as it always has: a
// last-bit move re-textures and re-shades saved scenes nobody edited. Baseline captured from this
// builder on x86-64. The inputs avoid libm and the builder is compiled without contraction, but
// this file is not: an arm64 compiler fuses the test's own input arithmetic into FMAs, so the
// baseline is checked where no FMA is emitted without being asked for.
#if defined(__x86_64__) || defined(_M_X64)
TEST(SplineStripBuilderTests, AStationWithNoCornerOrGroundOffsetSweepsByteForByteAsBefore){
    EXPECT_EQ(stripFrozenRunDigest(SplineProfileShape::Rectangle), 3182996133620962138ull);
    EXPECT_EQ(stripFrozenRunDigest(SplineProfileShape::Crown), 16964508710717870154ull);
}
#endif

TEST(SplineStripBuilderTests, OnlyGroundedPointsTakeTheGroundOffset){
    const SplineProfile wall = stripWall(1.0f, 3.0f);
    ASSERT_TRUE(wall.points[0].grounded != 0u && wall.points[3].grounded != 0u);
    ASSERT_TRUE(wall.points[1].grounded == 0u && wall.points[2].grounded == 0u);

    const std::vector<SplineStation> level = stripStraightStations(4u, 1.0f, 0.5f);
    std::vector<SplineStation> sunk = level;
    for (SplineStation& station : sunk){
        station.groundOffset = -0.25f;
    }

    const SplineStripMesh before = buildSplineStrip(wall, level, {});
    const SplineStripMesh after = buildSplineStrip(wall, sunk, {});
    ASSERT_EQ(before.vertices.size(), after.vertices.size());
    uint32_t moved = 0;
    for (size_t v = 0; v < before.vertices.size(); v++){
        const Vector3& a = before.vertices[v].position;
        const Vector3& b = after.vertices[v].position;
        EXPECT_FLOAT_EQ(a.x, b.x);
        EXPECT_FLOAT_EQ(a.z, b.z);
        if (a.y < 1.5f){
            EXPECT_NEAR(b.y, a.y - 0.25f, 1.0e-6f) << "base vertex " << v;
            moved++;
        }else{
            EXPECT_FLOAT_EQ(b.y, a.y) << "top vertex " << v;
        }
    }
    EXPECT_GT(moved, 0u);
}

// ---- V on the side faces ----

// Without a datum a wall's V runs around its cross-section: 0 at the left base, up the left face,
// across the top and down the right face to the perimeter. V runs along the profile edge, which is
// -cross(normal, forward) in this frame, so every tangent's w is -1.
TEST(SplineStripBuilderTests, WithoutADatumVRunsAroundThePerimeter){
    const SplineProfile wall = stripWall(1.0f, 3.0f);
    const SplineStripMesh mesh = buildSplineStrip(wall, stripStraightStations(3u, 1.0f, 0.5f), {});
    ASSERT_TRUE(mesh.isValid());
    for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
        const SplineVertex& vertex = mesh.vertices[k];
        const float x = vertex.position.x;
        const float y = vertex.position.y;
        const bool leftFace = std::abs(vertex.normal.x + 1.0f) < 1.0e-5f;
        const bool top = std::abs(vertex.normal.y - 1.0f) < 1.0e-5f;
        const bool rightFace = std::abs(vertex.normal.x - 1.0f) < 1.0e-5f;
        if (leftFace){
            EXPECT_NEAR(vertex.uv.y, y, 1.0e-5f);
        }else if (top){
            EXPECT_NEAR(vertex.uv.y, 3.0f + (x + 0.5f), 1.0e-5f);
        }else if (rightFace){
            EXPECT_NEAR(vertex.uv.y, 4.0f + (3.0f - y), 1.0e-5f);
        }
        EXPECT_EQ(vertex.tangent.w, -1.0f);
    }
    expectWellFormed(mesh, "perimeter V wall");
}

// With a datum the side faces and caps take V as height above it, so on a slope the courses stay
// level; the top keeps the perimeter V. V climbs up the right face along cross(normal, forward)
// (w = +1) and up the left face against it (w = -1).
TEST(SplineStripBuilderTests, SideFacesTakeTheirHeightAboveTheDatumAsV){
    const SplineProfile wall = stripWall(1.0f, 3.0f);
    const std::vector<SplineStation> stations =
        stripDrapedStations([](float, float along){ return 0.25f * along; }, 0.0f, 8.0f, 9u, 0.5f);

    SplineStripParams perimeter;
    perimeter.tilesPerMetreV = 0.5f;
    SplineStripParams height = perimeter;
    height.heightVDatumMetres = -2.0f;
    const SplineStripMesh around = buildSplineStrip(wall, stations, perimeter);
    const SplineStripMesh level = buildSplineStrip(wall, stations, height);
    ASSERT_EQ(around.vertices.size(), level.vertices.size());

    uint32_t sides = 0;
    uint32_t caps = 0;
    const uint32_t ringVertices = level.ringCount * level.ringVertexCount;
    for (uint32_t v = 0; v < level.vertices.size(); v++){
        const SplineVertex& vertex = level.vertices[v];
        const float heightV = (vertex.position.y + 2.0f) * 0.5f;
        if (v >= ringVertices){
            EXPECT_NEAR(vertex.uv.y, heightV, 1.0e-5f) << "cap vertex " << v;
            caps++;
            continue;
        }
        const bool side = std::abs(vertex.normal.x) > std::abs(vertex.normal.y);
        if (side){
            EXPECT_NEAR(vertex.uv.y, heightV, 1.0e-5f) << "side vertex " << v;
            EXPECT_EQ(vertex.tangent.w, vertex.normal.x > 0.0f ? 1.0f : -1.0f) << "side vertex " << v;
            sides++;
        }else{
            EXPECT_EQ(vertex.uv.y, around.vertices[v].uv.y) << "top or bottom vertex " << v;
            EXPECT_EQ(vertex.tangent.w, -1.0f);
        }
        EXPECT_EQ(vertex.uv.x, around.vertices[v].uv.x);
    }
    EXPECT_GT(sides, 0u);
    EXPECT_GT(caps, 0u);
    expectWellFormed(level, "level-course wall");
    expectWellFormed(around, "perimeter wall on a slope");
}

// ---- AC5: every profile builds a well-formed mesh ----

TEST(SplineStripBuilderTests, EveryProfileSweptAlongAWindingRoadIsWellFormed){
    const std::vector<SplineStation> stations = stripWindingRoad(120.0f, 0.8f);
    struct Case{
        SplineProfileShape shape;
        SplineProfileScale scale;
    };
    for (const Case c : {Case{SplineProfileShape::Flat, SplineProfileScale::LateralOnly},
                         Case{SplineProfileShape::Bevel, SplineProfileScale::LateralOnly},
                         Case{SplineProfileShape::Crown, SplineProfileScale::LateralOnly},
                         Case{SplineProfileShape::Rectangle, SplineProfileScale::None},
                         Case{SplineProfileShape::Tube, SplineProfileScale::Uniform}}){
        SplineProfileParams profileParams;
        profileParams.shape = c.shape;
        profileParams.width = 8.0f;
        SplineStripParams params;
        params.widthScale = c.scale;
        params.tilesPerMetreU = 0.5f;
        const SplineStripMesh mesh = buildSplineStrip(buildProfile(profileParams), stations, params);
        ASSERT_TRUE(mesh.isValid());
        expectWellFormed(mesh, "profile " + std::to_string(static_cast<int>(c.shape)));
    }
}

TEST(SplineStripBuilderTests, ATubeTakesItsRadiusFromTheRadiusChannel){
    SplineProfileParams profileParams;
    profileParams.shape = SplineProfileShape::Tube;
    profileParams.tubeSides = 10;
    std::vector<SplineStation> stations = stripStraightStations(6u, 1.0f, 1.0f);
    for (size_t i = 0; i < stations.size(); i++){
        stations[i].halfWidthLeft = 0.5f + 0.25f * static_cast<float>(i);
        stations[i].halfWidthRight = stations[i].halfWidthLeft;
    }
    SplineStripParams params;
    params.widthScale = SplineProfileScale::Uniform;
    const SplineStripMesh mesh = buildSplineStrip(buildProfile(profileParams), stations, params);
    ASSERT_TRUE(mesh.isValid());
    // ten smooth points and the V seam's duplicate
    EXPECT_EQ(mesh.ringVertexCount, 11u);
    for (uint32_t r = 0; r < mesh.ringCount; r++){
        for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
            const Vector3 offset = mesh.vertices[r * mesh.ringVertexCount + k].position - stations[r].position;
            EXPECT_NEAR(lengthOf(offset), stations[r].halfWidthLeft, 1e-5f) << "ring " << r << " slot " << k;
            EXPECT_NEAR(offset.z, 0.0f, 1e-6f);
        }
    }
    // capped at both ends, the caps' ten points each
    EXPECT_EQ(mesh.vertices.size(), static_cast<size_t>(mesh.ringCount) * mesh.ringVertexCount + 20u);
    expectWellFormed(mesh, "tube");
}

TEST(SplineStripBuilderTests, LateralAndVerticalOffsetsMoveTheWholeCrossSection){
    const std::vector<SplineStation> stations = stripStraightStations(5u, 1.5f, 1.0f);
    SplineStripParams plain;
    SplineStripParams offset;
    offset.lateralOffset = 1.5f;
    offset.verticalOffset = 0.25f;
    const SplineStripMesh a = buildSplineStrip(stripCrownRoad(), stations, plain);
    const SplineStripMesh b = buildSplineStrip(stripCrownRoad(), stations, offset);
    ASSERT_EQ(a.vertices.size(), b.vertices.size());
    for (size_t v = 0; v < a.vertices.size(); v++){
        EXPECT_NEAR(b.vertices[v].position.x - a.vertices[v].position.x, 1.5f, 1e-5f);
        EXPECT_NEAR(b.vertices[v].position.y - a.vertices[v].position.y, 0.25f, 1e-5f);
        EXPECT_EQ(b.vertices[v].position.z, a.vertices[v].position.z);
        EXPECT_EQ(b.vertices[v].uv.x, a.vertices[v].uv.x);
        EXPECT_EQ(b.vertices[v].uv.y, a.vertices[v].uv.y);
    }
}

TEST(SplineStripBuilderTests, AChannelRiverWritesTheFlowEncodingIntoTheVertexColour){
    std::vector<SplineStation> stations = stripStraightStations(4u, 2.0f, 3.0f);
    SplineStripParams params;
    const SplineStripMesh still = buildSplineStrip(flatRibbon(6.0f), stations, params);
    for (const SplineVertex& v : still.vertices){
        EXPECT_EQ(v.color.x, 1.0f);
        EXPECT_EQ(v.color.y, 1.0f);
        EXPECT_EQ(v.color.z, 1.0f);
        EXPECT_EQ(v.color.w, 1.0f);
    }

    // a run along -Z flows (0, -1) in XZ: rg = (0.5, 0)
    params.writeFlow = true;
    params.flowSpeedScale = 0.75f;
    const SplineStripMesh river = buildSplineStrip(flatRibbon(6.0f), stations, params);
    ASSERT_TRUE(river.isValid());
    for (const SplineVertex& v : river.vertices){
        EXPECT_NEAR(v.color.x, 0.5f, 1e-6f);
        EXPECT_NEAR(v.color.y, 0.0f, 1e-6f);
        EXPECT_FLOAT_EQ(v.color.z, 0.75f);
        EXPECT_EQ(v.color.w, 1.0f);
    }

    // a diagonal reach: rg = 0.5 + 0.5 d of its level direction, whatever its grade
    for (SplineStation& s : stations){
        s.forward = unit(Vector3(3.0f, -1.0f, -4.0f));
    }
    const SplineStripMesh diagonal = buildSplineStrip(flatRibbon(6.0f), stations, params);
    for (const SplineVertex& v : diagonal.vertices){
        EXPECT_NEAR(v.color.x, 0.5f + 0.5f * 0.6f, 1e-5f);
        EXPECT_NEAR(v.color.y, 0.5f - 0.5f * 0.8f, 1e-5f);
    }
}

// ---- AC5: the closed loop's seam, chunking ----

TEST(SplineStripBuilderTests, AClosedLoopsUSeamLandsOnAWholeTile){
    const std::vector<SplineStation> stations = stripCircleStations(10.0f, 96u, 1.5f);
    SplineStripParams params;
    params.tilesPerMetreU = 0.43f;
    params.snapUForClosedLoop = true;

    const SplineStripMesh whole = buildSplineStrip(stripCrownRoad(), stations, params);
    ASSERT_TRUE(whole.isValid());
    const float wholeSeam = whole.vertices[(whole.ringCount - 1u) * whole.ringVertexCount].uv.x;
    EXPECT_NEAR(wholeSeam, std::round(wholeSeam), 1e-4f);
    EXPECT_EQ(whole.vertices[0].uv.x, 0.0f);
    // the seam's two rings stand on one place
    for (uint32_t k = 0; k < whole.ringVertexCount; k++){
        EXPECT_TRUE(bitsEqual(whole.vertices[k].position,
                              whole.vertices[(whole.ringCount - 1u) * whole.ringVertexCount + k].position)) << "slot " << k;
    }
    expectWellFormed(whole, "closed crown loop");

    // chunked, the density snaps once over the whole loop and the last chunk ends on the same tile
    const std::vector<SplineStripChunkRange> chunks = planSplineStripChunks(stripCrownRoad(), stations, params, 16.0f);
    ASSERT_GE(chunks.size(), 3u);
    const std::vector<SplineStripMesh> meshes = stripBuildChunks(stripCrownRoad(), stations, params, chunks);
    const SplineStripMesh& last = meshes.back();
    const float chunkSeam = last.vertices[(last.ringCount - 1u) * last.ringVertexCount].uv.x;
    EXPECT_TRUE(bitsEqual(chunkSeam, wholeSeam));
    EXPECT_NEAR(chunkSeam, std::round(chunkSeam), 1e-4f);
}

TEST(SplineStripBuilderTests, ChunkBoundariesShareBitIdenticalVertices){
    const std::vector<SplineStation> stations = stripWindingRoad(300.0f, 0.75f);
    const SplineProfile road = stripCrownRoad();
    SplineStripParams params;
    params.tilesPerMetreU = 0.5f;
    params.writeFlow = true;

    const std::vector<SplineStripChunkRange> chunks = planSplineStripChunks(road, stations, params);
    ASSERT_GE(chunks.size(), 4u);
    EXPECT_TRUE(chunks.front().opensRun);
    EXPECT_TRUE(chunks.back().closesRun);
    EXPECT_EQ(chunks.front().firstStation, 0u);
    EXPECT_EQ(chunks.back().endStation, stations.size());
    for (size_t c = 0; c + 1u < chunks.size(); c++){
        // consecutive chunks meet on one station, at or past a multiple of the chunk length
        EXPECT_EQ(chunks[c].endStation - 1u, chunks[c + 1u].firstStation) << "chunk " << c;
        EXPECT_GE(chunks[c].endDistance, SPLINE_STRIP_CHUNK_LENGTH * static_cast<float>(c + 1u)) << "chunk " << c;
        EXPECT_LT(chunks[c].endDistance, SPLINE_STRIP_CHUNK_LENGTH * static_cast<float>(c + 1u) + 1.0f) << "chunk " << c;
    }

    const std::vector<SplineStripMesh> meshes = stripBuildChunks(road, stations, params, chunks);
    const SplineStripMesh whole = buildSplineStrip(road, stations, params);
    ASSERT_TRUE(whole.isValid());
    uint32_t ring = 0;
    for (size_t c = 0; c < meshes.size(); c++){
        const SplineStripMesh& mesh = meshes[c];
        ASSERT_TRUE(mesh.isValid()) << "chunk " << c;
        EXPECT_LE(mesh.vertices.size(), SPLINE_STRIP_MAX_CHUNK_VERTICES);
        expectWellFormed(mesh, "chunk " + std::to_string(c));
        // each chunk's rings are the whole build's rings, bit for bit
        for (uint32_t r = 0; r < mesh.ringCount; r++){
            for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
                EXPECT_TRUE(vertexBitsEqual(mesh.vertices[r * mesh.ringVertexCount + k],
                                            whole.vertices[(ring + r) * whole.ringVertexCount + k]))
                    << "chunk " << c << " ring " << r << " slot " << k;
            }
        }
        ring += mesh.ringCount - 1u;
        if (c + 1u < meshes.size()){
            const SplineStripMesh& next = meshes[c + 1u];
            for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
                EXPECT_TRUE(vertexBitsEqual(mesh.vertices[(mesh.ringCount - 1u) * mesh.ringVertexCount + k], next.vertices[k]))
                    << "boundary " << c << " slot " << k;
            }
        }
    }
    EXPECT_EQ(ring + 1u, whole.ringCount);
}

TEST(SplineStripBuilderTests, RebuildingOneChunkAfterALocalEditLeavesTheOthersUnchanged){
    std::vector<SplineStation> stations = stripWindingRoad(300.0f, 0.75f);
    const SplineProfile road = stripCrownRoad();
    const SplineStripParams params;
    const std::vector<SplineStripChunkRange> before = planSplineStripChunks(road, stations, params);
    const std::vector<SplineStripMesh> original = stripBuildChunks(road, stations, params, before);
    ASSERT_GE(before.size(), 4u);

    // lift the stations of the third chunk's middle stretch, as a knot drag there would
    const uint32_t from = before[2].firstStation + 10u;
    for (uint32_t i = from; i < from + 20u; i++){
        stations[i].position.y += 0.5f;
    }
    const std::vector<SplineStripChunkRange> after = planSplineStripChunks(road, stations, params);
    ASSERT_EQ(after.size(), before.size());
    for (size_t c = 0; c < after.size(); c++){
        EXPECT_EQ(after[c].firstStation, before[c].firstStation);
        EXPECT_EQ(after[c].endStation, before[c].endStation);
        SplineStripMesh rebuilt;
        buildSplineStripChunk(road, stations, after[c], params, rebuilt);
        ASSERT_EQ(rebuilt.vertices.size(), original[c].vertices.size());
        bool same = rebuilt.indices == original[c].indices;
        for (size_t v = 0; v < rebuilt.vertices.size() && same; v++){
            same = vertexBitsEqual(rebuilt.vertices[v], original[c].vertices[v]);
        }
        EXPECT_EQ(same, c != 2u) << "chunk " << c;
    }
}

TEST(SplineStripBuilderTests, AChunkNeverPassesTheVertexLimitAndNeverSplitsACorner){
    // a wall with mitred corners, chunked far finer than its corners are apart
    Walk walk = walkPolyline({Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, -30.0f), Vector3(30.0f, 0.0f, -30.0f),
                              Vector3(30.0f, 0.0f, -60.0f)}, 0.25f, 0.3f);
    ASSERT_TRUE(applyCorners(walk.stations, walk.corners, SplineCornerStyle::Round, UP, false).empty());
    const SplineProfile wall = wallProfile(0.6f, 3.0f);
    const SplineStripParams params;
    const uint32_t limit = 400u;
    const std::vector<SplineStripChunkRange> chunks = planSplineStripChunks(wall, walk.stations, params, 64.0f, limit);
    ASSERT_GE(chunks.size(), 3u);
    const std::vector<SplineStripMesh> meshes = stripBuildChunks(wall, walk.stations, params, chunks);
    uint32_t rings = 0;
    for (size_t c = 0; c < chunks.size(); c++){
        EXPECT_LE(meshes[c].vertices.size(), limit) << "chunk " << c;
        rings += meshes[c].ringCount - 1u;
        if (c + 1u < chunks.size()){
            const SplineStation& boundary = walk.stations[chunks[c].endStation - 1u];
            EXPECT_EQ(boundary.join, SplineStationJoin::Advance) << "chunk " << c;
            EXPECT_EQ(boundary.mitreSides, 0u) << "chunk " << c << " ends inside a corner";
            EXPECT_EQ(walk.stations[chunks[c].endStation].join, SplineStationJoin::Advance) << "chunk " << c;
        }
        expectWellFormed(meshes[c], "wall chunk " + std::to_string(c));
    }
    EXPECT_EQ(rings + 1u, buildSplineStrip(wall, walk.stations, params).ringCount);
}

// R19: a 1 km Crown road (its seven ring slots), stations 0.8 m apart, builds within 3 ms (enforced
// on the benchmark server), chunked as the extrude system builds it
TEST(SplineStripBuilderTests, AOneKilometreCrownRoadBuildsWithinItsBudget){
    const std::vector<SplineStation> stations = stripWindingRoad(1000.0f, 0.8f);
    const SplineProfile road = stripCrownRoad();
    const SplineStripParams params;
    std::vector<SplineStripMesh> meshes;
    const auto buildAll = [&](){
        const std::vector<SplineStripChunkRange> chunks = planSplineStripChunks(road, stations, params);
        meshes.resize(chunks.size());
        for (size_t c = 0; c < chunks.size(); c++){
            buildSplineStripChunk(road, stations, chunks[c], params, meshes[c]);
        }
    };
    buildAll();
    const auto start = std::chrono::steady_clock::now();
    buildAll();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("[bench] Crown road 1 km, %zu chunks %.3f ms (budget 3 ms)\n", meshes.size(), ms);
    ASSERT_GE(meshes.size(), 15u);
    EXPECT_EQ(meshes.front().ringVertexCount, 7u);
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(ms, 3.0 * testsupport::perfSlack());
    }
}
