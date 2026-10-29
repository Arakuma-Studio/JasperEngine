// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Tests/SplineCornerTests.cpp, Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

// Mitre and Round corners, the mitre limit, corner issues, face turn, inner folds and the welded
// loop (S18 R6, AC5): OpenEngine's cases mirrored z -> -z (SplineGeometryTestSupport.h).

#include "SplineGeometryTestSupport.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::spline;
using namespace jasperengine::spline::geometrytest;

namespace{

    // a square loop of `side` metres turning right at every corner, starting along -Z
    std::vector<Vector3> cornerSquareLoop(float side){
        return {Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, -side), Vector3(side, 0.0f, -side), Vector3(side, 0.0f, 0.0f),
                Vector3(0.0f, 0.0f, 0.0f)};
    }

    // How far each side face's U advance departs from the face's own length, band by band: the
    // largest |dU / length - 1| over every stitched band of every side slot. Bands with no length on
    // the face (the inside of a round corner pivots on one point) are skipped.
    float cornerWorstFaceStretch(const SplineStripMesh& mesh, const std::vector<SplineStation>& stations, bool topFaces){
        float worst = 0.0f;
        for (uint32_t r = 0; r + 1u < mesh.ringCount; r++){
            if (stations[r + 1u].join == SplineStationJoin::Crease){
                continue;
            }
            for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
                const SplineVertex& a = mesh.vertices[r * mesh.ringVertexCount + k];
                const SplineVertex& b = mesh.vertices[(r + 1u) * mesh.ringVertexCount + k];
                if (topFaces ? a.normal.y < 0.5f : std::abs(a.normal.y) > 0.5f){
                    continue;
                }
                const float length = lengthOf(b.position - a.position);
                if (length < 1.0e-3f){
                    continue;
                }
                worst = std::max(worst, std::abs(std::abs(b.uv.x - a.uv.x) / length - 1.0f));
            }
        }
        return worst;
    }

    // perpendicular distance, in the level plane, from `p` to the line through `origin` along `direction`
    float cornerPlanDistanceToLine(const Vector3& p, const Vector3& origin, const Vector3& direction){
        const Vector3 d = unit(Vector3(direction.x, 0.0f, direction.z));
        const Vector3 offset(p.x - origin.x, 0.0f, p.z - origin.z);
        return lengthOf(offset - d * dotOf(offset, d));
    }

    uint32_t cornerRingsAt(const std::vector<SplineStation>& stations, float distance){
        uint32_t rings = 0;
        for (const SplineStation& station : stations){
            rings += std::abs(station.distance - distance) <= 1.0e-6f ? 1u : 0u;
        }
        return rings;
    }

}

// A Mitre's ring stands on the bisector scaled by 1 / cos(theta/2): measured across the wall,
// perpendicular to each leg, every ring at the corner is the full thickness.
TEST(SplineCornerTests, ALinearNinetyDegreeCornerKeepsTheWallsFullThickness){
    for (const float thickness : {0.6f, 1.0f, 2.0f}){
        Walk walk = walkPolyline(elbow(10.0f, 7.3f, 90.0f), 0.5f, thickness * 0.5f);
        const uint32_t cornerIndex = walk.corners[0].stationIndex;
        const Vector3 corner = walk.stations[cornerIndex].position;
        const float cornerDistance = walk.stations[cornerIndex].distance;
        const std::vector<SplineCornerIssue> fallbacks = applyCorners(walk.stations, walk.corners, SplineCornerStyle::Mitre, UP, false);
        EXPECT_TRUE(fallbacks.empty());

        const SplineStripMesh mesh = sweepFromZero(wallProfile(thickness, 3.0f), walk.stations);
        ASSERT_TRUE(mesh.isValid());

        uint32_t cornerRings = 0;
        for (uint32_t r = 0; r < mesh.ringCount; r++){
            if (std::abs(walk.stations[r].distance - cornerDistance) > 1.0e-6f){
                continue;
            }
            cornerRings++;
            const RingBase base = baseOf(mesh, r, corner, walk.stations[r].right);
            // leg 1 runs -Z, so its across is X; leg 2 runs +X, across is Z
            EXPECT_NEAR(std::abs(base.right.x - base.left.x), thickness, 1.0e-3f) << "thickness " << thickness << ", ring " << r;
            EXPECT_NEAR(std::abs(base.right.z - base.left.z), thickness, 1.0e-3f) << "thickness " << thickness << ", ring " << r;
        }
        EXPECT_EQ(cornerRings, 2u) << "a mitre is one ring emitted twice";
        EXPECT_EQ(invertedSideTriangles(mesh), 0u) << "thickness " << thickness;
        expectWellFormed(mesh, "mitred wall " + std::to_string(thickness));
    }
}

// Both copies of the mitre ring stand on the same positions, the outer faces of the two legs meet
// on that edge, and each copy carries its own leg's normals: a sharp crease, not a pillow.
TEST(SplineCornerTests, AMitreMeetsBothLegsOuterFacesOnOneSharpEdge){
    const float thickness = 1.0f;
    Walk walk = walkPolyline(elbow(10.0f, 7.3f, 90.0f), 0.5f, thickness * 0.5f);
    const float cornerDistance = walk.stations[walk.corners[0].stationIndex].distance;
    (void)applyCorners(walk.stations, walk.corners, SplineCornerStyle::Mitre, UP, false);
    const SplineStripMesh mesh = sweepFromZero(wallProfile(thickness, 3.0f), walk.stations);
    ASSERT_TRUE(mesh.isValid());

    uint32_t incoming = mesh.ringCount;
    for (uint32_t r = 0; r < mesh.ringCount; r++){
        if (std::abs(walk.stations[r].distance - cornerDistance) <= 1.0e-6f){
            incoming = r;
            break;
        }
    }
    ASSERT_LT(incoming + 1u, mesh.ringCount);
    const uint32_t outgoing = incoming + 1u;
    EXPECT_EQ(walk.stations[outgoing].join, SplineStationJoin::Crease);

    for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
        EXPECT_TRUE(bitsEqual(mesh.vertices[incoming * mesh.ringVertexCount + k].position,
                              mesh.vertices[outgoing * mesh.ringVertexCount + k].position)) << "slot " << k;
    }

    // a right turn: the outside is the left (-X on leg 1, -Z on leg 2); the outer bottom corner lies
    // on leg 1's outer face (x = -0.5) and on leg 2's (z = -10.5)
    const RingBase base = baseOf(mesh, incoming, walk.stations[incoming].position, walk.stations[incoming].right);
    EXPECT_NEAR(base.left.x, -0.5f, 1.0e-5f);
    EXPECT_NEAR(base.left.z, -10.5f, 1.0e-5f);
    // the inner corner is where the two inside faces meet
    EXPECT_NEAR(base.right.x, 0.5f, 1.0e-5f);
    EXPECT_NEAR(base.right.z, -9.5f, 1.0e-5f);

    // the side faces keep their own leg's normal up to the edge: 90 degrees apart across the crease
    float leastDot = 1.0f;
    for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
        const Vector3& n0 = mesh.vertices[incoming * mesh.ringVertexCount + k].normal;
        const Vector3& n1 = mesh.vertices[outgoing * mesh.ringVertexCount + k].normal;
        if (std::abs(n0.y) > 0.5f){
            continue;
        }
        leastDot = std::min(leastDot, std::abs(dotOf(n0, n1)));
    }
    EXPECT_LT(leastDot, 1.0e-5f);

    // no band joins the two copies
    for (size_t t = 0; t < mesh.indices.size(); t += 3u){
        bool touchesIncoming = false;
        bool touchesOutgoing = false;
        for (size_t i = t; i < t + 3u; i++){
            touchesIncoming = touchesIncoming || mesh.indices[i] / mesh.ringVertexCount == incoming;
            touchesOutgoing = touchesOutgoing || mesh.indices[i] / mesh.ringVertexCount == outgoing;
        }
        EXPECT_FALSE(touchesIncoming && touchesOutgoing) << "triangle " << t / 3u;
    }
}

// A Round corner pivots about the corner point: the outside is an arc of the half-thickness, the
// inside meets at the inner mitre point, and a 90 degree turn takes 9 rings carrying one U.
TEST(SplineCornerTests, ARoundNinetyDegreeCornerIsNineRingsOnTheArc){
    const float thickness = 0.6f;
    Walk walk = walkPolyline(elbow(10.0f, 7.3f, 90.0f), 0.5f, thickness * 0.5f);
    const Vector3 corner = walk.stations[walk.corners[0].stationIndex].position;
    const float cornerDistance = walk.stations[walk.corners[0].stationIndex].distance;
    const auto fallbacks = applyCorners(walk.stations, walk.corners, SplineCornerStyle::Round, UP, false);
    EXPECT_TRUE(fallbacks.empty());
    const SplineStripMesh mesh = sweepFromZero(wallProfile(thickness, 3.0f), walk.stations);
    ASSERT_TRUE(mesh.isValid());

    uint32_t rings = 0;
    float cornerU = -1.0f;
    for (uint32_t r = 0; r < mesh.ringCount; r++){
        if (std::abs(walk.stations[r].distance - cornerDistance) > 1.0e-6f){
            continue;
        }
        rings++;
        const RingBase base = baseOf(mesh, r, corner, walk.stations[r].right);
        // outside (left of a right turn): on the arc about the corner point
        const Vector3 outer(base.left.x - corner.x, 0.0f, base.left.z - corner.z);
        EXPECT_NEAR(lengthOf(outer), thickness * 0.5f, 1.0e-4f) << "ring " << r;
        // inside: the inner mitre point
        EXPECT_NEAR(base.right.x, 0.3f, 1.0e-5f) << "ring " << r;
        EXPECT_NEAR(base.right.z, -(10.0f - 0.3f), 1.0e-5f) << "ring " << r;
        const float u = mesh.vertices[r * mesh.ringVertexCount].uv.x;
        if (cornerU < 0.0f){
            cornerU = u;
        }
        EXPECT_EQ(u, cornerU) << "ring " << r;
    }
    EXPECT_EQ(rings, 9u);
    EXPECT_EQ(invertedSideTriangles(mesh), 0u);
    expectWellFormed(mesh, "round cornered wall");
}

// Past the mitre limit (a scale of 2, a 120 degree turn) a Mitre is built Round and reported by its
// authored point. Its inside stands at the inner mitre point, 0.3 tan 75 = 1.12 m back along each leg.
TEST(SplineCornerTests, AMitreTurningPastTheLimitIsBuiltRoundAndReported){
    const float thickness = 0.6f;
    Walk walk = walkPolyline(elbow(10.0f, 7.3f, 150.0f), 0.5f, thickness * 0.5f);
    const Vector3 corner = walk.stations[walk.corners[0].stationIndex].position;
    const float cornerDistance = walk.stations[walk.corners[0].stationIndex].distance;
    const auto fallbacks = applyCorners(walk.stations, walk.corners, SplineCornerStyle::Mitre, UP, false);
    ASSERT_EQ(fallbacks.size(), 1u);
    EXPECT_EQ(fallbacks[0].kind, SplineCornerIssueKind::PastMitreLimit);
    EXPECT_EQ(fallbacks[0].pointIndex, 1u);
    EXPECT_NEAR(fallbacks[0].turnDegrees, 150.0f, 1.0e-2f);

    const SplineStripMesh mesh = sweepFromZero(wallProfile(thickness, 3.0f), walk.stations);
    ASSERT_TRUE(mesh.isValid());
    uint32_t rings = 0;
    for (uint32_t r = 0; r < mesh.ringCount; r++){
        if (std::abs(walk.stations[r].distance - cornerDistance) > 1.0e-6f){
            continue;
        }
        rings++;
        const RingBase base = baseOf(mesh, r, corner, walk.stations[r].right);
        EXPECT_NEAR(base.right.x, 0.3f, 1.0e-4f) << "ring " << r;
        EXPECT_NEAR(base.right.z, corner.z + 0.3f * std::tan(75.0f * PI / 180.0f), 1.0e-4f) << "ring " << r;
    }
    EXPECT_EQ(rings, 15u);
    expectWellFormed(mesh, "wall past the mitre limit");
}

// Closing a sampled loop makes its last station its first again, looking across the seam; a corner
// on the seam opens the stream with its outgoing half and closes it with its incoming half.
TEST(SplineCornerTests, AClosedLoopsSeamCornerSplitsAcrossTheSeam){
    Walk walk = walkPolyline(cornerSquareLoop(6.3f), 0.5f, 0.5f);
    walk.corners.insert(walk.corners.begin(), SplineCornerSite{0u, 0u});
    closeStationLoop(walk.stations, UP);
    EXPECT_TRUE(bitsEqual(walk.stations.front().position, walk.stations.back().position));
    EXPECT_TRUE(applyCorners(walk.stations, walk.corners, SplineCornerStyle::Mitre, UP, true).empty());

    SplineStripParams params;
    params.uOriginMetres = 0.0f;
    params.generateCaps = false;
    const SplineStripMesh mesh = buildSplineStrip(wallProfile(1.0f, 3.0f), walk.stations, params);
    ASSERT_TRUE(mesh.isValid());
    EXPECT_EQ(mesh.vertices.size(), static_cast<size_t>(mesh.ringCount) * mesh.ringVertexCount) << "no cap vertices";

    const uint32_t lastRing = (mesh.ringCount - 1u) * mesh.ringVertexCount;
    for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
        EXPECT_TRUE(bitsEqual(mesh.vertices[k].position, mesh.vertices[lastRing + k].position)) << "slot " << k;
    }
    // the seam is the loop's first corner: the outgoing leg runs -Z, the incoming one -X, and their
    // outer faces meet at (-0.5, 0, 0.5)
    EXPECT_NEAR(walk.stations.front().forward.z, -1.0f, 1.0e-6f);
    EXPECT_NEAR(walk.stations.back().forward.x, -1.0f, 1.0e-6f);
    const RingBase base = baseOf(mesh, 0u, walk.stations.front().position, walk.stations.front().right);
    EXPECT_NEAR(base.left.x, -0.5f, 1.0e-5f);
    EXPECT_NEAR(base.left.z, 0.5f, 1.0e-5f);
    EXPECT_EQ(invertedSideTriangles(mesh), 0u);
    expectWellFormed(mesh, "mitred square loop");
}

// without a corner on it, the seam frame looks from the station before the end to the one after the start
TEST(SplineCornerTests, ClosingASmoothLoopLooksAcrossTheSeam){
    std::vector<SplineStation> stations(24);
    for (size_t i = 0; i < stations.size(); i++){
        const float angle = 2.0f * PI * static_cast<float>(i) / static_cast<float>(stations.size() - 1u);
        stations[i].position = Vector3(10.0f * std::sin(angle), 0.0f, -10.0f * std::cos(angle));
        stations[i].forward = Vector3(0.0f, 0.0f, -1.0f);
        stations[i].distance = 10.0f * angle;
    }
    closeStationLoop(stations, UP);
    const Vector3 across = unit(stations[1].position - stations[stations.size() - 2u].position);
    EXPECT_NEAR(dotOf(stations.front().forward, across), 1.0f, 1.0e-6f);
    EXPECT_TRUE(bitsEqual(stations.front().forward, stations.back().forward));
    EXPECT_TRUE(bitsEqual(stations.front().position, stations.back().position));
    EXPECT_FLOAT_EQ(stations.back().distance, 20.0f * PI);
    // the frame is this engine's: right = forward x up
    EXPECT_NEAR(dotOf(stations.front().right, crossOf(stations.front().forward, UP)), 1.0f, 1.0e-5f);
}

// A ring standing at (or a centimetre beyond) the inner mitre point leaves a band with no length on
// the inside face that still carries U: it is dropped, and no side face has a zero-area triangle.
TEST(SplineCornerTests, ARingAtTheInnerMitrePointIsDroppedSoTheInsideFaceHasNoEmptyBand){
    for (const float offset : {0.0f, 0.011f}){
        Walk walk = walkPolyline(elbow(10.0f, 10.0f, 90.0f), 0.5f, 0.5f);
        const uint32_t nearIndex = walk.corners[0].stationIndex - 1u;
        walk.stations[nearIndex].position.z += offset;
        walk.stations[nearIndex].distance -= offset;
        const float cornerDistance = walk.stations[walk.corners[0].stationIndex].distance;
        ASSERT_FLOAT_EQ(cornerDistance, 10.0f);
        (void)applyCorners(walk.stations, walk.corners, SplineCornerStyle::Mitre, UP, false);

        for (size_t i = 0; i < walk.stations.size(); i++){
            const float fromCorner = std::abs(walk.stations[i].distance - cornerDistance);
            EXPECT_FALSE(fromCorner > 1.0e-6f && fromCorner < 0.5f + 0.05f)
                << "station " << i << " stands " << fromCorner << " m from the corner, offset " << offset;
        }

        const SplineStripMesh mesh = sweepFromZero(wallProfile(1.0f, 3.0f), walk.stations);
        ASSERT_TRUE(mesh.isValid());
        uint32_t emptySideTriangles = 0;
        const uint32_t ringVertices = mesh.ringCount * mesh.ringVertexCount;
        for (size_t t = 0; t + 2u < mesh.indices.size(); t += 3u){
            if (mesh.indices[t] >= ringVertices){
                continue;
            }
            const SplineVertex& a = mesh.vertices[mesh.indices[t]];
            if (std::abs(a.normal.y) > 0.5f){
                continue;
            }
            const Vector3 geometric = crossOf(mesh.vertices[mesh.indices[t + 1u]].position - a.position,
                                              mesh.vertices[mesh.indices[t + 2u]].position - a.position);
            if (lengthOf(geometric) < 1.0e-6f){
                emptySideTriangles++;
            }
        }
        EXPECT_EQ(emptySideTriangles, 0u) << "offset " << offset;
    }
}

// A 1 m wall at 90 degrees reaches 0.5 m: an end leg of 0.3 m is reported against its corner
// (naming the knot), one of 0.6 m is not.
TEST(SplineCornerTests, AnEndLegShorterThanTheCornersReachIsReported){
    for (const float endLeg : {0.3f, 0.6f}){
        Walk walk = walkPolyline(elbow(10.0f, endLeg, 90.0f), 0.1f, 0.5f);
        const std::vector<SplineCornerIssue> issues = applyCorners(walk.stations, walk.corners, SplineCornerStyle::Mitre, UP, false);
        if (endLeg < 0.5f){
            ASSERT_EQ(issues.size(), 1u);
            EXPECT_EQ(issues[0].kind, SplineCornerIssueKind::LegShorterThanReach);
            EXPECT_EQ(issues[0].pointIndex, 1u);
        }else{
            EXPECT_TRUE(issues.empty()) << "end leg " << endLeg;
        }
    }
}

// Two corners turning the same way (a U) share the leg between them, which must hold both reaches:
// 1 m for a 1 m wall at 90 degrees. Two turning opposite ways (a Z) are never reported.
TEST(SplineCornerTests, SameTurnCornersCloserThanTheirReachesAreReported){
    for (const float middle : {0.6f, 1.2f}){
        const std::vector<Vector3> u = {Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, -10.0f), Vector3(middle, 0.0f, -10.0f),
                                        Vector3(middle, 0.0f, 0.0f)};
        const std::vector<Vector3> z = {Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, -10.0f), Vector3(middle, 0.0f, -10.0f),
                                        Vector3(middle, 0.0f, -20.0f)};
        Walk uWalk = walkPolyline(u, 0.1f, 0.5f);
        const std::vector<SplineCornerIssue> uIssues = applyCorners(uWalk.stations, uWalk.corners, SplineCornerStyle::Mitre, UP, false);
        Walk zWalk = walkPolyline(z, 0.1f, 0.5f);
        const std::vector<SplineCornerIssue> zIssues = applyCorners(zWalk.stations, zWalk.corners, SplineCornerStyle::Mitre, UP, false);
        EXPECT_TRUE(zIssues.empty()) << "Z, middle leg " << middle;
        if (middle < 1.0f){
            ASSERT_EQ(uIssues.size(), 1u) << "U, middle leg " << middle;
            EXPECT_EQ(uIssues[0].kind, SplineCornerIssueKind::CornersOverlap);
            EXPECT_EQ(uIssues[0].pointIndex, 1u);
            EXPECT_EQ(uIssues[0].otherPointIndex, 2u);
        }else{
            EXPECT_TRUE(uIssues.empty()) << "U, middle leg " << middle;
        }
    }
}

// the mitre limit includes its own boundary: a corner at exactly 120 degrees is a Mitre
TEST(SplineCornerTests, ACornerAtExactlyTheMitreLimitIsMitred){
    Walk walk = walkPolyline(elbow(10.0f, 8.0f, 120.0f), 0.5f, 0.3f);
    const float cornerDistance = walk.stations[walk.corners[0].stationIndex].distance;
    EXPECT_TRUE(applyCorners(walk.stations, walk.corners, SplineCornerStyle::Mitre, UP, false).empty());
    EXPECT_EQ(cornerRingsAt(walk.stations, cornerDistance), 2u);
}

TEST(SplineCornerTests, NoneLeavesTheStreamAsSampled){
    Walk walk = walkPolyline(elbow(10.0f, 8.0f, 90.0f), 0.5f, 0.3f);
    const std::vector<SplineStation> sampled = walk.stations;
    EXPECT_TRUE(applyCorners(walk.stations, walk.corners, SplineCornerStyle::None, UP, false).empty());
    ASSERT_EQ(walk.stations.size(), sampled.size());
    for (size_t i = 0; i < sampled.size(); i++){
        EXPECT_TRUE(bitsEqual(walk.stations[i].position, sampled[i].position));
        EXPECT_EQ(walk.stations[i].join, SplineStationJoin::Advance);
    }
}

// on a straight run every forward agrees, so no face turns and the sweep's U is the centreline's to the bit
TEST(SplineCornerTests, AStraightRunGainsNoFaceTurnAndKeepsItsUExactly){
    Walk walk = walkPolyline({Vector3(0.0f, 0.0f, 0.0f), Vector3(3.0f, 0.4f, -17.0f)}, 0.5f, 0.5f);
    const SplineStripMesh before = sweepFromZero(wallProfile(1.0f, 3.0f), walk.stations);
    accumulateFaceTurn(walk.stations, UP);
    for (const SplineStation& station : walk.stations){
        EXPECT_EQ(station.faceTurnLeft, 0.0f);
        EXPECT_EQ(station.faceTurnRight, 0.0f);
    }
    const SplineStripMesh after = sweepFromZero(wallProfile(1.0f, 3.0f), walk.stations);
    ASSERT_EQ(before.vertices.size(), after.vertices.size());
    EXPECT_EQ(std::memcmp(before.vertices.data(), after.vertices.data(), before.vertices.size() * sizeof(SplineVertex)), 0);
}

// A mitre lengthens the outer face by halfWidth tan(theta/2) on each leg and shortens the inner one
// as much; with U along each face every band of every side face runs at true scale.
TEST(SplineCornerTests, EachSideFaceOfAMitreTakesUAlongItsOwnLength){
    for (const float thickness : {1.0f, 2.0f}){
        for (const float legB : {10.0f, 7.3f}){
            Walk walk = walkPolyline(elbow(10.0f, legB, 90.0f), 0.5f, thickness * 0.5f);
            ASSERT_TRUE(applyCorners(walk.stations, walk.corners, SplineCornerStyle::Mitre, UP, false).empty());
            accumulateFaceTurn(walk.stations, UP);
            const SplineStripMesh mesh = sweepFromZero(wallProfile(thickness, 3.0f), walk.stations);
            ASSERT_TRUE(mesh.isValid());
            EXPECT_LT(cornerWorstFaceStretch(mesh, walk.stations, false), 1.0e-4f) << "thickness " << thickness << ", second leg " << legB;
        }
    }
}

// a mitre's top takes the centreline U projected onto each leg: the last top band before the mitre
// line runs at true scale along both its arrises
TEST(SplineCornerTests, EachTopEdgeOfAMitreTakesUAlongItsOwnLength){
    for (const float legB : {10.0f, 7.3f}){
        Walk walk = walkPolyline(elbow(10.0f, legB, 90.0f), 0.5f, 0.5f);
        ASSERT_TRUE(applyCorners(walk.stations, walk.corners, SplineCornerStyle::Mitre, UP, false).empty());
        accumulateFaceTurn(walk.stations, UP);
        const SplineStripMesh mesh = sweepFromZero(wallProfile(1.0f, 3.0f), walk.stations);
        ASSERT_TRUE(mesh.isValid());
        EXPECT_LT(cornerWorstFaceStretch(mesh, walk.stations, true), 1.0e-4f) << "second leg " << legB;
    }
}

// A round corner's outside is an arc of the half-width whose length its U covers, and its inside
// pivots on the inner mitre point; past the mitre limit the same holds for the fallback. The top's
// outer edge advances halfWidth times each ring's turn around the fan.
TEST(SplineCornerTests, EachSideFaceOfARoundCornerTakesUAlongItsOwnLength){
    constexpr float halfWidth = 0.3f;
    for (const float turnDegrees : {90.0f, 150.0f}){
        Walk walk = walkPolyline(elbow(10.0f, 7.3f, turnDegrees), 0.5f, halfWidth);
        (void)applyCorners(walk.stations, walk.corners, SplineCornerStyle::Round, UP, false);
        accumulateFaceTurn(walk.stations, UP);
        const SplineStripMesh mesh = sweepFromZero(wallProfile(2.0f * halfWidth, 3.0f), walk.stations);
        ASSERT_TRUE(mesh.isValid());
        EXPECT_LT(cornerWorstFaceStretch(mesh, walk.stations, false), 2.0e-3f) << "turn " << turnDegrees;

        uint32_t rimBands = 0;
        uint32_t joiningBands = 0;
        for (uint32_t r = 0; r + 1u < mesh.ringCount; r++){
            const SplineStation& from = walk.stations[r];
            const SplineStation& to = walk.stations[r + 1u];
            const bool inFan = to.join == SplineStationJoin::Pivot;
            const bool joinsFan = !inFan && (from.mitreSides != 0u || to.mitreSides != 0u);
            if (!inFan && !joinsFan){
                continue;
            }
            for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
                const SplineVertex& a = mesh.vertices[r * mesh.ringVertexCount + k];
                const SplineVertex& b = mesh.vertices[(r + 1u) * mesh.ringVertexCount + k];
                if (a.normal.y < 0.5f){
                    continue;
                }
                const float edge = lengthOf(b.position - a.position);
                const float dU = b.uv.x - a.uv.x;
                if (inFan){
                    if (edge < 1.0e-4f){
                        continue; // the inside pinches on one point
                    }
                    rimBands++;
                    const float ringTurn = std::acos(std::clamp(dotOf(from.forward, to.forward), -1.0f, 1.0f));
                    EXPECT_NEAR(dU, halfWidth * ringTurn, 1.0e-4f) << "turn " << turnDegrees << ", fan band " << r << ", slot " << k;
                }else{
                    joiningBands++;
                    EXPECT_NEAR(dU / edge, 1.0f, 1.0e-4f) << "turn " << turnDegrees << ", band " << r << " joining the fan, slot " << k;
                }
            }
        }
        EXPECT_GT(rimBands, 0u);
        EXPECT_GT(joiningBands, 0u);
        EXPECT_LT(cornerWorstFaceStretch(mesh, walk.stations, true), 2.0e-3f) << "top, turn " << turnDegrees;
        expectWellFormed(mesh, "round corner " + std::to_string(turnDegrees));
    }
}

// on a curve the outer face of a 1 m wall on a 10 m radius is 5 % longer than the centreline and the
// inner 5 % shorter; U along each face tiles both at one size
TEST(SplineCornerTests, BothFacesOfACurveTileAtTheirOwnLength){
    const float radius = 10.0f;
    std::vector<SplineStation> stations(64);
    for (size_t i = 0; i < stations.size(); i++){
        const float angle = 0.5f * PI * static_cast<float>(i) / static_cast<float>(stations.size() - 1u);
        SplineStation& s = stations[i];
        // a right turn about (radius, 0, 0), starting along -Z
        s.position = Vector3(radius - radius * std::cos(angle), 0.0f, -radius * std::sin(angle));
        s.forward = Vector3(std::sin(angle), 0.0f, -std::cos(angle));
        s.right = rightOf(s.forward);
        s.up = crossOf(s.right, s.forward);
        s.halfWidthLeft = 0.5f;
        s.halfWidthRight = 0.5f;
        s.distance = radius * angle;
    }
    accumulateFaceTurn(stations, UP);
    EXPECT_NEAR(stations.back().faceTurnLeft, 0.5f * PI, 1.0e-4f);
    EXPECT_NEAR(stations.back().faceTurnRight, -0.5f * PI, 1.0e-4f);
    const SplineStripMesh mesh = sweepFromZero(wallProfile(1.0f, 3.0f), stations);
    ASSERT_TRUE(mesh.isValid());
    EXPECT_LT(cornerWorstFaceStretch(mesh, stations, false), 1.0e-3f);
    expectWellFormed(mesh, "curved wall");
}

// A welded loop closes each face on itself: a square of four right-turning mitres ends with 8 tan 45
// on the outside and as much off the inside, and with the loop's face snap every side face ends its
// U on a whole tile where it began at zero.
TEST(SplineCornerTests, EachFaceOfAWeldedLoopClosesOnAWholeTile){
    Walk walk = walkPolyline(cornerSquareLoop(6.3f), 0.5f, 0.5f);
    walk.corners.insert(walk.corners.begin(), SplineCornerSite{0u, 0u});
    closeStationLoop(walk.stations, UP);
    ASSERT_TRUE(applyCorners(walk.stations, walk.corners, SplineCornerStyle::Mitre, UP, true).empty());
    accumulateFaceTurn(walk.stations, UP);
    const SplineStation& first = walk.stations.front();
    const SplineStation& last = walk.stations.back();
    EXPECT_EQ(first.faceTurnLeft, 0.0f);
    EXPECT_EQ(first.faceTurnRight, 0.0f);
    EXPECT_NEAR(last.faceTurnLeft, 8.0f, 1.0e-4f);
    EXPECT_NEAR(last.faceTurnRight, -8.0f, 1.0e-4f);

    SplineStripParams params;
    params.uOriginMetres = 0.0f;
    params.generateCaps = false;
    params.loopLengthMetres = last.distance;
    params.loopFaceTurnLeft = last.faceTurnLeft;
    params.loopFaceTurnRight = last.faceTurnRight;
    const SplineStripMesh mesh = buildSplineStrip(wallProfile(1.0f, 3.0f), walk.stations, params);
    ASSERT_TRUE(mesh.isValid());

    const uint32_t lastRing = (mesh.ringCount - 1u) * mesh.ringVertexCount;
    uint32_t sideSlots = 0;
    for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
        const SplineVertex& opening = mesh.vertices[k];
        const SplineVertex& closing = mesh.vertices[lastRing + k];
        if (std::abs(opening.normal.y) > 0.5f){
            continue;
        }
        sideSlots++;
        EXPECT_EQ(opening.uv.x, 0.0f) << "slot " << k;
        EXPECT_NEAR(closing.uv.x, std::round(closing.uv.x), 1.0e-3f) << "slot " << k;
        const bool left = dotOf(opening.position - first.position, first.right) < 0.0f;
        const float faceLength = last.distance + 0.5f * (left ? last.faceTurnLeft : last.faceTurnRight);
        EXPECT_LE(std::abs(closing.uv.x - faceLength), 0.5f) << "slot " << k;
    }
    EXPECT_EQ(sideSlots, 4u);
}

// A 150 degree hairpin past the mitre limit: the inside stands on the inner mitre point, which lies
// on both legs' inside faces, no station survives within the reach and no face folds.
TEST(SplineCornerTests, AHairpinPastTheLimitMeetsBothInsideFacesWithNoSlotOrOverlap){
    const float halfWidth = 0.3f;
    const std::vector<Vector3> points = elbow(10.0f, 7.3f, 150.0f);
    Walk walk = walkPolyline(points, 0.5f, halfWidth);
    const float cornerDistance = walk.stations[walk.corners[0].stationIndex].distance;
    const float reach = halfWidth * std::tan(75.0f * PI / 180.0f);
    (void)applyCorners(walk.stations, walk.corners, SplineCornerStyle::Mitre, UP, false);
    const SplineStripMesh mesh = sweepFromZero(wallProfile(2.0f * halfWidth, 3.0f), walk.stations);
    ASSERT_TRUE(mesh.isValid());

    for (const SplineStation& station : walk.stations){
        const float fromCorner = std::abs(station.distance - cornerDistance);
        EXPECT_TRUE(fromCorner < 1.0e-6f || fromCorner > reach) << "a station " << fromCorner << " m from the corner survives inside its reach";
    }
    uint32_t cornerRings = 0;
    for (uint32_t r = 0; r < mesh.ringCount; r++){
        if (std::abs(walk.stations[r].distance - cornerDistance) > 1.0e-6f){
            continue;
        }
        cornerRings++;
        const RingBase base = baseOf(mesh, r, points[1], walk.stations[r].right);
        EXPECT_NEAR(cornerPlanDistanceToLine(base.right, points[0], points[1] - points[0]), halfWidth, 1.0e-4f)
            << "ring " << r << " inside off the incoming leg's inside face";
        EXPECT_NEAR(cornerPlanDistanceToLine(base.right, points[1], points[2] - points[1]), halfWidth, 1.0e-4f)
            << "ring " << r << " inside off the outgoing leg's inside face";
    }
    EXPECT_GT(cornerRings, 2u);
    EXPECT_EQ(invertedSideTriangles(mesh), 0u);
    EXPECT_EQ(foldedTopTriangles(mesh), 0u);
    expectWellFormed(mesh, "hairpin");
}

// where a leg is shorter than the hairpin's reach, the inside point stands only as far back as that
// leg holds (its length less the 5 cm band); it fits by construction, so no fold is reported
TEST(SplineCornerTests, AHairpinsInsideIsBoundedByAShortLeg){
    const float halfWidth = 0.3f;
    const float shortLeg = 0.8f;
    const std::vector<Vector3> points = elbow(10.0f, shortLeg, 150.0f);
    Walk walk = walkPolyline(points, 0.1f, halfWidth);
    const float cornerDistance = walk.stations[walk.corners[0].stationIndex].distance;
    const std::vector<SplineCornerIssue> issues = applyCorners(walk.stations, walk.corners, SplineCornerStyle::Mitre, UP, false);
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].kind, SplineCornerIssueKind::PastMitreLimit);

    const SplineStripMesh mesh = sweepFromZero(wallProfile(2.0f * halfWidth, 3.0f), walk.stations);
    ASSERT_TRUE(mesh.isValid());
    const float held = shortLeg - 0.05f;
    const Vector3 incoming = unit(points[1] - points[0]);
    const Vector3 outgoing = unit(points[2] - points[1]);
    for (uint32_t r = 0; r < mesh.ringCount; r++){
        if (std::abs(walk.stations[r].distance - cornerDistance) > 1.0e-6f){
            continue;
        }
        const RingBase base = baseOf(mesh, r, points[1], walk.stations[r].right);
        const Vector3 offset(base.right.x - points[1].x, 0.0f, base.right.z - points[1].z);
        EXPECT_NEAR(-dotOf(offset, incoming), held, 1.0e-4f) << "ring " << r;
        EXPECT_NEAR(dotOf(offset, outgoing), held, 1.0e-4f) << "ring " << r;
    }
    EXPECT_EQ(invertedSideTriangles(mesh), 0u);
    EXPECT_EQ(foldedTopTriangles(mesh), 0u);
}

TEST(SplineCornerTests, AHairpinWithBothLegsShortFoldsNoTopTriangle){
    Walk walk = walkPolyline(elbow(0.9f, 0.7f, 150.0f), 0.1f, 0.3f);
    (void)applyCorners(walk.stations, walk.corners, SplineCornerStyle::Mitre, UP, false);
    const SplineStripMesh mesh = sweepFromZero(wallProfile(0.6f, 3.0f), walk.stations);
    ASSERT_TRUE(mesh.isValid());
    EXPECT_EQ(invertedSideTriangles(mesh), 0u);
    EXPECT_EQ(foldedTopTriangles(mesh), 0u);
}

// A curve folds its wall's inside face where its radius is smaller than the inside half-width: on a
// 0.2 m circle every station of a 0.6 m wall folds, on an 8 m circle none does, and a skipped
// station is not measured.
TEST(SplineCornerTests, AStationOnACurveTighterThanTheWallIsAFold){
    const auto circle = [](float radius){
        std::vector<SplineStation> stations(24);
        for (size_t i = 0; i < stations.size(); i++){
            const float angle = 0.5f * PI * static_cast<float>(i) / static_cast<float>(stations.size() - 1u);
            stations[i].position = Vector3(radius - radius * std::cos(angle), 0.0f, -radius * std::sin(angle));
            stations[i].halfWidthLeft = 0.3f;
            stations[i].halfWidthRight = 0.3f;
        }
        return stations;
    };
    const std::vector<SplineStation> tight = circle(0.2f);
    EXPECT_EQ(findInnerFolds(tight, {}, UP).size(), tight.size() - 2u);
    EXPECT_TRUE(findInnerFolds(circle(8.0f), {}, UP).empty());
    std::vector<uint8_t> skip(tight.size(), 1u);
    EXPECT_TRUE(findInnerFolds(tight, skip, UP).empty());

    // the fold reads the inside of the bend: a right turn folds on the right half-width alone
    std::vector<SplineStation> rightOnly = circle(0.2f);
    for (SplineStation& s : rightOnly){
        s.halfWidthRight = 0.1f;
    }
    EXPECT_TRUE(findInnerFolds(rightOnly, {}, UP).empty());
}

// Inner folds become one InnerFold issue per knot interval, naming the knots either side so the
// editor can name them: on an open spline of 4 knots and on a closed one, whose last interval
// ends on knot 0.
TEST(SplineCornerTests, InnerFoldsAreReportedOncePerKnotInterval){
    std::vector<SplineStation> stations(12);
    for (size_t i = 0; i < stations.size(); i++){
        stations[i].splineT = static_cast<float>(i) * 0.25f; // knots 0..2 and the 2.75 tail
    }
    const std::vector<uint32_t> folds = {1u, 2u, 5u, 9u, 10u, 11u};
    const std::vector<SplineCornerIssue> open = innerFoldIssues(stations, folds, 4u, false);
    ASSERT_EQ(open.size(), 3u);
    EXPECT_EQ(open[0].kind, SplineCornerIssueKind::InnerFold);
    EXPECT_EQ(open[0].pointIndex, 0u);
    EXPECT_EQ(open[0].otherPointIndex, 1u);
    EXPECT_EQ(open[1].pointIndex, 1u);
    EXPECT_EQ(open[1].otherPointIndex, 2u);
    EXPECT_EQ(open[2].pointIndex, 2u);
    EXPECT_EQ(open[2].otherPointIndex, 3u);

    // a closed spline of three knots: the interval from knot 2 closes on knot 0
    const std::vector<SplineCornerIssue> closed = innerFoldIssues(stations, folds, 3u, true);
    ASSERT_EQ(closed.size(), 3u);
    EXPECT_EQ(closed[2].pointIndex, 2u);
    EXPECT_EQ(closed[2].otherPointIndex, 0u);

    // a station a builder inserted (no splineT) names nothing
    stations[1].splineT = -1.0f;
    stations[2].splineT = -1.0f;
    EXPECT_EQ(innerFoldIssues(stations, folds, 4u, false).size(), 2u);
}

// A round corner on a welded loop's seam is split across it the way a mitre is: the fan is whole
// (nine rings at 90 degrees, all at the loop's length) and the loop's two end rings coincide.
TEST(SplineCornerTests, AClosedLoopsRoundSeamCornerSplitsAcrossTheSeam){
    Walk walk = walkPolyline(cornerSquareLoop(6.3f), 0.5f, 0.5f);
    walk.corners.insert(walk.corners.begin(), SplineCornerSite{0u, 0u});
    closeStationLoop(walk.stations, UP);
    EXPECT_TRUE(applyCorners(walk.stations, walk.corners, SplineCornerStyle::Round, UP, true).empty());

    const float loopLength = walk.stations.back().distance;
    uint32_t closingFan = 0;
    for (const SplineStation& station : walk.stations){
        closingFan += std::abs(station.distance - loopLength) < 1.0e-5f ? 1u : 0u;
    }
    EXPECT_EQ(closingFan, 9u);
    EXPECT_NEAR(walk.stations.front().forward.z, -1.0f, 1.0e-5f) << "opens looking along the first leg";

    SplineStripParams params;
    params.uOriginMetres = 0.0f;
    params.generateCaps = false;
    const SplineStripMesh mesh = buildSplineStrip(wallProfile(1.0f, 3.0f), walk.stations, params);
    ASSERT_TRUE(mesh.isValid());
    const uint32_t lastRing = (mesh.ringCount - 1u) * mesh.ringVertexCount;
    for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
        EXPECT_TRUE(bitsEqual(mesh.vertices[k].position, mesh.vertices[lastRing + k].position)) << "slot " << k;
    }
    EXPECT_EQ(invertedSideTriangles(mesh), 0u);
}

// a welded loop of round corners carries the top's arc allowance all the way round, and its top
// meets itself at the seam on a whole tile
TEST(SplineCornerTests, TheTopOfARoundCorneredLoopMeetsItselfAtTheSeam){
    Walk walk = walkPolyline(cornerSquareLoop(6.3f), 0.5f, 0.5f);
    walk.corners.insert(walk.corners.begin(), SplineCornerSite{0u, 0u});
    closeStationLoop(walk.stations, UP);
    ASSERT_TRUE(applyCorners(walk.stations, walk.corners, SplineCornerStyle::Round, UP, true).empty());
    accumulateFaceTurn(walk.stations, UP);
    const SplineStation& last = walk.stations.back();
    EXPECT_EQ(walk.stations.front().topArcAllowance, 0.0f);
    EXPECT_NEAR(last.topArcAllowance, 4.0f * 0.5f * 0.5f * PI, 1.0e-4f) << "four quarter arcs of 0.5 m";

    SplineStripParams params;
    params.uOriginMetres = 0.0f;
    params.generateCaps = false;
    params.loopLengthMetres = last.distance;
    params.loopFaceTurnLeft = last.faceTurnLeft;
    params.loopFaceTurnRight = last.faceTurnRight;
    params.loopTopArcAllowance = last.topArcAllowance;
    const SplineStripMesh mesh = buildSplineStrip(wallProfile(1.0f, 3.0f), walk.stations, params);
    ASSERT_TRUE(mesh.isValid());

    const uint32_t lastRing = (mesh.ringCount - 1u) * mesh.ringVertexCount;
    uint32_t topSlots = 0;
    for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
        const SplineVertex& opening = mesh.vertices[k];
        const SplineVertex& closing = mesh.vertices[lastRing + k];
        if (opening.normal.y < 0.5f){
            continue;
        }
        topSlots++;
        const float step = closing.uv.x - opening.uv.x;
        EXPECT_NEAR(step, std::round(step), 1.0e-3f) << "slot " << k;
        EXPECT_GE(std::round(step), 1.0f) << "slot " << k;
    }
    EXPECT_EQ(topSlots, 2u);
    expectWellFormed(mesh, "round cornered loop");
}
