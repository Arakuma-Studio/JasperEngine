// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineLayout/Tests/TileLayoutTests.cpp, Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

// Spline Instantiate (S18 R8, AC7): the station walk (buildTileFrames, ported) and the Spacing
// and Tile modes over a pool (buildTileLayout). OpenEngine is left-handed; every scenario here
// is its mirror (world z negated, and a piece's across-travel local coordinate with it), so the
// ported expectations hold with the sign of every world z flipped.

#include "SplineTileLayout.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    constexpr float TILE_TEST_PI = 3.14159265358979f;

    // a straight run along -Z (the engine's forward), `length` metres in `sampleCount` samples
    std::vector<SplineCenterSample> tileStraightLine(float length, uint32_t sampleCount, Vector3 normal = Vector3(0.0f, 1.0f, 0.0f)){
        std::vector<SplineCenterSample> center;
        for (uint32_t i = 0; i < sampleCount; i++){
            SplineCenterSample s;
            s.position = Vector3(0.0f, 0.0f, -(length * static_cast<float>(i) / static_cast<float>(sampleCount - 1)));
            s.normal = normal;
            center.push_back(s);
        }
        return center;
    }

    std::vector<SplineLayoutPose> tileFrames(const std::vector<SplineCenterSample>& center, const SplineTileFrameParams& params){
        std::vector<SplineLayoutPose> out;
        buildTileFrames(center, params, out);
        return out;
    }

    std::vector<SplinePiecePose> tileLayout(const std::vector<SplineCenterSample>& center, const SplineTileLayoutParams& params){
        std::vector<SplinePiecePose> out;
        buildTileLayout(center, params, out);
        return out;
    }

    void tileExpectOrthonormal(const SplineLayoutPose& pose){
        EXPECT_NEAR(pose.right.dotProduct(pose.right), 1.0f, 1.0e-4f) << "right";
        EXPECT_NEAR(pose.up.dotProduct(pose.up), 1.0f, 1.0e-4f) << "up";
        EXPECT_NEAR(pose.forward.dotProduct(pose.forward), 1.0f, 1.0e-4f) << "forward";
        EXPECT_NEAR(pose.right.dotProduct(pose.up), 0.0f, 1.0e-4f);
        EXPECT_NEAR(pose.right.dotProduct(pose.forward), 0.0f, 1.0e-4f);
        EXPECT_NEAR(pose.up.dotProduct(pose.forward), 0.0f, 1.0e-4f);
        // right-handed as the engine is: right = forward x up
        EXPECT_GT(pose.forward.crossProduct(pose.up).dotProduct(pose.right), 0.9999f);
    }

    // a pool entry from a centre and half extents, pivot at the origin
    SplinePiece tilePiece(uint32_t poolIndex, const Vector3& center, const Vector3& halfExtents, float weight = 1.0f){
        SplinePiece piece;
        piece.poolIndex = poolIndex;
        piece.center = center;
        piece.halfExtents = halfExtents;
        piece.weight = weight;
        return piece;
    }

    // where a mesh-local point lands through an emitted TRS pose
    Vector3 tileToWorld(const SplinePiecePose& pose, const Vector3& local){
        return pose.position + pose.rotation * (pose.scale * local);
    }

    // the piece's run axis as a local unit vector (the axis the layouts lay along forward)
    Vector3 tileRunAxis(const SplinePiece& piece){
        return piece.axis() == SplinePieceAxis::X ? Vector3(1.0f, 0.0f, 0.0f) : Vector3(0.0f, 0.0f, 1.0f);
    }

    void tileExpectNear(const Vector3& a, const Vector3& b, float tolerance, const char* what){
        EXPECT_NEAR(a.x, b.x, tolerance) << what;
        EXPECT_NEAR(a.y, b.y, tolerance) << what;
        EXPECT_NEAR(a.z, b.z, tolerance) << what;
    }

    bool tilePoseBitsEqual(const SplinePiecePose& a, const SplinePiecePose& b){
        return std::memcmp(&a.position, &b.position, sizeof(Vector3)) == 0 && std::memcmp(&a.scale, &b.scale, sizeof(Vector3)) == 0 &&
               a.rotation.w == b.rotation.w && a.rotation.x == b.rotation.x && a.rotation.y == b.rotation.y &&
               a.rotation.z == b.rotation.z && a.poolIndex == b.poolIndex && a.stationIndex == b.stationIndex &&
               a.ordinal == b.ordinal;
    }

}

// ---- Degenerate inputs ---------------------------------------------------------------------

TEST(SplineTileLayoutTests, EmptyAndSingleSampleCenterlinesProduceNoPoses){
    SplineTileFrameParams params;
    EXPECT_TRUE(tileFrames({}, params).empty());

    const std::vector<SplineCenterSample> one(1, SplineCenterSample{Vector3(0, 0, 0), Vector3(0, 1, 0), true});
    EXPECT_TRUE(tileFrames(one, params).empty());
}

TEST(SplineTileLayoutTests, ZeroLengthCenterlineProducesNoPoses){
    const std::vector<SplineCenterSample> center(3, SplineCenterSample{Vector3(5, 0, 5), Vector3(0, 1, 0), true});
    SplineTileFrameParams params;
    EXPECT_TRUE(tileFrames(center, params).empty());
}

TEST(SplineTileLayoutTests, VerticalCenterlineFallsBackToTheEngineForwardWithoutNaNs){
    // a centerline straight up has no ground-plane travel direction at all
    std::vector<SplineCenterSample> center;
    for (uint32_t i = 0; i < 5; i++){
        center.push_back({Vector3(0.0f, static_cast<float>(i), 0.0f), Vector3(0, 1, 0), true});
    }

    SplineTileFrameParams params;
    params.spacing = 1.0f;
    const std::vector<SplineLayoutPose> poses = tileFrames(center, params);
    ASSERT_FALSE(poses.empty());
    for (const SplineLayoutPose& pose : poses){
        tileExpectOrthonormal(pose);
        EXPECT_NEAR(pose.forward.z, -1.0f, 1.0e-4f);
        EXPECT_TRUE(std::isfinite(pose.position.x) && std::isfinite(pose.position.y) && std::isfinite(pose.position.z));
    }
}

// ---- Station generation --------------------------------------------------------------------

TEST(SplineTileLayoutTests, FitToLengthLandsStationsOnBothEnds){
    // length 9, spacing 2: lround(4.5) + 1 = 6 stations, evenly spread 0..9
    const std::vector<SplineCenterSample> center = tileStraightLine(9.0f, 10);
    SplineTileFrameParams params;
    params.spacing = 2.0f;
    params.fit = SplinePlacementFit::FitToLength;
    const std::vector<SplineLayoutPose> poses = tileFrames(center, params);
    ASSERT_EQ(poses.size(), 6u);
    EXPECT_NEAR(poses.front().position.z, 0.0f, 1.0e-4f);
    EXPECT_NEAR(poses.back().position.z, -9.0f, 1.0e-4f);
    for (size_t i = 1; i < poses.size(); i++){
        EXPECT_NEAR(poses[i - 1].position.z - poses[i].position.z, 9.0f / 5.0f, 1.0e-3f);
    }
}

TEST(SplineTileLayoutTests, FixedPitchCutsThePartialStep){
    // length 9, spacing 2: stations at 0, 2, 4, 6, 8; the last partial step dies
    const std::vector<SplineCenterSample> center = tileStraightLine(9.0f, 10);
    SplineTileFrameParams params;
    params.spacing = 2.0f;
    params.fit = SplinePlacementFit::FixedPitch;
    const std::vector<SplineLayoutPose> poses = tileFrames(center, params);
    ASSERT_EQ(poses.size(), 5u);
    for (size_t i = 0; i < poses.size(); i++){
        EXPECT_NEAR(poses[i].position.z, -2.0f * static_cast<float>(i), 1.0e-3f);
    }
}

TEST(SplineTileLayoutTests, MaxTilesClampsBothFitModes){
    const std::vector<SplineCenterSample> center = tileStraightLine(100.0f, 101);
    SplineTileFrameParams params;
    params.spacing = 1.0f;
    params.maxTiles = 4;

    params.fit = SplinePlacementFit::FitToLength;
    EXPECT_EQ(tileFrames(center, params).size(), 4u);
    params.fit = SplinePlacementFit::FixedPitch;
    EXPECT_EQ(tileFrames(center, params).size(), 4u);
}

// ---- Chord yaw -----------------------------------------------------------------------------

TEST(SplineTileLayoutTests, CornerStationYawsAlongTheChordNotTheSegmentTangent){
    // An L in the ground plane: -Z for 10 m, then +X (a right turn) for 10 m. The corner
    // station's forward follows the chord across its own footprint (bisecting the turn at 45
    // degrees), not either segment's tangent.
    std::vector<SplineCenterSample> center;
    for (uint32_t i = 0; i <= 10; i++){
        center.push_back({Vector3(0.0f, 0.0f, -static_cast<float>(i)), Vector3(0, 1, 0), true});
    }
    for (uint32_t i = 1; i <= 10; i++){
        center.push_back({Vector3(static_cast<float>(i), 0.0f, -10.0f), Vector3(0, 1, 0), true});
    }

    SplineTileFrameParams params;
    params.spacing = 2.0f;
    params.fit = SplinePlacementFit::FixedPitch;
    params.meshBoundsHalfExtents = Vector3(1.0f, 0.5f, 1.0f);
    const std::vector<SplineLayoutPose> poses = tileFrames(center, params);

    // the station 10 m in sits on the corner; its chord runs from 9 m to 11 m along the path
    ASSERT_GE(poses.size(), 6u);
    const SplineLayoutPose& corner = poses[5];
    const float inv = 1.0f / std::sqrt(2.0f);
    EXPECT_NEAR(corner.forward.x, inv, 1.0e-3f);
    EXPECT_NEAR(corner.forward.z, -inv, 1.0e-3f);
    // straight-run stations are unaffected
    EXPECT_NEAR(poses[2].forward.z, -1.0f, 1.0e-3f);
    EXPECT_NEAR(poses[2].forward.x, 0.0f, 1.0e-3f);
}

// ---- Surface alignment ---------------------------------------------------------------------

TEST(SplineTileLayoutTests, SlopeBlendInterpolatesUpTowardTheSurfaceNormal){
    const float inv = 1.0f / std::sqrt(2.0f);
    // a 45 degree bank toward +X
    const std::vector<SplineCenterSample> center = tileStraightLine(10.0f, 11, Vector3(inv, inv, 0.0f));

    SplineTileFrameParams params;
    params.spacing = 2.0f;
    params.alignToSurfaceNormal = true;
    // the blend is the subject; the default 35 degree ceiling would hold this bank
    params.maxTiltDegrees = 90.0f;

    params.slopeBlend = 1.0f;
    for (const SplineLayoutPose& pose : tileFrames(center, params)){
        tileExpectOrthonormal(pose);
        EXPECT_NEAR(pose.up.x, inv, 1.0e-3f);
        EXPECT_NEAR(pose.up.y, inv, 1.0e-3f);
    }

    params.slopeBlend = 0.0f;
    for (const SplineLayoutPose& pose : tileFrames(center, params)){
        EXPECT_NEAR(pose.up.y, 1.0f, 1.0e-4f);
    }

    params.alignToSurfaceNormal = false;
    params.slopeBlend = 1.0f;
    for (const SplineLayoutPose& pose : tileFrames(center, params)){
        EXPECT_NEAR(pose.up.y, 1.0f, 1.0e-4f);
    }
}

// ---- Pivot compensation --------------------------------------------------------------------

TEST(SplineTileLayoutTests, CornerPivotedMeshIsFootprintCenteredAndBaseLifted){
    // A corner pivot: bounds centre 1.25 m off the pivot on both horizontal axes, half extents
    // (1.25, 0.5, 1.25), base at y = 0. A Z-run piece's +X is on the left of travel, so its
    // centre at local x = -1.25 is 1.25 m to the RIGHT: the pivot lands 1.25 m left (-X) of the
    // line and 1.25 m back along it; the base lift (halfExtents.y - center.y = 0) vanishes.
    const std::vector<SplineCenterSample> center = tileStraightLine(10.0f, 11);
    SplineTileFrameParams params;
    params.spacing = 5.0f;
    params.meshBoundsCenter = Vector3(-1.25f, 0.5f, 1.25f);
    params.meshBoundsHalfExtents = Vector3(1.25f, 0.5f, 1.25f);

    const std::vector<SplineLayoutPose> poses = tileFrames(center, params);
    ASSERT_EQ(poses.size(), 3u);
    const SplineLayoutPose& mid = poses[1];
    EXPECT_NEAR(mid.position.x, -1.25f, 1.0e-3f);
    EXPECT_NEAR(mid.position.y, 0.0f, 1.0e-3f);
    EXPECT_NEAR(mid.position.z, -(5.0f - 1.25f), 1.0e-3f);

    // centre-pivoted in Y (center.y = 0): lift by halfExtents.y so the lowest vertex lands
    params.meshBoundsCenter = Vector3(0.0f, 0.0f, 0.0f);
    const std::vector<SplineLayoutPose> lifted = tileFrames(center, params);
    ASSERT_EQ(lifted.size(), 3u);
    EXPECT_NEAR(lifted[1].position.y, 0.5f, 1.0e-3f);
}

// ---- Conform-ray gaps ----------------------------------------------------------------------
//
// Stations come from the 3D length of the draped polyline, so a sample that kept its authored
// altitude because no ray hit anything is measured as length: a 40 m centerline authored 20 m
// above flat ground, draped to y = 0 except one interior miss, measures 79.02 m and doubles the
// piece count.

namespace{

    std::vector<SplineCenterSample> tileDrapedWithGaps(const std::vector<size_t>& gapIndices, float groundY, bool markGapsUnmeasured){
        constexpr float length = 40.0f;
        // centerlineSampleCount(40 m)
        constexpr uint32_t denseSamples = 82;
        constexpr float authoredAltitude = 20.0f;

        std::vector<SplineCenterSample> center = tileStraightLine(length, denseSamples);
        for (SplineCenterSample& s : center){
            s.position.y = groundY;
        }
        for (const size_t i : gapIndices){
            center[i].position.y = authoredAltitude;
            center[i].surfaceValid = !markGapsUnmeasured;
        }
        return center;
    }

}

TEST(SplineTileLayoutTests, OneConformGapDoesNotReparameteriseTheSpline){
    SplineTileFrameParams params;
    params.spacing = 2.0f;
    params.fit = SplinePlacementFit::FitToLength;

    const std::vector<SplineLayoutPose> flat = tileFrames(tileDrapedWithGaps({}, 0.0f, true), params);
    ASSERT_EQ(flat.size(), 21u);

    // a spike the caller declares measured IS measured: 40.00 m of drape becomes 79.02 m
    const std::vector<SplineLayoutPose> measuredSpike = tileFrames(tileDrapedWithGaps({40}, 0.0f, false), params);
    EXPECT_EQ(measuredSpike.size(), 41u);

    // declared unmeasured, the same sample holds the altitude before it
    const std::vector<SplineLayoutPose> held = tileFrames(tileDrapedWithGaps({40}, 0.0f, true), params);
    ASSERT_EQ(held.size(), flat.size());
    for (size_t i = 0; i < held.size(); i++){
        EXPECT_NEAR(held[i].position.z, flat[i].position.z, 1.0e-3f) << "station " << i;
        EXPECT_NEAR(held[i].position.y, 0.0f, 1.0e-3f) << "station " << i;
    }
}

TEST(SplineTileLayoutTests, RunsOfConformGapsHoldTheNearestMeasuredAltitude){
    SplineTileFrameParams params;
    params.spacing = 2.0f;
    params.fit = SplinePlacementFit::FitToLength;

    constexpr float ground = -1.5f;
    const std::vector<SplineLayoutPose> flat = tileFrames(tileDrapedWithGaps({}, ground, true), params);
    ASSERT_EQ(flat.size(), 21u);

    // a leading gap back-fills from the first measured sample; trailing and interior runs hold
    // the measured run before them
    const std::vector<std::vector<size_t>> gapRuns = {{0, 1, 2}, {79, 80, 81}, {30, 31, 32, 33}, {0, 40, 81}};
    for (const std::vector<size_t>& gaps : gapRuns){
        const std::vector<SplineLayoutPose> held = tileFrames(tileDrapedWithGaps(gaps, ground, true), params);
        ASSERT_EQ(held.size(), flat.size()) << "first gap index " << gaps.front();
        for (size_t i = 0; i < held.size(); i++){
            EXPECT_NEAR(held[i].position.z, flat[i].position.z, 1.0e-3f) << "station " << i;
            EXPECT_NEAR(held[i].position.y, ground, 1.0e-3f) << "station " << i;
        }
    }
}

TEST(SplineTileLayoutTests, ACenterlineWithNoMeasuredSampleKeepsItsAuthoredAltitudes){
    // conform found nothing anywhere: the authored altitudes are the only surface there is
    std::vector<SplineCenterSample> authored = tileStraightLine(40.0f, 82);
    for (SplineCenterSample& s : authored){
        s.position.y = -0.25f * s.position.z;
    }

    std::vector<SplineCenterSample> unmeasured = authored;
    for (SplineCenterSample& s : unmeasured){
        s.surfaceValid = false;
    }

    SplineTileFrameParams params;
    params.spacing = 2.0f;
    params.fit = SplinePlacementFit::FitToLength;

    const std::vector<SplineLayoutPose> measured = tileFrames(authored, params);
    const std::vector<SplineLayoutPose> held = tileFrames(unmeasured, params);
    ASSERT_FALSE(held.empty());
    ASSERT_EQ(held.size(), measured.size());
    for (size_t i = 0; i < held.size(); i++){
        EXPECT_NEAR(held[i].position.y, measured[i].position.y, 1.0e-4f) << "station " << i;
        EXPECT_NEAR(held[i].position.z, measured[i].position.z, 1.0e-4f) << "station " << i;
    }
}

// ---- Station surface probe -----------------------------------------------------------------

namespace{

    // ground height as a function of the distance travelled along -Z
    using TileGroundProfile = std::function<float(float along)>;

    // a centerline dense-sampled at the pinned step and draped onto a ground profile
    std::vector<SplineCenterSample> tileDrapedAlong(float length, float step, const TileGroundProfile& ground){
        const uint32_t count = static_cast<uint32_t>(length / step) + 1u;
        std::vector<SplineCenterSample> center;
        center.reserve(count);
        for (uint32_t i = 0; i < count; i++){
            const float along = static_cast<float>(i) * step;
            center.push_back(SplineCenterSample{Vector3(0.0f, ground(along), -along), Vector3(0.0f, 1.0f, 0.0f), true});
        }
        return center;
    }

    SplineStationProbe tileProbeOf(const TileGroundProfile& ground){
        return [ground](const Vector3& at, float& outAltitude, Vector3& outNormal){
            outAltitude = ground(-at.z);
            outNormal = Vector3(0.0f, 1.0f, 0.0f);
            return true;
        };
    }

    float tileMaxAltitudeError(const std::vector<SplineLayoutPose>& poses, const TileGroundProfile& ground){
        float worst = 0.0f;
        for (const SplineLayoutPose& pose : poses){
            worst = std::max(worst, std::fabs(pose.position.y - ground(-pose.position.z)));
        }
        return worst;
    }

    // a 0.30 m kerb, sited between the dense samples at 1.0 and 1.5 m along
    float tileKerbGround(float along){
        return along < 1.3f ? 0.0f : 0.30f;
    }

}

TEST(SplineTileLayoutTests, InterpolatedStationsCutAcrossAKerbAndTheProbeDoesNot){
    // the station at arc 1.3 lands 1.2572 m along, still short of the kerb, but the chord it
    // interpolates has already climbed 51.4% of the riser: 0.1543 m the ground does not have
    const TileGroundProfile kerb = tileKerbGround;
    const std::vector<SplineCenterSample> center = tileDrapedAlong(40.0f, 0.5f, kerb);

    SplineTileFrameParams params;
    params.spacing = 1.3f;
    params.fit = SplinePlacementFit::FixedPitch;

    const std::vector<SplineLayoutPose> interpolated = tileFrames(center, params);
    ASSERT_FALSE(interpolated.empty());
    EXPECT_NEAR(tileMaxAltitudeError(interpolated, kerb), 0.1543f, 2.0e-3f);

    const SplineStationProbe probe = tileProbeOf(kerb);
    params.probe = &probe;
    const std::vector<SplineLayoutPose> probed = tileFrames(center, params);
    EXPECT_LT(tileMaxAltitudeError(probed, kerb), 1.0e-4f);

    // the probe re-reads altitude only: the count and every station's XZ are untouched
    ASSERT_EQ(probed.size(), interpolated.size());
    for (size_t i = 0; i < probed.size(); i++){
        EXPECT_NEAR(probed[i].position.x, interpolated[i].position.x, 1.0e-6f) << "station " << i;
        EXPECT_NEAR(probed[i].position.z, interpolated[i].position.z, 1.0e-6f) << "station " << i;
    }
}

TEST(SplineTileLayoutTests, InterpolatedStationsSagBelowSmoothGroundAndTheProbeDoesNot){
    // nothing sharp: curvature alone leaves the chord between dense samples ~0.014 m off
    const TileGroundProfile rolling = [](float along){ return 0.4f * std::sin(along * (2.0f * 3.14159265f / 6.0f)); };
    const std::vector<SplineCenterSample> center = tileDrapedAlong(40.0f, 0.5f, rolling);

    SplineTileFrameParams params;
    params.spacing = 1.3f;
    params.fit = SplinePlacementFit::FixedPitch;

    const std::vector<SplineLayoutPose> interpolated = tileFrames(center, params);
    ASSERT_FALSE(interpolated.empty());
    EXPECT_GT(tileMaxAltitudeError(interpolated, rolling), 0.005f);

    const SplineStationProbe probe = tileProbeOf(rolling);
    params.probe = &probe;
    EXPECT_LT(tileMaxAltitudeError(tileFrames(center, params), rolling), 1.0e-4f);
}

TEST(SplineTileLayoutTests, StationProbeSuppliesTheNormalTheTileTiltsTo){
    const std::vector<SplineCenterSample> center = tileStraightLine(10.0f, 21);
    const float inv = 1.0f / std::sqrt(2.0f);

    SplineTileFrameParams params;
    params.spacing = 2.0f;
    params.alignToSurfaceNormal = true;
    params.slopeBlend = 1.0f;
    params.maxTiltDegrees = 90.0f;
    const SplineStationProbe probe = [inv](const Vector3&, float& outAltitude, Vector3& outNormal){
        outAltitude = 3.0f;
        outNormal = Vector3(inv, inv, 0.0f);
        return true;
    };
    params.probe = &probe;

    const std::vector<SplineLayoutPose> poses = tileFrames(center, params);
    ASSERT_FALSE(poses.empty());
    for (const SplineLayoutPose& pose : poses){
        tileExpectOrthonormal(pose);
        EXPECT_NEAR(pose.up.x, inv, 1.0e-3f);
        EXPECT_NEAR(pose.up.y, inv, 1.0e-3f);
        EXPECT_NEAR(pose.position.y, 3.0f, 1.0e-3f);
    }
}

TEST(SplineTileLayoutTests, AStationProbeMissLeavesThatTileOnTheInterpolatedCenterline){
    // a probe miss is a hole under one piece: it costs that piece its correction and nothing else
    const TileGroundProfile kerb = tileKerbGround;
    const std::vector<SplineCenterSample> center = tileDrapedAlong(40.0f, 0.5f, kerb);

    SplineTileFrameParams params;
    params.spacing = 1.3f;
    params.fit = SplinePlacementFit::FixedPitch;
    const std::vector<SplineLayoutPose> interpolated = tileFrames(center, params);

    const SplineStationProbe missAll = [](const Vector3&, float&, Vector3&){ return false; };
    params.probe = &missAll;
    const std::vector<SplineLayoutPose> allMissed = tileFrames(center, params);
    ASSERT_EQ(allMissed.size(), interpolated.size());
    for (size_t i = 0; i < allMissed.size(); i++){
        EXPECT_NEAR(allMissed[i].position.y, interpolated[i].position.y, 1.0e-6f) << "station " << i;
    }

    const SplineStationProbe holeOverKerb = [](const Vector3& at, float& outAltitude, Vector3&){
        if (-at.z < 2.0f){
            return false;
        }
        outAltitude = tileKerbGround(-at.z);
        return true;
    };
    params.probe = &holeOverKerb;
    const std::vector<SplineLayoutPose> partial = tileFrames(center, params);
    ASSERT_EQ(partial.size(), interpolated.size());
    EXPECT_NEAR(tileMaxAltitudeError(partial, kerb), 0.1543f, 2.0e-3f);
}

// ---- Caller-supplied stations, yaw and plant mode ------------------------------------------

TEST(SplineTileLayoutTests, CallerSuppliedStationDistancesWinOverSpacingAndFit){
    const std::vector<SplineCenterSample> center = tileStraightLine(10.0f, 21u);
    const float stations[4] = {0.0f, 1.0f, 6.5f, 10.0f};

    SplineTileFrameParams params;
    params.spacing = 2.0f;
    params.fit = SplinePlacementFit::FitToLength;
    params.stationDistances = stations;

    const std::vector<SplineLayoutPose> poses = tileFrames(center, params);
    ASSERT_EQ(poses.size(), 4u);
    for (size_t i = 0; i < 4u; i++){
        EXPECT_NEAR(poses[i].position.z, -stations[i], 1.0e-3f) << "station " << i;
        EXPECT_NEAR(poses[i].base.z, -stations[i], 1.0e-3f) << "station " << i;
        tileExpectOrthonormal(poses[i]);
    }
}

// base is the conformed centerline point BEFORE footprint centering and planting move the pivot
TEST(SplineTileLayoutTests, BaseIsTheCenterlinePointAndPositionIsTheMovedPivot){
    const std::vector<SplineCenterSample> center = tileStraightLine(10.0f, 21u);
    SplineTileFrameParams params;
    params.spacing = 5.0f;
    params.meshBoundsCenter = Vector3(-1.2f, 0.5f, 0.4f);
    params.meshBoundsHalfExtents = Vector3(1.2f, 0.5f, 1.2f);

    const std::vector<SplineLayoutPose> poses = tileFrames(center, params);
    ASSERT_FALSE(poses.empty());
    for (const SplineLayoutPose& pose : poses){
        EXPECT_NEAR(pose.base.x, 0.0f, 1.0e-4f);
        EXPECT_NEAR(pose.base.y, 0.0f, 1.0e-4f);
        // BoundsMin lifts by halfExtents.y - center.y = 0
        EXPECT_NEAR(pose.position.y, 0.0f, 1.0e-4f);
        EXPECT_NEAR(pose.position.x, -1.2f, 1.0e-4f);
        EXPECT_NEAR(pose.position.z - pose.base.z, 0.4f, 1.0e-4f);
    }
}

// Pivot lands the pivot's plane on the surface and lets a planting skirt bury; BoundsMin floats
// that same mesh by the depth of its skirt
TEST(SplineTileLayoutTests, PlantModeChoosesWhichPlaneMeetsTheGround){
    const std::vector<SplineCenterSample> center = tileStraightLine(10.0f, 21u);
    SplineTileFrameParams params;
    params.spacing = 5.0f;
    // bounds min y = 0.24 - 0.76 = -0.52: a measured fence panel's skirt
    params.meshBoundsCenter = Vector3(0.0f, 0.24f, 0.0f);
    params.meshBoundsHalfExtents = Vector3(0.1f, 0.76f, 0.1f);

    params.plantMode = SplinePlantMode::BoundsMin;
    const std::vector<SplineLayoutPose> lifted = tileFrames(center, params);
    ASSERT_FALSE(lifted.empty());
    EXPECT_NEAR(lifted[0].position.y, 0.52f, 1.0e-4f);

    params.plantMode = SplinePlantMode::Pivot;
    const std::vector<SplineLayoutPose> planted = tileFrames(center, params);
    ASSERT_EQ(planted.size(), lifted.size());
    EXPECT_NEAR(planted[0].position.y, 0.0f, 1.0e-4f);
}

// The proud-paving defect on the two slabs it was measured on, both authored about a mid-plane:
// BoundsMin lifts the WHOLE slab clear of the ground, Pivot leaves only what is above the pivot.
TEST(SplineTileLayoutTests, MeasuredPavingSlabStandsItsFullThicknessProudUntilPivotPlant){
    struct Slab{
        const char* name;
        float minY;
        float maxY;
        float halfX;
        float halfZ;
        float expectedProudMetres;
    };
    const Slab slabs[] = {
        {"SM_Env_Footpath_01", -0.1908f, 0.0758f, 1.3121f, 1.1878f, 0.076f},
        {"SM_Env_Footpath_02", -0.1857f, 0.0969f, 1.3121f, 1.1878f, 0.097f},
    };

    for (const Slab& slab : slabs){
        const std::vector<SplineCenterSample> center = tileStraightLine(10.0f, 21u);
        SplineTileFrameParams params;
        params.spacing = 2.5f;
        params.meshBoundsCenter = Vector3(0.0f, (slab.minY + slab.maxY) * 0.5f, 0.0f);
        params.meshBoundsHalfExtents = Vector3(slab.halfX, (slab.maxY - slab.minY) * 0.5f, slab.halfZ);

        params.plantMode = SplinePlantMode::BoundsMin;
        const std::vector<SplineLayoutPose> lifted = tileFrames(center, params);
        ASSERT_FALSE(lifted.empty()) << slab.name;

        params.plantMode = SplinePlantMode::Pivot;
        const std::vector<SplineLayoutPose> planted = tileFrames(center, params);
        ASSERT_EQ(planted.size(), lifted.size()) << slab.name;

        // ground is y = 0 here, so a pose's y IS its pivot's height above ground
        EXPECT_NEAR(lifted[0].position.y, -slab.minY, 1.0e-4f) << slab.name << ": BoundsMin no longer lands the lowest vertex";
        EXPECT_NEAR(lifted[0].position.y + slab.maxY, slab.maxY - slab.minY, 1.0e-4f) << slab.name;
        EXPECT_NEAR(planted[0].position.y, 0.0f, 1.0e-4f) << slab.name << ": Pivot no longer lands the pivot on the surface";
        // the tolerance is the reported figure's own rounding half-width (3 dp)
        EXPECT_NEAR(planted[0].position.y + slab.maxY, slab.expectedProudMetres, 5.0e-4f) << slab.name;
        EXPECT_NEAR(lifted[0].position.y - planted[0].position.y, -slab.minY, 1.0e-4f) << slab.name;
    }
}

// a caller-supplied direction overrides the chord for that station only; zero means "no opinion"
TEST(SplineTileLayoutTests, StationForwardOverridesReplaceOnlyTheStationsTheyName){
    const std::vector<SplineCenterSample> center = tileStraightLine(10.0f, 21u);
    const float stations[3] = {0.0f, 5.0f, 10.0f};
    const Vector3 forwards[3] = {Vector3(0, 0, 0), Vector3(1.0f, 0.0f, -1.0f), Vector3(0, 0, 0)};

    SplineTileFrameParams params;
    params.spacing = 5.0f;
    params.stationDistances = stations;
    params.stationForwards = forwards;

    const std::vector<SplineLayoutPose> poses = tileFrames(center, params);
    ASSERT_EQ(poses.size(), 3u);
    EXPECT_NEAR(poses[0].forward.z, -1.0f, 1.0e-4f);
    EXPECT_NEAR(poses[1].forward.x, 0.70710678f, 1.0e-3f);
    EXPECT_NEAR(poses[1].forward.z, -0.70710678f, 1.0e-3f);
    EXPECT_NEAR(poses[2].forward.z, -1.0f, 1.0e-4f);
    for (const SplineLayoutPose& pose : poses){
        tileExpectOrthonormal(pose);
    }
}

// ---- Which local axis the tile is laid along -----------------------------------------------

// every measured path tile is near-square (2.62 x 2.38 m) and keeps the Z convention
TEST(SplineTileLayoutTests, NearSquareTileKeepsTheZConventionForChordAndCentering){
    const std::vector<SplineCenterSample> center = tileStraightLine(10.0f, 21u);
    SplineTileFrameParams params;
    params.spacing = 2.5f;
    // SM_Env_Footpath_01, measured: 2.6242 x 0.2666 x 2.3756 m, corner-pivoted
    params.meshBoundsCenter = Vector3(1.2435f, -0.0575f, -1.2429f);
    params.meshBoundsHalfExtents = Vector3(1.3121f, 0.1333f, 1.1878f);
    params.plantMode = SplinePlantMode::Pivot;

    ASSERT_EQ(choosePieceAxis(params.meshBoundsHalfExtents), SplinePieceAxis::Z);

    const std::vector<SplineLayoutPose> poses = tileFrames(center, params);
    ASSERT_FALSE(poses.empty());
    for (const SplineLayoutPose& pose : poses){
        // local +X is on the left (-X world travelling -Z), local +Z on forward
        EXPECT_NEAR(pose.position.x, 1.2435f, 1.0e-4f);
        EXPECT_NEAR(pose.position.z - pose.base.z, -1.2429f, 1.0e-4f);
    }
}

// an X-long piece is laid along its long axis, and the chord window follows the same axis
TEST(SplineTileLayoutTests, LongTileIsCenteredAlongItsOwnAxes){
    const std::vector<SplineCenterSample> center = tileStraightLine(10.0f, 21u);
    SplineTileFrameParams params;
    params.spacing = 5.0f;
    // long in X, thin in Z, corner-pivoted on both
    params.meshBoundsCenter = Vector3(2.0f, 0.5f, -0.25f);
    params.meshBoundsHalfExtents = Vector3(2.0f, 0.5f, 0.25f);
    params.plantMode = SplinePlantMode::Pivot;

    ASSERT_EQ(choosePieceAxis(params.meshBoundsHalfExtents), SplinePieceAxis::X);

    const std::vector<SplineLayoutPose> poses = tileFrames(center, params);
    ASSERT_FALSE(poses.empty());
    for (const SplineLayoutPose& pose : poses){
        // local +X on forward (-Z world), local +Z on right (+X world): center.x is cancelled
        // along travel and center.z across it
        EXPECT_NEAR(pose.position.z - pose.base.z, 2.0f, 1.0e-4f);
        EXPECT_NEAR(pose.position.x, 0.25f, 1.0e-4f);
    }
}

// ---- normalizedOrFallback: the shared normalize every pose is built from -----------------

TEST(SplineTileLayoutTests, NormalizedOrFallbackRejectsNaNInput){
    const Vector3 fallback(0.0f, 0.0f, 1.0f);
    const float nan = std::numeric_limits<float>::quiet_NaN();

    const Vector3 allNaN = normalizedOrFallback(Vector3(nan, nan, nan), fallback);
    EXPECT_FLOAT_EQ(allNaN.x, fallback.x);
    EXPECT_FLOAT_EQ(allNaN.y, fallback.y);
    EXPECT_FLOAT_EQ(allNaN.z, fallback.z);

    // one poisoned component is enough: the dot product spreads it
    const Vector3 oneNaN = normalizedOrFallback(Vector3(1.0f, nan, 0.0f), fallback);
    EXPECT_FLOAT_EQ(oneNaN.x, fallback.x);
    EXPECT_FLOAT_EQ(oneNaN.y, fallback.y);
    EXPECT_FLOAT_EQ(oneNaN.z, fallback.z);
}

TEST(SplineTileLayoutTests, NormalizedOrFallbackRejectsInfiniteInput){
    const Vector3 fallback(0.0f, 1.0f, 0.0f);
    const float inf = std::numeric_limits<float>::infinity();

    const Vector3 positive = normalizedOrFallback(Vector3(inf, 0.0f, 0.0f), fallback);
    EXPECT_FLOAT_EQ(positive.x, fallback.x);
    EXPECT_FLOAT_EQ(positive.y, fallback.y);
    EXPECT_FLOAT_EQ(positive.z, fallback.z);

    const Vector3 negative = normalizedOrFallback(Vector3(0.0f, 0.0f, -inf), fallback);
    EXPECT_FLOAT_EQ(negative.x, fallback.x);
    EXPECT_FLOAT_EQ(negative.y, fallback.y);
    EXPECT_FLOAT_EQ(negative.z, fallback.z);
}

// a finite vector whose squared length overflows reaches the normalize as an infinite lenSq
TEST(SplineTileLayoutTests, NormalizedOrFallbackRejectsOverflowingMagnitude){
    const Vector3 fallback(1.0f, 0.0f, 0.0f);
    const Vector3 huge(1.0e30f, 1.0e30f, 1.0e30f);
    ASSERT_TRUE(std::isfinite(huge.x));

    const Vector3 result = normalizedOrFallback(huge, fallback);
    EXPECT_FLOAT_EQ(result.x, fallback.x);
    EXPECT_FLOAT_EQ(result.y, fallback.y);
    EXPECT_FLOAT_EQ(result.z, fallback.z);
}

TEST(SplineTileLayoutTests, NormalizedOrFallbackNormalizesHealthyInput){
    const Vector3 fallback(0.0f, 0.0f, 1.0f);

    const Vector3 axis = normalizedOrFallback(Vector3(0.0f, 5.0f, 0.0f), fallback);
    EXPECT_NEAR(axis.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(axis.y, 1.0f, 1.0e-6f);
    EXPECT_NEAR(axis.z, 0.0f, 1.0e-6f);

    const Vector3 diagonal = normalizedOrFallback(Vector3(3.0f, 0.0f, 4.0f), fallback);
    EXPECT_NEAR(diagonal.x, 0.6f, 1.0e-6f);
    EXPECT_NEAR(diagonal.z, 0.8f, 1.0e-6f);
    EXPECT_NEAR(diagonal.dotProduct(diagonal), 1.0f, 1.0e-6f);
}

TEST(SplineTileLayoutTests, NormalizedOrFallbackFallsBackOnZeroLength){
    const Vector3 fallback(0.0f, 0.0f, 1.0f);

    const Vector3 zero = normalizedOrFallback(Vector3(0.0f, 0.0f, 0.0f), fallback);
    EXPECT_FLOAT_EQ(zero.x, fallback.x);
    EXPECT_FLOAT_EQ(zero.y, fallback.y);
    EXPECT_FLOAT_EQ(zero.z, fallback.z);

    // just under the 1e-8 squared-length threshold
    const Vector3 tiny = normalizedOrFallback(Vector3(0.0f, 9.0e-5f, 0.0f), fallback);
    EXPECT_FLOAT_EQ(tiny.y, fallback.y);
    EXPECT_FLOAT_EQ(tiny.z, fallback.z);
}

// ---- Tilt ceiling on steep ground ----------------------------------------------------------
//
// A path crossing a near-vertical crest folded into standing sheets: a piece's up came straight
// from the surface normal, and since right is crossed against up, an up leaning 88 degrees turns
// the WIDTH axis skyward. The ridge runs oblique to the path, as ordinary terrain does: right's
// verticality is set by the normal's cross-slope against its vertical component.

namespace{

    constexpr float TILE_CREST_OBLIQUITY = 0.28f;

    // a path along -Z whose ground climbs `rise` metres across a face `halfWidth` wide 10 m in
    std::vector<SplineCenterSample> tileCrestCrossing(float halfWidth, float rise, uint32_t count = 161u){
        std::vector<SplineCenterSample> center;
        for (uint32_t i = 0; i < count; i++){
            const float along = 20.0f * static_cast<float>(i) / static_cast<float>(count - 1u);
            const float u = (along - (10.0f - halfWidth)) / (2.0f * halfWidth);
            float y = rise;
            float slope = 0.0f;
            if (u <= 0.0f){
                y = 0.0f;
            }else if (u < 1.0f){
                y = rise * (u * u * (3.0f - 2.0f * u));
                slope = rise * (6.0f * u * (1.0f - u)) / (2.0f * halfWidth);
            }
            // moving the crest line by obliquity * dx moves the whole face
            const float dydx = -slope * TILE_CREST_OBLIQUITY;
            SplineCenterSample s;
            s.position = Vector3(0.0f, y, -along);
            // the heightfield gradient along -Z is -slope per metre of z
            s.normal = normalizedOrFallback(Vector3(-dydx, 1.0f, slope), Vector3(0, 1, 0));
            center.push_back(s);
        }
        return center;
    }

    float tileAngleDegrees(const Vector3& a, const Vector3& b){
        return std::acos(std::clamp(a.dotProduct(b), -1.0f, 1.0f)) * 180.0f / TILE_TEST_PI;
    }

    float tileTiltFromWorldUp(const Vector3& up){ return tileAngleDegrees(up, Vector3(0.0f, 1.0f, 0.0f)); }

    SplineTileFrameParams tileSteepCrestParams(float maxTiltDegrees){
        SplineTileFrameParams params;
        params.spacing = 2.0f;
        params.fit = SplinePlacementFit::FitToLength;
        params.alignToSurfaceNormal = true;
        params.slopeBlend = 1.0f;
        params.maxTiltDegrees = maxTiltDegrees;
        params.meshBoundsHalfExtents = Vector3(1.2f, 0.05f, 1.2f);
        return params;
    }

}

TEST(SplineTileLayoutTests, SteepCrestWithoutTheCeilingStandsTilesOnEdge){
    // the mechanism's witness: 90 degrees is "no ceiling"
    const std::vector<SplineCenterSample> center = tileCrestCrossing(0.2f, 12.0f);
    const std::vector<SplineLayoutPose> poses = tileFrames(center, tileSteepCrestParams(90.0f));
    ASSERT_GE(poses.size(), 8u);

    float worstTilt = 0.0f;
    for (const SplineLayoutPose& pose : poses){
        worstTilt = std::max(worstTilt, tileTiltFromWorldUp(pose.up));
    }
    EXPECT_GT(worstTilt, 80.0f) << "fixture must actually present near-vertical ground";

    float mostVerticalRight = 0.0f;
    for (const SplineLayoutPose& pose : poses){
        mostVerticalRight = std::max(mostVerticalRight, std::abs(pose.right.y));
    }
    EXPECT_GT(mostVerticalRight, std::cos(20.0f * TILE_TEST_PI / 180.0f)) << "unbounded blend must put some width axis near vertical";
}

TEST(SplineTileLayoutTests, TiltCeilingHoldsTilesUprightOnASteepCrest){
    const std::vector<SplineCenterSample> center = tileCrestCrossing(0.2f, 12.0f);
    const std::vector<SplineLayoutPose> poses = tileFrames(center, tileSteepCrestParams(35.0f));
    ASSERT_GE(poses.size(), 8u);

    for (const SplineLayoutPose& pose : poses){
        tileExpectOrthonormal(pose);
        EXPECT_LE(tileTiltFromWorldUp(pose.up), 35.0f + 1.0e-2f);
        EXPECT_LE(std::abs(pose.right.y), std::sin(36.0f * TILE_TEST_PI / 180.0f));
    }
}

TEST(SplineTileLayoutTests, TiltCeilingBoundsTheFrameStepBetweenAdjacentStations){
    // two stations may differ by twice the ceiling (held at +35 and -35 across a ridge line)
    const std::vector<SplineCenterSample> center = tileCrestCrossing(0.2f, 12.0f);
    const std::vector<SplineLayoutPose> poses = tileFrames(center, tileSteepCrestParams(35.0f));
    ASSERT_GE(poses.size(), 8u);

    for (size_t i = 1; i < poses.size(); i++){
        EXPECT_LE(tileAngleDegrees(poses[i].up, poses[i - 1].up), 71.0f) << "up step at station " << i;
        EXPECT_LE(tileAngleDegrees(poses[i].right, poses[i - 1].right), 71.0f) << "right step at station " << i;
        EXPECT_GT(poses[i].right.dotProduct(poses[i - 1].right), 0.0f) << "right reversed between stations " << i - 1 << " and " << i;
    }
}

TEST(SplineTileLayoutTests, TiltCeilingReportsWhatItHeldAndTheSteepestGroundSeen){
    const std::vector<SplineCenterSample> center = tileCrestCrossing(0.2f, 12.0f);
    SplineTileFrameParams params = tileSteepCrestParams(35.0f);
    SplineTiltClampReport report;
    params.outTiltClamp = &report;
    const std::vector<SplineLayoutPose> poses = tileFrames(center, params);
    ASSERT_FALSE(poses.empty());

    EXPECT_GT(report.clampedStations, 0u);
    EXPECT_LT(report.clampedStations, poses.size()) << "flat stations either side of the face must not be counted";
    EXPECT_GT(report.steepestDegrees, 80.0f);
    EXPECT_NEAR(report.steepestAt.z, -10.0f, 1.5f);
}

TEST(SplineTileLayoutTests, GentleSlopesArePoseIdenticalWithAndWithoutTheCeiling){
    // ground within the ceiling must be untouched, bit for bit
    const std::vector<SplineCenterSample> center = tileCrestCrossing(6.0f, 2.0f);
    const std::vector<SplineLayoutPose> held = tileFrames(center, tileSteepCrestParams(35.0f));
    const std::vector<SplineLayoutPose> unheld = tileFrames(center, tileSteepCrestParams(90.0f));
    ASSERT_EQ(held.size(), unheld.size());
    ASSERT_FALSE(held.empty());

    for (size_t i = 0; i < held.size(); i++){
        EXPECT_FLOAT_EQ(held[i].up.x, unheld[i].up.x) << "station " << i;
        EXPECT_FLOAT_EQ(held[i].up.y, unheld[i].up.y) << "station " << i;
        EXPECT_FLOAT_EQ(held[i].up.z, unheld[i].up.z) << "station " << i;
        EXPECT_FLOAT_EQ(held[i].right.x, unheld[i].right.x) << "station " << i;
        EXPECT_FLOAT_EQ(held[i].right.y, unheld[i].right.y) << "station " << i;
        EXPECT_FLOAT_EQ(held[i].right.z, unheld[i].right.z) << "station " << i;
        EXPECT_FLOAT_EQ(held[i].position.y, unheld[i].position.y) << "station " << i;
    }
}

TEST(SplineTileLayoutTests, TheChordCollapsesOnASteepFaceWithoutReversingTheFrame){
    // the control that falsifies the chord as the mechanism: only the up-blend is off
    const std::vector<SplineCenterSample> center = tileCrestCrossing(0.2f, 12.0f);
    SplineTileFrameParams params = tileSteepCrestParams(90.0f);
    params.alignToSurfaceNormal = false;
    const std::vector<SplineLayoutPose> poses = tileFrames(center, params);
    ASSERT_GE(poses.size(), 8u);

    for (size_t i = 0; i < poses.size(); i++){
        tileExpectOrthonormal(poses[i]);
        EXPECT_NEAR(poses[i].up.y, 1.0f, 1.0e-4f);
        if (i == 0){
            continue;
        }
        EXPECT_GT(poses[i].right.dotProduct(poses[i - 1].right), 0.0f);
        EXPECT_LE(tileAngleDegrees(poses[i].forward, poses[i - 1].forward), 15.0f);
    }
}

TEST(SplineTileLayoutTests, ClampTiltFromWorldUpKeepsAzimuthAndPassesWhatIsAlreadyInside){
    const Vector3 worldUp(0.0f, 1.0f, 0.0f);

    // inside the cone: returned unchanged
    const Vector3 gentle = normalizedOrFallback(Vector3(0.2f, 1.0f, 0.0f), worldUp);
    const Vector3 passed = clampTiltFromWorldUp(gentle, 35.0f);
    EXPECT_FLOAT_EQ(passed.x, gentle.x);
    EXPECT_FLOAT_EQ(passed.y, gentle.y);

    // outside: held at exactly the ceiling, leaning the same way
    const Vector3 steep = normalizedOrFallback(Vector3(1.0f, 0.05f, 0.0f), worldUp);
    const Vector3 heldSteep = clampTiltFromWorldUp(steep, 35.0f);
    EXPECT_NEAR(tileTiltFromWorldUp(heldSteep), 35.0f, 1.0e-3f);
    EXPECT_GT(heldSteep.x, 0.0f) << "azimuth of the lean must survive the clamp";
    EXPECT_NEAR(heldSteep.z, 0.0f, 1.0e-5f);
    EXPECT_NEAR(heldSteep.dotProduct(heldSteep), 1.0f, 1.0e-5f);

    // an upside-down normal names no azimuth; world up is the only answer
    const Vector3 inverted = clampTiltFromWorldUp(Vector3(0.0f, -1.0f, 0.0f), 35.0f);
    EXPECT_NEAR(inverted.y, 1.0f, 1.0e-5f);

    // a NaN ceiling must not become a NaN basis
    const Vector3 nanCeiling = clampTiltFromWorldUp(steep, std::numeric_limits<float>::quiet_NaN());
    EXPECT_TRUE(std::isfinite(nanCeiling.x) && std::isfinite(nanCeiling.y) && std::isfinite(nanCeiling.z));
    EXPECT_NEAR(tileTiltFromWorldUp(nanCeiling), SPLINE_DEFAULT_MAX_TILT_DEGREES, 1.0e-3f);
}

// ---- Footprint centering survives the emitted rotation -------------------------------------
//
// The layout cancels the bounds centre along the piece's OWN axes and the emitted TRS puts it
// back through the same basis: the two must cancel at EVERY heading (resolving either against
// world axes leaves a residue of R*c - c, zero along one heading and metres at others).

namespace{

    // a straight, flat centerline running at `yawDeg` from -Z, turning right (toward +X)
    std::vector<SplineCenterSample> tileStraightAtHeading(float yawDeg, float length, uint32_t sampleCount){
        const float yaw = yawDeg * TILE_TEST_PI / 180.0f;
        const Vector3 direction(std::sin(yaw), 0.0f, -std::cos(yaw));
        std::vector<SplineCenterSample> center;
        for (uint32_t i = 0; i < sampleCount; i++){
            SplineCenterSample s;
            s.position = direction * (length * static_cast<float>(i) / static_cast<float>(sampleCount - 1u));
            center.push_back(s);
        }
        return center;
    }

}

TEST(SplineTileLayoutTests, FootprintCenterStaysOnTheCenterlineAtEveryHeading){
    // SM_Env_Footpath_01, measured: 2.6242 x 0.2666 x 2.3756 m, corner-pivoted
    const SplinePiece piece = tilePiece(0u, Vector3(1.2435f, -0.0575f, -1.2429f), Vector3(1.3121f, 0.1333f, 1.1878f));
    ASSERT_EQ(piece.axis(), SplinePieceAxis::Z);

    for (const float yawDeg : {0.0f, 30.0f, 60.0f, 90.0f, 100.0f, 135.0f, 180.0f, -96.0f}){
        const std::vector<SplineCenterSample> center = tileStraightAtHeading(yawDeg, 20.0f, 41u);
        SplineTileFrameParams params;
        params.spacing = 2.309f;
        params.meshBoundsCenter = piece.center;
        params.meshBoundsHalfExtents = piece.halfExtents;

        const std::vector<SplineLayoutPose> poses = tileFrames(center, params);
        ASSERT_FALSE(poses.empty()) << "yaw " << yawDeg;

        for (const SplineLayoutPose& pose : poses){
            const SplinePiecePose emitted = makePiecePose(pose, piece, 1.0f);
            const Vector3 delta = tileToWorld(emitted, piece.center) - pose.base;
            EXPECT_NEAR(delta.dotProduct(pose.right), 0.0f, 1.0e-3f) << "lateral drift at yaw " << yawDeg;
            EXPECT_NEAR(delta.dotProduct(pose.forward), 0.0f, 1.0e-3f) << "along-track drift at yaw " << yawDeg;
            // what remains is the plant lift: the centre sits one half-height above the station
            EXPECT_NEAR(delta.dotProduct(pose.up), piece.halfExtents.y, 1.0e-3f) << "plant lift at yaw " << yawDeg;
        }
    }
}

// ---- Spline Instantiate: Spacing and Tile modes over a pool (AC7) --------------------------

namespace{

    // two interchangeable 2 m slabs and a 1 m one, all corner-pivoted the same way
    std::vector<SplinePiece> tileMixedPool(){
        return {tilePiece(0u, Vector3(0.0f, 0.1f, 1.0f), Vector3(0.6f, 0.1f, 1.0f)),
                tilePiece(1u, Vector3(0.0f, 0.1f, 1.0f), Vector3(0.6f, 0.1f, 1.0f)),
                tilePiece(2u, Vector3(0.0f, 0.1f, 0.5f), Vector3(0.6f, 0.1f, 0.5f))};
    }

    // a winding, climbing centerline with a tilted ground, dense at the pinned step
    std::vector<SplineCenterSample> tileWindingLine(){
        std::vector<SplineCenterSample> center;
        constexpr uint32_t samples = 241u;
        for (uint32_t i = 0; i < samples; i++){
            const float t = static_cast<float>(i) / static_cast<float>(samples - 1u);
            SplineCenterSample s;
            s.position = Vector3(12.0f * std::sin(t * 3.0f), 3.0f * t, -100.0f * t);
            s.normal = normalizedOrFallback(Vector3(0.1f * std::cos(t * 5.0f), 1.0f, 0.15f), Vector3(0, 1, 0));
            center.push_back(s);
        }
        return center;
    }

    SplineTileLayoutParams tileJitteredParams(const std::vector<SplinePiece>& pool){
        SplineTileLayoutParams params;
        params.pieces = pool;
        params.spacing = 1.5f;
        params.alignToSurfaceNormal = true;
        params.seed = 2024u;
        params.spacingJitterMetres = 0.3f;
        params.yawJitterDegrees = 12.0f;
        params.lateralJitterMetres = 0.4f;
        params.lateralOffset = 0.75f;
        params.scaleMin = 0.8f;
        params.scaleMax = 1.2f;
        return params;
    }

}

TEST(SplineTileLayoutTests, TileFitToLengthExactlyCoversTheLength){
    const std::vector<SplinePiece> pool = tileMixedPool();
    for (const float length : {7.3f, 20.0f, 33.3f}){
        SplineTileLayoutParams params;
        params.mode = SplineTileMode::Tile;
        params.fit = SplinePlacementFit::FitToLength;
        params.pieces = pool;
        params.seed = 7u;
        const std::vector<SplinePiecePose> poses = tileLayout(tileStraightLine(length, 200u), params);
        ASSERT_FALSE(poses.empty()) << length;

        // each piece's extent along -Z, from its own bounds through its emitted pose
        float cursor = 0.0f;
        for (size_t i = 0; i < poses.size(); i++){
            const SplinePiece& piece = pool[poses[i].poolIndex];
            const Vector3 nearEnd = tileToWorld(poses[i], piece.center - Vector3(0.0f, 0.0f, piece.halfExtents.z));
            const Vector3 farEnd = tileToWorld(poses[i], piece.center + Vector3(0.0f, 0.0f, piece.halfExtents.z));
            EXPECT_NEAR(-nearEnd.z, cursor, 1.0e-3f) << "length " << length << ": piece " << i << " leaves a gap or overlaps";
            EXPECT_GT(-farEnd.z, -nearEnd.z) << "piece " << i << " is laid backwards";
            // centred on the line, lifted onto it
            EXPECT_NEAR(tileToWorld(poses[i], piece.center).x, 0.0f, 1.0e-4f);
            EXPECT_NEAR(tileToWorld(poses[i], piece.center).y, piece.halfExtents.y, 1.0e-4f);
            cursor = -farEnd.z;
        }
        EXPECT_NEAR(cursor, length, 1.0e-3f) << "the tiles do not close the run";
        // one shared stretch, within a piece of the natural fill
        for (const SplinePiecePose& pose : poses){
            EXPECT_EQ(pose.scale.z, poses.front().scale.z);
            EXPECT_EQ(pose.scale.x, 1.0f);
            EXPECT_EQ(pose.scale.y, 1.0f);
        }
        // the natural fill lies within half the longest piece (1 m) of the length either way
        EXPECT_NEAR(poses.front().scale.z, 1.0f, 1.0f / (length - 1.0f));
    }
}

TEST(SplineTileLayoutTests, TileFixedPitchLaysWholePiecesAndLeavesTheRemainder){
    const std::vector<SplinePiece> pool = tileMixedPool();
    SplineTileLayoutParams params;
    params.mode = SplineTileMode::Tile;
    params.fit = SplinePlacementFit::FixedPitch;
    params.pieces = pool;
    params.seed = 3u;
    constexpr float length = 15.5f;
    const std::vector<SplinePiecePose> poses = tileLayout(tileStraightLine(length, 100u), params);
    ASSERT_FALSE(poses.empty());

    float cursor = 0.0f;
    for (const SplinePiecePose& pose : poses){
        const SplinePiece& piece = pool[pose.poolIndex];
        EXPECT_EQ(pose.scale.z, 1.0f);
        const Vector3 nearEnd = tileToWorld(pose, piece.center - Vector3(0.0f, 0.0f, piece.halfExtents.z));
        EXPECT_NEAR(-nearEnd.z, cursor, 1.0e-3f);
        cursor += piece.length();
    }
    EXPECT_LE(cursor, length + 1.0e-3f);
    // the next pick would not have fitted whole
    const SplinePiece& next = pool[splinePickPiece(params.seed, SplinePoolRole::Tile, static_cast<uint32_t>(poses.size()), pool)];
    EXPECT_GT(cursor + next.length(), length);
}

TEST(SplineTileLayoutTests, SpacingModePicksFromThePoolByStationAndKeepsTheSpacing){
    const std::vector<SplinePiece> pool = tileMixedPool();
    SplineTileLayoutParams params;
    params.pieces = pool;
    params.spacing = 2.0f;
    params.fit = SplinePlacementFit::FitToLength;
    params.seed = 11u;
    const std::vector<SplinePiecePose> poses = tileLayout(tileStraightLine(40.0f, 81u), params);
    ASSERT_EQ(poses.size(), 21u);
    bool sawEveryEntry[3] = {false, false, false};
    for (size_t i = 0; i < poses.size(); i++){
        EXPECT_EQ(poses[i].stationIndex, static_cast<uint32_t>(i));
        EXPECT_EQ(poses[i].ordinal, static_cast<uint32_t>(i));
        EXPECT_EQ(poses[i].role, SplinePieceRole::Tile);
        EXPECT_EQ(poses[i].poolIndex, pool[splinePickPiece(params.seed, SplinePoolRole::Tile, static_cast<uint32_t>(i), pool)].poolIndex);
        sawEveryEntry[poses[i].poolIndex] = true;
        const SplinePiece& piece = pool[poses[i].poolIndex];
        // each piece's footprint centre lands on its station
        EXPECT_NEAR(tileToWorld(poses[i], piece.center).z, -2.0f * static_cast<float>(i), 1.0e-3f);
    }
    EXPECT_TRUE(sawEveryEntry[0] && sawEveryEntry[1] && sawEveryEntry[2]);
}

TEST(SplineTileLayoutTests, ZeroWeightEntriesAreNeverPlaced){
    std::vector<SplinePiece> pool = tileMixedPool();
    pool[1].weight = 0.0f;
    pool[2].weight = 3.0f;
    SplineTileLayoutParams params;
    params.pieces = pool;
    params.spacing = 1.0f;
    params.seed = 5u;
    const std::vector<SplinePiecePose> poses = tileLayout(tileStraightLine(200.0f, 401u), params);
    ASSERT_FALSE(poses.empty());
    size_t heavy = 0;
    for (const SplinePiecePose& pose : poses){
        EXPECT_NE(pose.poolIndex, 1u);
        heavy += pose.poolIndex == 2u ? 1u : 0u;
    }
    EXPECT_NEAR(static_cast<float>(heavy) / static_cast<float>(poses.size()), 0.75f, 0.1f);
}

TEST(SplineTileLayoutTests, LateralOffsetIsOnTheRightAndTrimsLeaveTheEndsEmpty){
    const std::vector<SplinePiece> pool = {tilePiece(0u, Vector3(0.0f, 0.0f, 0.0f), Vector3(0.2f, 0.5f, 0.2f))};
    SplineTileLayoutParams params;
    params.pieces = pool;
    params.spacing = 1.0f;
    params.fit = SplinePlacementFit::FitToLength;
    params.lateralOffset = 2.0f;
    params.startTrimMetres = 3.0f;
    params.endTrimMetres = 4.0f;
    const std::vector<SplinePiecePose> poses = tileLayout(tileStraightLine(20.0f, 41u), params);
    // 13 m between the trims at a 1 m pitch: 14 stations
    ASSERT_EQ(poses.size(), 14u);
    for (const SplinePiecePose& pose : poses){
        // travelling -Z the right of travel is +X
        EXPECT_NEAR(pose.position.x, 2.0f, 1.0e-4f);
        EXPECT_LE(pose.position.z, -3.0f + 1.0e-4f);
        EXPECT_GE(pose.position.z, -16.0f - 1.0e-4f);
    }
    EXPECT_NEAR(poses.front().position.z, -3.0f, 1.0e-4f);
    EXPECT_NEAR(poses.back().position.z, -16.0f, 1.0e-4f);
}

TEST(SplineTileLayoutTests, ThePivotIsWhatLandsOnTheStation){
    // a piece whose pivot is 1 m above its mesh origin: the origin lands 1 m below the station
    SplinePiece piece = makeSplinePiece(4u, AABB(Vector3(-0.5f, 0.0f, -0.5f), Vector3(0.5f, 2.0f, 0.5f)), Vector3(0.0f, 1.0f, 0.0f));
    EXPECT_EQ(piece.poolIndex, 4u);
    EXPECT_NEAR(piece.center.y, 0.0f, 1.0e-6f);
    EXPECT_NEAR(piece.skirtDepth(), 1.0f, 1.0e-6f);
    const std::vector<SplinePiece> pool = {piece};
    SplineTileLayoutParams params;
    params.pieces = pool;
    params.spacing = 5.0f;
    params.plantMode = SplinePlantMode::Pivot;
    const std::vector<SplinePiecePose> poses = tileLayout(tileStraightLine(10.0f, 21u), params);
    ASSERT_EQ(poses.size(), 3u);
    for (const SplinePiecePose& pose : poses){
        EXPECT_EQ(pose.poolIndex, 4u);
        EXPECT_NEAR(pose.position.y, -1.0f, 1.0e-5f);
        tileExpectNear(tileToWorld(pose, Vector3(0.0f, 1.0f, 0.0f)), Vector3(0.0f, 0.0f, pose.position.z), 1.0e-5f, "pivot");
    }
}

TEST(SplineTileLayoutTests, SameSeedGivesIdenticalPoses){
    const std::vector<SplinePiece> pool = tileMixedPool();
    for (const SplineTileMode mode : {SplineTileMode::Spacing, SplineTileMode::Tile}){
        SplineTileLayoutParams params = tileJitteredParams(pool);
        params.mode = mode;
        params.dropoutChance = 0.25f;
        const std::vector<SplinePiecePose> first = tileLayout(tileWindingLine(), params);
        const std::vector<SplinePiecePose> second = tileLayout(tileWindingLine(), params);
        ASSERT_FALSE(first.empty());
        ASSERT_EQ(first.size(), second.size());
        for (size_t i = 0; i < first.size(); i++){
            EXPECT_TRUE(tilePoseBitsEqual(first[i], second[i])) << "pose " << i;
        }
        params.seed++;
        const std::vector<SplinePiecePose> reseeded = tileLayout(tileWindingLine(), params);
        bool anyDiffer = reseeded.size() != first.size();
        for (size_t i = 0; !anyDiffer && i < first.size(); i++){
            anyDiffer = !tilePoseBitsEqual(first[i], reseeded[i]);
        }
        EXPECT_TRUE(anyDiffer) << "the seed does not reach the layout";
    }
}

TEST(SplineTileLayoutTests, RaisingDropoutLeavesEverySurvivorBitIdentical){
    const std::vector<SplinePiece> pool = tileMixedPool();
    for (const SplineTileMode mode : {SplineTileMode::Spacing, SplineTileMode::Tile}){
        SplineTileLayoutParams params = tileJitteredParams(pool);
        params.mode = mode;
        params.dropoutChance = 0.2f;
        const std::vector<SplinePiecePose> light = tileLayout(tileWindingLine(), params);
        params.dropoutChance = 0.4f;
        const std::vector<SplinePiecePose> heavy = tileLayout(tileWindingLine(), params);
        ASSERT_FALSE(heavy.empty());
        ASSERT_LT(heavy.size(), light.size()) << "raising the dropout removed nothing";

        // every survivor of the heavier dropout survived the lighter one, at the same pose
        size_t j = 0;
        for (const SplinePiecePose& survivor : heavy){
            while (j < light.size() && light[j].stationIndex < survivor.stationIndex){
                j++;
            }
            ASSERT_LT(j, light.size()) << "station " << survivor.stationIndex << " appeared only under the heavier dropout";
            ASSERT_EQ(light[j].stationIndex, survivor.stationIndex);
            EXPECT_TRUE(tilePoseBitsEqual(light[j], survivor)) << "station " << survivor.stationIndex << " moved";
        }
    }
}

TEST(SplineTileLayoutTests, RotationLaysTheRunAxisOnForwardAndYOnUp){
    std::vector<SplinePiece> pool = tileMixedPool();
    // and one X-run piece, to check both axis conventions
    pool.push_back(tilePiece(3u, Vector3(1.0f, 0.2f, 0.0f), Vector3(1.0f, 0.2f, 0.1f)));
    ASSERT_EQ(pool.back().axis(), SplinePieceAxis::X);
    SplineTileLayoutParams params = tileJitteredParams(pool);
    params.maxTiltDegrees = 60.0f;
    const std::vector<SplineCenterSample> center = tileWindingLine();
    const std::vector<SplinePiecePose> poses = tileLayout(center, params);
    ASSERT_GT(poses.size(), 40u);

    for (const SplinePiecePose& pose : poses){
        EXPECT_NEAR(pose.rotation.norm(), 1.0f, 1.0e-5f);
        const SplinePiece& piece = pool[pose.poolIndex];
        const Vector3 runAxis = pose.rotation * tileRunAxis(piece);
        const Vector3 up = pose.rotation * Vector3(0.0f, 1.0f, 0.0f);
        // the run axis is on the ground direction of travel (-Z-ish here) and up is near +Y
        EXPECT_LT(runAxis.z, 0.0f);
        EXPECT_GT(up.y, 0.5f);
        // right-handed: the piece's run axis crossed with its up is the right of travel, which is
        // where the lateral offset put it (+X of the line here)
        EXPECT_NEAR(runAxis.length(), 1.0f, 1.0e-5f);
        EXPECT_NEAR(runAxis.dotProduct(up), 0.0f, 1.0e-5f);
    }

    // exact frame check on a single straight run, where every frame is known
    SplineTileLayoutParams straight;
    straight.pieces = pool;
    straight.spacing = 2.0f;
    straight.yawJitterDegrees = 25.0f;
    straight.seed = 9u;
    std::vector<SplineCenterSample> slope = tileStraightLine(30.0f, 61u);
    for (SplineCenterSample& s : slope){
        s.position.y = -0.2f * s.position.z;
    }
    const std::vector<SplinePiecePose> laid = tileLayout(slope, straight);
    SplineTileFrameParams frameParams;
    frameParams.spacing = 2.0f;
    frameParams.yawJitterDegrees = 25.0f;
    frameParams.seed = 9u;
    const std::vector<SplineLayoutPose> frames = tileFrames(slope, frameParams);
    ASSERT_EQ(laid.size(), frames.size());
    for (size_t i = 0; i < laid.size(); i++){
        const SplinePiece& piece = pool[laid[i].poolIndex];
        tileExpectNear(laid[i].rotation * tileRunAxis(piece), frames[i].forward, 1.0e-5f, "run axis on forward");
        tileExpectNear(laid[i].rotation * Vector3(0.0f, 1.0f, 0.0f), frames[i].up, 1.0e-5f, "+Y on up");
        // and the piece's local +X lands on the left of travel for Z-run, forward for X-run
        const Vector3 localX = laid[i].rotation * Vector3(1.0f, 0.0f, 0.0f);
        if (piece.axis() == SplinePieceAxis::Z){
            tileExpectNear(localX, frames[i].right * -1.0f, 1.0e-5f, "Z-run +X on the left");
        }else{
            tileExpectNear(laid[i].rotation * Vector3(0.0f, 0.0f, 1.0f), frames[i].right, 1.0e-5f, "X-run +Z on the right");
        }
    }
}

// R19: 2,000 pieces placed in <= 2 ms (Release, desktop). Measured with headroom for a shared
// build server; the budget itself is held by the bench (AC16).
TEST(SplineTileLayoutTests, TwoThousandPiecesAreCheapToPlace){
    const std::vector<SplinePiece> pool = tileMixedPool();
    std::vector<SplineCenterSample> center;
    constexpr uint32_t samples = 8001u;
    for (uint32_t i = 0; i < samples; i++){
        const float t = static_cast<float>(i) / static_cast<float>(samples - 1u);
        center.push_back({Vector3(40.0f * std::sin(t * 9.0f), 5.0f * std::sin(t * 4.0f), -4000.0f * t), Vector3(0, 1, 0), true});
    }
    SplineTileLayoutParams params = tileJitteredParams(pool);
    params.spacing = 2.0f;
    std::vector<SplinePiecePose> out;
    buildTileLayout(center, params, out);
    ASSERT_GE(out.size(), 2000u);
    ASSERT_LE(out.size(), params.maxPieces);

    double best = 1.0e9;
    for (int run = 0; run < 5; run++){
        const auto start = std::chrono::steady_clock::now();
        buildTileLayout(center, params, out);
        const auto stop = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(stop - start).count());
    }
    RecordProperty("best_ms", std::to_string(best));
    EXPECT_LT(best, 20.0) << "2,000 pieces took " << best << " ms";
}
