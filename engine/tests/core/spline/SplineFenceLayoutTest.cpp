// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineLayout/Tests/FenceLayoutTests.cpp, Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

// The fence layout (S18 R8, AC7): the per-run station walk, the mixed-length span fill, racked
// and stepped span frames, planting, the gap hold, the bury check, the crest row and its caps,
// reservations, span overrides (gates, pinned pieces) and the span mitres.
//
// OpenEngine is left-handed; these fixtures are its own mirrored into our right-handed frame:
// every world z is negated (so a run OpenEngine laid along +Z travels -Z, the engine's forward,
// and keeps the right of travel at +X), and a piece's local bounds centre is negated across
// travel (x for a piece run along Z, z for a piece run along X). Signed turns keep their sign
// (positive = right), lengths, insets and indices are unchanged. The Sheared grade is not
// ported (TRS instances only), nor anything that needs a mesh cut.

#include "SplineFenceLayout.h"
#include "SplineSpanMitre.h"
#include "SplineTileLayout.h"

#include "SplineFenceLayoutCrestGoldens.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::spline;
using jasperengine::spline::fencegolden::FenceCrestGolden;

namespace{

    constexpr float FENCE_PI = 3.14159265358979f;

    // Goldens are captured on x86-64 as float bit patterns; libm and FMA contraction differ per
    // architecture, so values are compared in ULPs (1-8 observed, with headroom).
    constexpr uint32_t FENCE_GOLDEN_MAX_ULPS = 64;

    uint32_t fenceFloatBits(float value){
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    // How many representable floats lie between two bit patterns (+0 and -0 are one point).
    uint64_t fenceUlpDistance(uint32_t aBits, uint32_t bBits){
        constexpr uint32_t signBit = 0x80000000u;
        auto ordered = [](uint32_t bits) -> int64_t {
            const int64_t magnitude = static_cast<int64_t>(bits & ~signBit);
            return (bits & signBit) != 0u ? -magnitude : magnitude;
        };
        const int64_t a = ordered(aBits);
        const int64_t b = ordered(bBits);
        return static_cast<uint64_t>(a > b ? a - b : b - a);
    }

    float fenceDot(const Vector3& a, const Vector3& b){
        return a.dotProduct(b);
    }

    float fenceLength(const Vector3& v){
        return std::sqrt(v.dotProduct(v));
    }

    // A straight, flat centerline along -Z (OpenEngine's +Z), sampled every length / (count - 1).
    std::vector<SplineCenterSample> fenceStraightLine(float length, uint32_t sampleCount){
        std::vector<SplineCenterSample> center;
        for (uint32_t i = 0; i < sampleCount; ++i){
            SplineCenterSample s;
            s.position = Vector3(0.0f, 0.0f, -(length * static_cast<float>(i) / static_cast<float>(sampleCount - 1u)));
            s.normal = Vector3(0.0f, 1.0f, 0.0f);
            center.push_back(s);
        }
        return center;
    }

    // A straight ramp along -Z with a constant grade.
    std::vector<SplineCenterSample> fenceRamp(float run, float rise, uint32_t sampleCount){
        std::vector<SplineCenterSample> center;
        for (uint32_t i = 0; i < sampleCount; ++i){
            const float t = static_cast<float>(i) / static_cast<float>(sampleCount - 1u);
            SplineCenterSample s;
            s.position = Vector3(0.0f, rise * t, -(run * t));
            s.normal = Vector3(0.0f, 1.0f, 0.0f);
            center.push_back(s);
        }
        return center;
    }

    // Flat along -Z but for a half-sine bump of `height` between the distances bumpFrom and
    // bumpTo: the draped centerline climbs over it, so the run is longer than its chord.
    std::vector<SplineCenterSample> fenceBumpedLine(float length, float bumpFrom, float bumpTo, float height, uint32_t sampleCount){
        std::vector<SplineCenterSample> center = fenceStraightLine(length, sampleCount);
        for (SplineCenterSample& sample : center){
            const float t = (-sample.position.z - bumpFrom) / (bumpTo - bumpFrom);
            if (t > 0.0f && t < 1.0f){
                sample.position.y = height * std::sin(FENCE_PI * t);
            }
        }
        return center;
    }

    // An L turning right: (0,0,0) -> (0,0,-armZ) -> (armX,0,-armZ), sampled at `step` metres.
    std::vector<SplineCenterSample> fenceCorneredL(float armZ, float armX, float step){
        std::vector<SplineCenterSample> center;
        const uint32_t legZ = static_cast<uint32_t>(std::lround(armZ / step));
        const uint32_t legX = static_cast<uint32_t>(std::lround(armX / step));
        for (uint32_t i = 0; i <= legZ; ++i){
            center.push_back({Vector3(0.0f, 0.0f, -(static_cast<float>(i) * step)), Vector3(0, 1, 0), true});
        }
        for (uint32_t i = 1; i <= legX; ++i){
            center.push_back({Vector3(static_cast<float>(i) * step, 0.0f, -armZ), Vector3(0, 1, 0), true});
        }
        return center;
    }

    // A closed square of `side` metres traversed p0->p1->p2->p3->p0, at `step`.
    std::vector<SplineCenterSample> fenceClosedSquare(float side, float step){
        const Vector3 corners[4] = {Vector3(0, 0, 0), Vector3(side, 0, 0), Vector3(side, 0, -side), Vector3(0, 0, -side)};
        std::vector<SplineCenterSample> center;
        const uint32_t perLeg = static_cast<uint32_t>(std::lround(side / step));
        for (uint32_t leg = 0; leg < 4; ++leg){
            const Vector3& a = corners[leg];
            const Vector3& b = corners[(leg + 1u) % 4u];
            for (uint32_t i = 0; i < perLeg; ++i){
                const float t = static_cast<float>(i) / static_cast<float>(perLeg);
                center.push_back({a + (b - a) * t, Vector3(0, 1, 0), true});
            }
        }
        center.push_back({corners[0], Vector3(0, 1, 0), true}); // closes the loop
        return center;
    }

    // A synthetic span: thin in X, long in Z (run along Z).
    SplinePiece fencePiece(float length, float height = 1.0f){
        SplinePiece b;
        b.halfExtents = Vector3(0.05f, height * 0.5f, length * 0.5f);
        b.center = Vector3(0.0f, height * 0.5f, 0.0f);
        return b;
    }

    // ---- Real kit shapes ----------------------------------------------------
    //
    // Half-extents measured off shipped kit meshes. Every kit piece that runs anywhere runs along
    // local X; the near-square ones are posts and slabs.

    // SM_Prop_Fence_01: 2.4707 x 1.6387 x 0.1567 m, centre-pivoted in XZ with a -0.52 m planting
    // skirt; 15.8:1.
    SplinePiece fenceMeasuredKitPanel(){
        SplinePiece b;
        b.halfExtents = Vector3(1.2353f, 0.8194f, 0.0784f);
        b.center = Vector3(0.0f, 0.2978f, 0.0f);
        return b;
    }

    // SM_Env_StoneWall_Pillar_01: 1.0066 x 1.1668 x 1.0043 m, a standalone post (near-square).
    SplinePiece fenceMeasuredKitPost(){
        SplinePiece b;
        b.halfExtents = Vector3(0.5033f, 0.5834f, 0.5021f);
        b.center = Vector3(0.0f, 0.5834f, 0.0f);
        return b;
    }

    // Where the piece's local axis `index` points in the world, scaled: the emitted transform's
    // basis column.
    Vector3 fenceColumn(const SplinePiecePose& pose, int index){
        const Vector3 unit(index == 0 ? 1.0f : 0.0f, index == 1 ? 1.0f : 0.0f, index == 2 ? 1.0f : 0.0f);
        return pose.rotation * (unit * pose.scale);
    }

    float fenceBasisDeterminant(const SplinePiecePose& pose){
        return fenceDot(fenceColumn(pose, 0), fenceColumn(pose, 1).crossProduct(fenceColumn(pose, 2)));
    }

    // A piece-local point in the world, through the emitted TRS.
    Vector3 fenceToWorld(const SplinePiecePose& pose, const Vector3& local){
        return pose.position + pose.rotation * (pose.scale * local);
    }

    bool fenceAnyValidationContains(const SplineFenceLayoutResult& result, const char* needle){
        return std::any_of(result.validation.begin(), result.validation.end(),
                           [needle](const std::string& m){ return m.find(needle) != std::string::npos; });
    }

}

// ---- Per-run station walk -------------------------------------------------

// Knots are mandatory stations, and the pitch fills between them. Three knots on a 20 m line at
// pitch 2.5 give five stations per 10 m run, with the shared station emitted exactly once.
TEST(SplineFenceLayoutTests, AuthoredPointsAreStationsAndPitchFillsBetween)
{
    const auto center = fenceStraightLine(20.0f, 41u);
    const float boundaries[3] = {0.0f, 20.0f, 40.0f};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = 2.5f;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_EQ(result.stations.size(), 9u);
    for (uint32_t i = 0; i < 9u; ++i){
        EXPECT_NEAR(result.stations[i].pose.base.z, -2.5f * static_cast<float>(i), 1.0e-3f) << "station " << i;
        EXPECT_EQ(result.stations[i].index, i);
    }
    // Stations 0 and 4 open their runs; 8 closes the last one.
    EXPECT_TRUE(result.stations[0].isKnot);
    EXPECT_TRUE(result.stations[4].isKnot);
    EXPECT_TRUE(result.stations[8].isKnot);
    EXPECT_FALSE(result.stations[1].isKnot);
    EXPECT_TRUE(result.validation.empty());
}

// A closed spline adds the last->first run and emits the shared station ONCE: four corners, four
// stations, no duplicate where the loop closes.
TEST(SplineFenceLayoutTests, ClosedSplineWrapsAndSharesTheFirstStationOnce)
{
    const auto center = fenceClosedSquare(10.0f, 0.5f);
    const float boundaries[5] = {0.0f, 20.0f, 40.0f, 60.0f, 80.0f};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.closed = true;
    params.postPitch = 12.0f; // longer than a side: two stations per run

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_EQ(result.stations.size(), 4u);
    for (const SplineFenceStation& s : result.stations){
        EXPECT_TRUE(s.isKnot);
    }

    // Exactly one station at the loop's origin, not two.
    const auto atOrigin = std::count_if(result.stations.begin(), result.stations.end(), [](const SplineFenceStation& s){
        return std::abs(s.pose.base.x) < 1.0e-3f && std::abs(s.pose.base.z) < 1.0e-3f;
    });
    EXPECT_EQ(atOrigin, 1);

    // The wrap station is a two-run corner like every other one, so it takes the bisector too.
    // The last run arrives along +Z, run 0 leaves along +X.
    EXPECT_NEAR(result.stations[0].pose.forward.x, 0.70710678f, 2.0e-3f);
    EXPECT_NEAR(result.stations[0].pose.forward.y, 0.0f, 1.0e-3f);
    EXPECT_NEAR(result.stations[0].pose.forward.z, 0.70710678f, 2.0e-3f);
}

// The shared corner station takes the BISECTOR of both runs' end directions.
TEST(SplineFenceLayoutTests, SharedCornerStationTakesTheBisectorYaw)
{
    const auto center = fenceCorneredL(10.0f, 10.0f, 0.5f);
    const float boundaries[3] = {0.0f, 20.0f, 40.0f};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = 12.0f; // two stations per run, so index 1 IS the corner

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_EQ(result.stations.size(), 3u);

    // Run 0 travels -Z, run 1 travels +X: the bisector is 45 degrees between.
    const Vector3 corner = result.stations[1].pose.forward;
    EXPECT_NEAR(corner.x, 0.70710678f, 2.0e-3f);
    EXPECT_NEAR(corner.y, 0.0f, 1.0e-3f);
    EXPECT_NEAR(corner.z, -0.70710678f, 2.0e-3f);

    // The runs' own ends keep their own chords.
    EXPECT_NEAR(result.stations[0].pose.forward.z, -1.0f, 1.0e-3f);
    EXPECT_NEAR(result.stations[2].pose.forward.x, 1.0f, 1.0e-3f);
}

// Coincident knots: the run between them is skipped, the shared station emitted once, and the
// pair is named rather than silently deduped.
TEST(SplineFenceLayoutTests, ZeroLengthRunIsSkippedAndReported)
{
    const auto center = fenceStraightLine(20.0f, 41u);
    const float boundaries[4] = {0.0f, 20.0f, 20.0f, 40.0f};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = 12.0f;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    EXPECT_TRUE(fenceAnyValidationContains(result, "coincident"));
    ASSERT_EQ(result.stations.size(), 3u);
    EXPECT_NEAR(result.stations[0].pose.base.z, 0.0f, 1.0e-3f);
    EXPECT_NEAR(result.stations[1].pose.base.z, -10.0f, 1.0e-3f);
    EXPECT_NEAR(result.stations[2].pose.base.z, -20.0f, 1.0e-3f);
}

// ---- Variable-length span fill -------------------------------------------

// Pool pieces need not share a length: the fill draws a sequence and one shared stretch closes
// the run exactly on both knot stations.
TEST(SplineFenceLayoutTests, MixedLengthPoolFillsARunExactlyAndWithinTheCap)
{
    const auto center = fenceStraightLine(30.0f, 61u);
    const float boundaries[2] = {0.0f, 60.0f};
    const SplinePiece pieces[2] = {fencePiece(2.0f), fencePiece(3.0f)};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = 2.4f;
    params.spanMaxStretch = 1.25f;
    params.seed = 7u;
    params.spanPieces = pieces;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_FALSE(result.spans.empty());
    EXPECT_EQ(result.stations.size(), result.spans.size() + 1u);

    // Both pool lengths are actually drawn.
    const bool usesShort = std::any_of(result.spans.begin(), result.spans.end(), [](const SplineFenceSpan& s){ return s.poolSlot == 0u; });
    const bool usesLong = std::any_of(result.spans.begin(), result.spans.end(), [](const SplineFenceSpan& s){ return s.poolSlot == 1u; });
    EXPECT_TRUE(usesShort);
    EXPECT_TRUE(usesLong);

    // One shared stretch, inside the cap, and the spans cover the run exactly.
    float covered = 0.0f;
    for (const SplineFenceSpan& span : result.spans){
        EXPECT_LE(span.lengthScale, params.spanMaxStretch + 1.0e-3f);
        EXPECT_GE(span.lengthScale, 1.0f / params.spanMaxStretch - 1.0e-3f);
        covered += pieces[span.poolSlot].length() * span.lengthScale;
    }
    EXPECT_NEAR(covered, 30.0f, 1.0e-2f);
    EXPECT_TRUE(result.validation.empty());
}

// The count is seeded from the pool's own pieces, so it misses only when the DRAWN sequence
// differs from the pool's mean. Mean 4 m seeds two pieces for this 6 m run, the draw comes back
// 2 m + 2 m (1.50x, past the cap), and one more lands on 6 m exactly.
TEST(SplineFenceLayoutTests, CountAdjustsUpWhenTheDrawnFillMissesTheCap)
{
    const auto center = fenceStraightLine(6.0f, 13u);
    const float boundaries[2] = {0.0f, 12.0f};
    const SplinePiece pieces[2] = {fencePiece(2.0f), fencePiece(6.0f)};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = 2.4f; // deliberately unrelated: the pieces decide the count
    params.spanMaxStretch = 1.25f;
    params.seed = 1u;
    params.spanPieces = pieces;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    EXPECT_EQ(result.spans.size(), 3u); // 3 x 2 m = 6 m, scale 1.0
    for (const SplineFenceSpan& span : result.spans){
        EXPECT_NEAR(span.lengthScale, 1.0f, 1.0e-3f);
    }
    EXPECT_TRUE(result.validation.empty());
}

// The count comes from the pool's piece lengths, never from postPitch: a 20 m wall run of 5 m
// segments at the default 2.4 m pitch takes the four that land on 1.00x.
TEST(SplineFenceLayoutTests, FillCountComesFromPieceLengthNotPostPitch)
{
    const auto center = fenceStraightLine(20.0f, 41u);
    const float boundaries[2] = {0.0f, 40.0f};
    const SplinePiece pieces[1] = {fencePiece(5.0f)};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = 2.4f;
    params.spanMaxStretch = 1.25f;
    params.spanPieces = pieces;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_EQ(result.spans.size(), 4u);
    for (const SplineFenceSpan& span : result.spans){
        EXPECT_NEAR(span.lengthScale, 1.0f, 1.0e-4f);
    }
    EXPECT_EQ(result.stations.size(), 5u);
    EXPECT_TRUE(result.validation.empty());
}

// The cap can be genuinely unsatisfiable (2 m piece, 3 m run, 1.25x cap has no integer count).
// The search stops after one attempt each way, the run is still built, and the miss is named.
TEST(SplineFenceLayoutTests, UnsatisfiableStretchIsReportedNotChased)
{
    const auto center = fenceStraightLine(3.0f, 7u);
    const float boundaries[2] = {0.0f, 6.0f};
    const SplinePiece pieces[1] = {fencePiece(2.0f)};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = 3.0f;
    params.spanMaxStretch = 1.25f;
    params.spanPieces = pieces;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_FALSE(result.spans.empty());
    // 2 pieces at 0.75x: a COMPRESSION, under the 0.800x floor.
    EXPECT_TRUE(fenceAnyValidationContains(result, "under the 0.800x floor"));
    EXPECT_FALSE(fenceAnyValidationContains(result, "past the"));
    // The search tries the nominal and its two neighbours and no more.
    EXPECT_TRUE(fenceAnyValidationContains(result, "within one of the nominal"));
    EXPECT_EQ(result.spans.size(), 2u);
    for (const SplineFenceSpan& span : result.spans){
        EXPECT_NEAR(span.lengthScale, 0.75f, 1.0e-3f);
    }
    // The run still closes on both knot stations.
    EXPECT_NEAR(result.stations.back().pose.base.z, -3.0f, 1.0e-3f);
}

// The other direction: a run too long for its nearest counts is genuinely past the cap.
TEST(SplineFenceLayoutTests, StretchPastTheCapIsReportedAsTheCapNotTheFloor)
{
    const auto center = fenceStraightLine(2.6f, 7u);
    const float boundaries[2] = {0.0f, 6.0f};
    const SplinePiece pieces[1] = {fencePiece(2.0f)};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanMaxStretch = 1.25f;
    params.spanPieces = pieces;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_EQ(result.spans.size(), 1u);
    EXPECT_NEAR(result.spans[0].lengthScale, 1.3f, 1.0e-3f);
    EXPECT_TRUE(fenceAnyValidationContains(result, "past the 1.250x cap"));
    EXPECT_FALSE(fenceAnyValidationContains(result, "floor"));
}

// ---- Span frames ----------------------------------------------------------

// Racked spans pitch to follow the grade; stepped spans stay level at the LOWER station so no
// span floats off its downhill end.
TEST(SplineFenceLayoutTests, RackedAndSteppedSpansDifferOnAGrade)
{
    const auto center = fenceRamp(6.0f, 2.0f, 13u);
    const float boundaries[2] = {0.0f, 12.0f};
    const float runLength = std::sqrt(6.0f * 6.0f + 2.0f * 2.0f);
    const SplinePiece pieces[1] = {fencePiece(runLength)};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = runLength; // exactly one span
    params.spanPieces = pieces;

    params.spanGrade = SplineSpanGrade::Racked;
    const SplineFenceLayoutResult racked = buildFenceLayout(center, params);
    ASSERT_EQ(racked.spans.size(), 1u);
    // Follows the grade: rise 2 over run 6 is a normalized +y of 2/sqrt(40).
    EXPECT_NEAR(racked.spans[0].pose.forward.y, 2.0f / runLength, 1.0e-3f);
    EXPECT_NEAR(racked.spans[0].pose.base.y, 1.0f, 1.0e-3f); // midpoint of 0 and 2

    params.spanGrade = SplineSpanGrade::Stepped;
    const SplineFenceLayoutResult stepped = buildFenceLayout(center, params);
    ASSERT_EQ(stepped.spans.size(), 1u);
    EXPECT_NEAR(stepped.spans[0].pose.forward.y, 0.0f, 1.0e-3f);
    EXPECT_NEAR(stepped.spans[0].pose.base.y, 0.0f, 1.0e-3f); // the lower station
    // A level span between two stations 6 m apart in XZ is shorter than the racked one.
    EXPECT_LT(stepped.spans[0].lengthScale, racked.spans[0].lengthScale);

    // The stations themselves are unmoved by the grade mode.
    ASSERT_EQ(stepped.stations.size(), racked.stations.size());
    EXPECT_NEAR(stepped.stations.back().pose.base.y, 2.0f, 1.0e-3f);
}

// A stepped riser taller than the piece that has to cover it leaves a gap, and that is reported.
TEST(SplineFenceLayoutTests, SteppedRiserTallerThanItsCoverIsReported)
{
    const auto center = fenceRamp(6.0f, 4.0f, 13u);
    const float boundaries[2] = {0.0f, 12.0f};
    const SplinePiece pieces[1] = {fencePiece(2.0f, 1.0f)}; // 1 m tall

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = 2.0f;
    params.spanGrade = SplineSpanGrade::Stepped;
    params.spanPieces = pieces;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    EXPECT_TRUE(fenceAnyValidationContains(result, "steps"));
}

// ---- Planting -------------------------------------------------------------

// The measured kits carry a planting skirt below the pivot, so the two plant modes are half a
// metre apart on the same bounds. Pivot lands the pivot on the ground; BoundsMin lifts until the
// lowest vertex touches.
TEST(SplineFenceLayoutTests, PlantModeDistinguishesOnSkirtedBounds)
{
    const auto center = fenceStraightLine(10.0f, 21u);
    const float boundaries[2] = {0.0f, 20.0f};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = 12.0f;
    // Bounds min y = 0.24 - 0.76 = -0.52: the measured skirt.
    SplinePiece post;
    post.center = Vector3(0.0f, 0.24f, 0.0f);
    post.halfExtents = Vector3(0.1f, 0.76f, 0.1f);
    const SplinePiece posts[1] = {post};
    params.postPieces = posts;

    params.plantMode = SplinePlantMode::Pivot;
    const SplineFenceLayoutResult planted = buildFenceLayout(center, params);
    ASSERT_FALSE(planted.stations.empty());
    EXPECT_NEAR(planted.stations[0].pose.position.y, 0.0f, 1.0e-4f);
    EXPECT_NEAR(planted.stations[0].pose.base.y, 0.0f, 1.0e-4f);

    params.plantMode = SplinePlantMode::BoundsMin;
    const SplineFenceLayoutResult lifted = buildFenceLayout(center, params);
    ASSERT_FALSE(lifted.stations.empty());
    EXPECT_NEAR(lifted.stations[0].pose.position.y, 0.52f, 1.0e-4f);
    EXPECT_NEAR(lifted.stations[0].pose.base.y, 0.0f, 1.0e-4f);
}

// An empty post pool is first-class: the stations still exist and still articulate the spans,
// and with no post mesh the pose sits on the centerline.
TEST(SplineFenceLayoutTests, EmptyPostPoolStillArticulatesSpans)
{
    const auto center = fenceCorneredL(10.0f, 10.0f, 0.5f);
    const float boundaries[3] = {0.0f, 20.0f, 40.0f};
    const SplinePiece pieces[1] = {fencePiece(2.0f)};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = 2.0f;
    params.spanPieces = pieces;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_EQ(result.spans.size(), 10u); // 5 per 10 m run at 2 m pieces
    for (const SplineFenceStation& station : result.stations){
        EXPECT_NEAR(station.pose.position.x, station.pose.base.x, 1.0e-4f);
        EXPECT_NEAR(station.pose.position.y, station.pose.base.y, 1.0e-4f);
        EXPECT_NEAR(station.pose.position.z, station.pose.base.z, 1.0e-4f);
    }
}

// ---- Gap hold and burial --------------------------------------------------

// Hold-then-split: the gap patch runs on the WHOLE centerline before it is cut into runs. A run
// that opens inside the hole inherits the altitude its neighbour ended on.
TEST(SplineFenceLayoutTests, GapHoldRunsOnTheWholeCenterlineBeforeSplitting)
{
    auto center = fenceStraightLine(20.0f, 41u);
    for (auto& s : center){
        s.position.y = 5.0f; // measured ground at 5 m
    }
    // A hole spanning the second run's opening: samples 18..26 unmeasured, and the ground on the
    // far side sits 3 m lower.
    for (uint32_t i = 27; i < 41u; ++i){
        center[i].position.y = 2.0f;
    }
    for (uint32_t i = 18; i <= 26u; ++i){
        center[i].surfaceValid = false;
        center[i].position.y = 100.0f; // authored fallback, deliberately absurd
    }

    const float boundaries[3] = {0.0f, 20.0f, 40.0f};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = 12.0f;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_EQ(result.stations.size(), 3u);
    EXPECT_NEAR(result.stations[1].pose.base.y, 5.0f, 1.0e-3f);
    for (const SplineFenceStation& s : result.stations){
        EXPECT_LT(s.pose.base.y, 10.0f);
    }
}

// Spans cast no rays, but the run's draped samples are dense enough to notice the ground rising
// through a span's underside.
TEST(SplineFenceLayoutTests, GroundRisingThroughASpanIsReported)
{
    auto center = fenceStraightLine(10.0f, 21u);
    center[10].position.y = 1.0f; // a bump the span cannot clear
    const float boundaries[2] = {0.0f, 20.0f};
    const SplinePiece pieces[1] = {fencePiece(10.0f)};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = 10.0f; // one span across the whole run
    params.spanPieces = pieces;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    EXPECT_TRUE(fenceAnyValidationContains(result, "rises through"));

    // The same run without the bump reports nothing.
    center[10].position.y = 0.0f;
    const SplineFenceLayoutResult flat = buildFenceLayout(center, params);
    EXPECT_FALSE(fenceAnyValidationContains(flat, "rises through"));
}

// ---- Determinism ----------------------------------------------------------

// Rebuild idempotence: identical inputs give byte-identical stations, spans and pool picks.
TEST(SplineFenceLayoutTests, RebuildIsIdempotentWithPoolsAndSeed)
{
    const auto center = fenceCorneredL(12.0f, 8.0f, 0.5f);
    const float boundaries[3] = {0.0f, 24.0f, 40.0f};
    const SplinePiece pieces[3] = {fencePiece(2.0f), fencePiece(3.0f), fencePiece(1.5f)};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = 2.4f;
    params.seed = 4242u;
    params.spanPieces = pieces;

    const SplineFenceLayoutResult a = buildFenceLayout(center, params);
    const SplineFenceLayoutResult b = buildFenceLayout(center, params);

    ASSERT_FALSE(a.stations.empty());
    ASSERT_FALSE(a.spans.empty());
    ASSERT_EQ(a.stations.size(), b.stations.size());
    ASSERT_EQ(a.spans.size(), b.spans.size());
    EXPECT_EQ(a.validation, b.validation);

    for (size_t i = 0; i < a.stations.size(); ++i){
        EXPECT_EQ(a.stations[i].pose.position.x, b.stations[i].pose.position.x);
        EXPECT_EQ(a.stations[i].pose.position.y, b.stations[i].pose.position.y);
        EXPECT_EQ(a.stations[i].pose.position.z, b.stations[i].pose.position.z);
        EXPECT_EQ(a.stations[i].index, b.stations[i].index);
    }
    for (size_t i = 0; i < a.spans.size(); ++i){
        EXPECT_EQ(a.spans[i].poolSlot, b.spans[i].poolSlot);
        EXPECT_EQ(a.spans[i].lengthScale, b.spans[i].lengthScale);
        EXPECT_EQ(a.spans[i].pose.position.z, b.spans[i].pose.position.z);
    }

    // A different seed reshuffles the picks but not the structure.
    params.seed = 4243u;
    const SplineFenceLayoutResult reseeded = buildFenceLayout(center, params);
    std::vector<uint32_t> first, second;
    for (const SplineFenceSpan& s : a.spans){
        first.push_back(s.poolSlot);
    }
    for (const SplineFenceSpan& s : reseeded.spans){
        second.push_back(s.poolSlot);
    }
    EXPECT_NE(first, second);
}

namespace{

    void fenceExpectFinite(const SplineLayoutPose& p, const char* what){
        EXPECT_TRUE(std::isfinite(p.position.x) && std::isfinite(p.position.y) && std::isfinite(p.position.z)) << what;
    }

    void fenceExpectOrthonormal(const SplineLayoutPose& p, const char* what){
        EXPECT_NEAR(fenceDot(p.right, p.right), 1.0f, 1.0e-4f) << what;
        EXPECT_NEAR(fenceDot(p.up, p.up), 1.0f, 1.0e-4f) << what;
        EXPECT_NEAR(fenceDot(p.forward, p.forward), 1.0f, 1.0e-4f) << what;
        EXPECT_NEAR(fenceDot(p.right, p.up), 0.0f, 1.0e-4f) << what;
        EXPECT_NEAR(fenceDot(p.right, p.forward), 0.0f, 1.0e-4f) << what;
        EXPECT_NEAR(fenceDot(p.up, p.forward), 0.0f, 1.0e-4f) << what;
        // right-handed as the engine is: right = forward x up
        EXPECT_NEAR(fenceDot(p.right, p.forward.crossProduct(p.up)), 1.0f, 1.0e-4f) << what;
        fenceExpectFinite(p, what);
    }

}

// Every emitted frame is finite and orthonormal: a NaN basis would poison the transform
// hierarchy silently.
TEST(SplineFenceLayoutTests, EveryEmittedFrameIsOrthonormalAndFinite)
{
    const auto center = fenceClosedSquare(9.0f, 0.5f);
    const float boundaries[5] = {0.0f, 18.0f, 36.0f, 54.0f, 72.0f};
    const SplinePiece pieces[2] = {fencePiece(2.0f), fencePiece(2.5f)};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.closed = true;
    params.postPitch = 2.4f;
    params.seed = 11u;
    params.spanPieces = pieces;

    for (const SplineSpanGrade grade : {SplineSpanGrade::Racked, SplineSpanGrade::Stepped}){
        params.spanGrade = grade;
        const SplineFenceLayoutResult result = buildFenceLayout(center, params);
        ASSERT_FALSE(result.stations.empty());
        ASSERT_FALSE(result.spans.empty());

        for (const SplineFenceStation& s : result.stations){
            fenceExpectOrthonormal(s.pose, "station");
        }
        for (const SplineFenceSpan& s : result.spans){
            fenceExpectOrthonormal(s.pose, "span");
            EXPECT_TRUE(std::isfinite(s.lengthScale));
            EXPECT_GT(s.lengthScale, 0.0f);
        }
    }
}

// ---- Piece axis: which way a kit piece runs --------------------------------

// A 2.47 x 0.157 m panel once filled the run 15.7 deep and crosswise, because the fill measured
// its THICKNESS as its length. A 24.707 m run takes ten 2.4707 m panels at 1.00x.
TEST(SplineFenceLayoutTests, MeasuredKitPanelFillsByItsLengthNotItsThickness)
{
    const auto center = fenceStraightLine(24.707f, 249u);
    const float boundaries[2] = {0.0f, 248.0f};
    const SplinePiece pieces[1] = {fenceMeasuredKitPanel()};

    // The default postPitch is left in place deliberately: the fill seeds from the pieces.
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = pieces;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    EXPECT_EQ(result.spans.size(), 10u);
    EXPECT_EQ(result.stations.size(), 11u);
    for (const SplineFenceSpan& span : result.spans){
        EXPECT_NEAR(span.lengthScale, 1.0f, 1.0e-2f);
    }
    EXPECT_TRUE(result.validation.empty());

    // What the same run measured on the thickness would have wanted.
    EXPECT_NEAR(24.707f / (pieces[0].halfExtents.z * 2.0f), 157.6f, 0.5f);
}

// Orientation and measurement are one answer: the panel's long axis lands ON the direction of
// travel, carries the length scale, and the emitted basis is a yaw rather than a mirror.
TEST(SplineFenceLayoutTests, MeasuredKitPanelIsLaidAlongTheSplineNotAcrossIt)
{
    const auto center = fenceStraightLine(24.707f, 249u);
    const float boundaries[2] = {0.0f, 248.0f};
    const SplinePiece pieces[1] = {fenceMeasuredKitPanel()};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = pieces;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_FALSE(result.spans.empty());
    ASSERT_EQ(pieces[0].axis(), SplinePieceAxis::X);

    for (const SplineFenceSpan& span : result.spans){
        const SplinePiecePose t = makePiecePose(span.pose, pieces[0], span.lengthScale);

        // Local +X runs downstream (-Z world here) and carries the stretch.
        const Vector3 localX = fenceColumn(t, 0);
        EXPECT_NEAR(localX.z, -span.lengthScale, 1.0e-3f);
        EXPECT_NEAR(localX.x, 0.0f, 1.0e-3f);
        // Local +Z is the thickness: across travel, unscaled.
        const Vector3 localZ = fenceColumn(t, 2);
        EXPECT_NEAR(fenceLength(localZ), 1.0f, 1.0e-3f);
        EXPECT_NEAR(std::abs(localZ.x), 1.0f, 1.0e-3f);
        // A yaw, never a mirror.
        EXPECT_NEAR(fenceBasisDeterminant(t), span.lengthScale, 1.0e-3f);

        // The world length the piece covers along travel is the station gap it was fitted to.
        EXPECT_NEAR(span.lengthScale * pieces[0].length(), 2.4707f, 1.0e-2f);
    }
}

// Structural pin, independent of any mesh: whichever axis a piece is measured on is the axis
// laid along travel and the only one the length scale reaches.
TEST(SplineFenceLayoutTests, TheAxisMeasuredIsTheAxisLaidAlongTravel)
{
    constexpr float scaleAlong = 3.0f;
    const Vector3 travel(0.0f, 0.0f, -1.0f);

    SplineLayoutPose pose;
    pose.right = Vector3(1.0f, 0.0f, 0.0f);
    pose.up = Vector3(0.0f, 1.0f, 0.0f);
    pose.forward = travel;
    pose.base = Vector3(0.0f, 0.0f, 0.0f);
    pose.position = Vector3(0.0f, 0.0f, 0.0f);

    for (const SplinePieceAxis axis : {SplinePieceAxis::X, SplinePieceAxis::Z}){
        // a piece long only on `axis` reports that extent as its length
        SplinePiece piece;
        piece.halfExtents = Vector3(0.05f, 0.5f, 0.05f);
        (axis == SplinePieceAxis::X ? piece.halfExtents.x : piece.halfExtents.z) = 1.5f;
        ASSERT_EQ(piece.axis(), axis);
        const SplinePiecePose t = makePiecePose(pose, piece, scaleAlong);

        // Column 0 is where local +X points, column 2 where local +Z points.
        const int measuredColumn = axis == SplinePieceAxis::X ? 0 : 2;
        const int acrossColumn = axis == SplinePieceAxis::X ? 2 : 0;
        const Vector3 measured = fenceColumn(t, measuredColumn);
        const Vector3 across = fenceColumn(t, acrossColumn);

        // The measured axis is the one on the travel direction...
        EXPECT_NEAR(fenceDot(measured, travel), scaleAlong, 1.0e-3f);
        // ...and the only one the length scale reaches.
        EXPECT_NEAR(fenceLength(measured), scaleAlong, 1.0e-3f);
        EXPECT_NEAR(fenceLength(across), 1.0f, 1.0e-3f);
        EXPECT_NEAR(fenceDot(across, travel), 0.0f, 1.0e-3f);
        EXPECT_GT(fenceBasisDeterminant(t), 0.0f);

        EXPECT_EQ(choosePieceAxis(piece.halfExtents), axis);
        EXPECT_NEAR(pieceLength(piece.halfExtents, axis), 3.0f, 1.0e-4f);
    }
}

// The measured kit table. Spans and walls run along X; posts, towers and slabs are near-square
// and keep the Z convention.
TEST(SplineFenceLayoutTests, MeasuredKitPiecesResolveToTheAxisTheyWereModelledOn)
{
    struct Case{
        const char* name;
        Vector3 halfExtents;
        SplinePieceAxis expected;
    };
    const Case cases[] = {
        // Spans and walls: modelled running along X.
        {"SM_Prop_Fence_01", Vector3(1.2353f, 0.8194f, 0.0784f), SplinePieceAxis::X},
        {"SM_Env_Fence_Wood_01", Vector3(1.2878f, 0.7130f, 0.0863f), SplinePieceAxis::X},
        {"SM_Env_StoneWall_01", Vector3(2.5647f, 0.5447f, 0.3041f), SplinePieceAxis::X},
        {"SM_Env_StoneWall_03", Vector3(0.6534f, 0.5353f, 0.2339f), SplinePieceAxis::X},
        {"SM_Bld_Castle_Wall_01", Vector3(2.5000f, 2.5000f, 0.2500f), SplinePieceAxis::X},
        {"SM_Bld_Railing_01", Vector3(1.3060f, 0.5500f, 0.1120f), SplinePieceAxis::X},
        {"SM_Env_Hedge_03", Vector3(1.0452f, 0.6712f, 0.5600f), SplinePieceAxis::X},
        // Posts: no run direction to read.
        {"SM_Env_StoneWall_Pillar_01", Vector3(0.5033f, 0.5834f, 0.5021f), SplinePieceAxis::Z},
        {"SM_Bld_Castle_Wall_Tower_S_01", Vector3(1.2218f, 3.7620f, 1.2125f), SplinePieceAxis::Z},
        {"SM_Bld_Railing_Pole_01", Vector3(0.1120f, 0.5500f, 0.1120f), SplinePieceAxis::Z},
        // The footpath tile: 2.62 x 2.38 m, a 10% edge is modelling noise, not a run direction.
        {"SM_Env_Footpath_01", Vector3(1.3121f, 0.1333f, 1.1878f), SplinePieceAxis::Z},
        // An amorphous hedge cube, 1.045 x 1.031 m: the near-square rule takes it.
        {"SM_Env_Hedge_04", Vector3(0.5227f, 0.6526f, 0.5156f), SplinePieceAxis::Z},
    };

    for (const Case& c : cases){
        SplinePiece bounds;
        bounds.halfExtents = c.halfExtents;
        EXPECT_EQ(bounds.axis(), c.expected) << c.name;
        EXPECT_NEAR(bounds.length(), (c.expected == SplinePieceAxis::X ? c.halfExtents.x : c.halfExtents.z) * 2.0f, 1.0e-4f)
            << c.name;
    }
}

// Posts are point-placed and near-square: local +X on the LEFT of travel (-right), +Y on up, +Z
// on forward, the mesh origin on the pose position.
TEST(SplineFenceLayoutTests, NearSquarePostKeepsTheZConventionAndItsPlacement)
{
    const auto center = fenceStraightLine(12.0f, 61u);
    const float boundaries[2] = {0.0f, 60.0f};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = 3.0f;
    const SplinePiece posts[1] = {fenceMeasuredKitPost()};
    params.postPieces = posts;

    ASSERT_EQ(posts[0].axis(), SplinePieceAxis::Z);

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_EQ(result.stations.size(), 5u);
    for (const SplineFenceStation& station : result.stations){
        const SplinePiecePose t = makePiecePose(station.pose, posts[0], 1.0f);
        EXPECT_NEAR(fenceDot(fenceColumn(t, 0), station.pose.right), -1.0f, 1.0e-4f);
        EXPECT_NEAR(fenceDot(fenceColumn(t, 1), station.pose.up), 1.0f, 1.0e-4f);
        EXPECT_NEAR(fenceDot(fenceColumn(t, 2), station.pose.forward), 1.0f, 1.0e-4f);
        EXPECT_NEAR(t.position.x, station.pose.position.x, 1.0e-4f);
        EXPECT_NEAR(t.position.y, station.pose.position.y, 1.0e-4f);
        EXPECT_NEAR(t.position.z, station.pose.position.z, 1.0e-4f);
    }
}

// A span whose two stations sit vertically above one another has no ground-plane direction at
// all; roll falls back to station A's frame instead of producing a zero basis.
TEST(SplineFenceLayoutTests, NearVerticalSpanFallsBackWithoutDegenerateBasis)
{
    std::vector<SplineCenterSample> center;
    for (uint32_t i = 0; i < 9u; ++i){
        center.push_back({Vector3(0.0f, static_cast<float>(i) * 0.5f, 0.0f), Vector3(0, 1, 0), true});
    }
    const float boundaries[2] = {0.0f, 8.0f};
    const SplinePiece pieces[1] = {fencePiece(4.0f)};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = 4.0f;
    params.spanPieces = pieces;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_FALSE(result.spans.empty());
    for (const SplineFenceSpan& s : result.spans){
        EXPECT_NEAR(fenceDot(s.pose.right, s.pose.right), 1.0f, 1.0e-4f);
        EXPECT_NEAR(fenceDot(s.pose.up, s.pose.up), 1.0f, 1.0e-4f);
        EXPECT_NEAR(fenceDot(s.pose.forward, s.pose.forward), 1.0f, 1.0e-4f);
        // A's up, not merely a unit basis: a horizontal up would be a flat-lying panel.
        EXPECT_NEAR(s.pose.up.y, 1.0f, 1.0e-4f);
    }
}

// A run too long for the layout budget comes back short AND says so.
TEST(SplineFenceLayoutTests, RunPastThePieceBudgetIsTruncatedAndReported)
{
    const auto center = fenceStraightLine(100.0f, 201u);
    const float boundaries[2] = {0.0f, 200.0f};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPitch = 0.5f; // 201 stations
    params.maxPieces = 20u;  // deliberately far too small

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    EXPECT_FALSE(result.stations.empty());
    EXPECT_LE(result.stations.size(), 20u);
    EXPECT_FALSE(result.validation.empty());
    EXPECT_TRUE(fenceAnyValidationContains(result, "budget"));
}

// ---- Near-vertical spans on real ground: the roll rule ---------------------
//
// On a near-vertical FACE the stations stack almost vertically, the span's forward comes out
// within a degree of vertical, and deriving its up from a cross product with right yields a
// HORIZONTAL up: the panel lies flat while its basis stays orthonormal and finite. The threshold
// is a DEGENERACY escape, not a lean ceiling: racking survives 45 and 60 degrees joining its
// posts exactly, and the fallback only takes over near vertical.

namespace{

    // Ground climbing `rise` metres over `run` metres of horizontal travel along -Z, sampled
    // densely so stations land ON the face.
    std::vector<SplineCenterSample> fenceSteepFace(float run, float rise, uint32_t sampleCount){
        std::vector<SplineCenterSample> center;
        for (uint32_t i = 0; i < sampleCount; ++i){
            const float t = static_cast<float>(i) / static_cast<float>(sampleCount - 1u);
            SplineCenterSample s;
            s.position = Vector3(0.0f, rise * t, -(run * t));
            // the fence default is slopeBlend 0, so stations stay world-up
            s.normal = Vector3(0.0f, 1.0f, 0.0f);
            center.push_back(s);
        }
        return center;
    }

    SplinePiece fencePanelPiece(){
        SplinePiece piece;
        piece.center = Vector3(0.0f, 0.6f, 0.0f);
        piece.halfExtents = Vector3(0.06f, 0.6f, 1.2f); // 2.4 m long, 1.2 m tall
        return piece;
    }

    SplineFenceLayoutParams fencePanelRunParams(std::span<const float> boundaries, const SplinePiece& piece){
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = std::span<const SplinePiece>(&piece, 1u);
        params.postPieces = std::span<const SplinePiece>(&piece, 1u);
        return params;
    }

    // A ramp of the given grade, long enough to carry several spans.
    std::vector<SplineCenterSample> fenceGradeRamp(float degrees, float run, uint32_t sampleCount){
        return fenceRamp(run, run * std::tan(degrees * FENCE_PI / 180.0f), sampleCount);
    }

}

// A racked span follows the grade it climbs, however steep, and still reaches both posts.
TEST(SplineFenceLayoutTests, RackedSpansStillFollowSteepGradesAndJoinTheirPosts)
{
    const SplinePiece piece = fencePanelPiece();
    for (const float grade : {20.0f, 45.0f, 60.0f, 75.0f}){
        const auto center = fenceGradeRamp(grade, 20.0f, 161u);
        const float boundaries[2] = {0.0f, 160.0f};
        const SplineFenceLayoutResult result = buildFenceLayout(center, fencePanelRunParams(boundaries, piece));
        ASSERT_FALSE(result.spans.empty()) << grade << " degrees";
        EXPECT_EQ(result.rollBorrowedSpans, 0u) << grade << " degrees must not trip the degeneracy escape";

        for (const SplineFenceSpan& span : result.spans){
            // pitches to the grade rather than being held level
            EXPECT_NEAR(span.pose.forward.y, std::sin(grade * FENCE_PI / 180.0f), 0.02f) << grade << " degrees, span " << span.index;
            EXPECT_NEAR(span.pose.up.y, std::cos(grade * FENCE_PI / 180.0f), 0.02f) << grade << " degrees, span " << span.index;
        }

        // ...and each span still reaches both of the posts it joins
        ASSERT_GE(result.stations.size(), result.spans.size() + 1u) << grade << " degrees";
        for (size_t i = 0; i < result.spans.size(); ++i){
            const SplineFenceSpan& span = result.spans[i];
            const float half = 0.5f * span.lengthScale * piece.length();
            const Vector3 tail = span.pose.base - span.pose.forward * half;
            const Vector3 head = span.pose.base + span.pose.forward * half;
            const Vector3& postA = result.stations[i].pose.base;
            const Vector3& postB = result.stations[i + 1u].pose.base;
            EXPECT_NEAR(fenceLength(tail - postA), 0.0f, 1.0e-3f) << grade << " degrees, span " << i << " tail misses its post";
            EXPECT_NEAR(fenceLength(head - postB), 0.0f, 1.0e-3f) << grade << " degrees, span " << i << " head misses its post";
        }
    }
}

TEST(SplineFenceLayoutTests, TheRollEscapeOnlyFiresNearVertical)
{
    const SplinePiece piece = fencePanelPiece();
    const float boundaries[2] = {0.0f, 160.0f};

    // Just under the threshold: still racked, still joining its posts.
    const auto shallow = fenceGradeRamp(80.0f, 20.0f, 161u);
    const SplineFenceLayoutResult below = buildFenceLayout(shallow, fencePanelRunParams(boundaries, piece));
    ASSERT_FALSE(below.spans.empty());
    EXPECT_EQ(below.rollBorrowedSpans, 0u);
    EXPECT_NEAR(below.spans.front().pose.forward.y, std::sin(80.0f * FENCE_PI / 180.0f), 0.02f);

    // Past it: the escape takes over and the run reports it.
    const auto steep = fenceGradeRamp(88.0f, 20.0f, 161u);
    const SplineFenceLayoutResult above = buildFenceLayout(steep, fencePanelRunParams(boundaries, piece));
    ASSERT_FALSE(above.spans.empty());
    EXPECT_GT(above.rollBorrowedSpans, 0u);
}

TEST(SplineFenceLayoutTests, ANearVerticalSpanStaysUprightInsteadOfLyingFlat)
{
    const auto center = fenceSteepFace(0.4f, 12.0f, 161u);
    const float boundaries[2] = {0.0f, 160.0f};
    const SplinePiece piece = fencePanelPiece();

    const SplineFenceLayoutResult result = buildFenceLayout(center, fencePanelRunParams(boundaries, piece));
    ASSERT_FALSE(result.spans.empty());
    EXPECT_GT(result.rollBorrowedSpans, 0u) << "fixture must reach the degenerate case";

    for (const SplineFenceSpan& span : result.spans){
        EXPECT_GT(span.pose.up.y, std::cos(36.0f * FENCE_PI / 180.0f))
            << "span " << span.index << " up = (" << span.pose.up.x << ", " << span.pose.up.y << ", " << span.pose.up.z << ")";
        EXPECT_NEAR(fenceDot(span.pose.right, span.pose.right), 1.0f, 1.0e-4f);
        EXPECT_NEAR(fenceDot(span.pose.up, span.pose.up), 1.0f, 1.0e-4f);
        EXPECT_NEAR(fenceDot(span.pose.forward, span.pose.forward), 1.0f, 1.0e-4f);
        EXPECT_NEAR(fenceDot(span.pose.right, span.pose.up), 0.0f, 1.0e-4f);
        EXPECT_NEAR(fenceDot(span.pose.right, span.pose.forward), 0.0f, 1.0e-4f);
        EXPECT_NEAR(fenceDot(span.pose.up, span.pose.forward), 0.0f, 1.0e-4f);
    }
}

// The bury check models a span's underside from the span's OWN pose: a span that borrowed its
// roll is level across a face its posts climb.
TEST(SplineFenceLayoutTests, BuryCheckSeesASpanThatBorrowedItsRoll)
{
    const auto center = fenceSteepFace(0.4f, 12.0f, 161u);
    const float boundaries[2] = {0.0f, 160.0f};
    const SplinePiece piece = fencePanelPiece();

    const SplineFenceLayoutResult result = buildFenceLayout(center, fencePanelRunParams(boundaries, piece));
    ASSERT_GT(result.rollBorrowedSpans, 0u);
    EXPECT_TRUE(fenceAnyValidationContains(result, "ground rises through the spans"))
        << "upright panels crossing the face must not pass the bury check silently";
}

TEST(SplineFenceLayoutTests, BuryCheckStillPassesACleanRackedRun)
{
    const auto center = fenceGradeRamp(20.0f, 20.0f, 161u);
    const float boundaries[2] = {0.0f, 160.0f};
    const SplinePiece piece = fencePanelPiece();

    const SplineFenceLayoutResult result = buildFenceLayout(center, fencePanelRunParams(boundaries, piece));
    ASSERT_FALSE(result.spans.empty());
    EXPECT_EQ(result.rollBorrowedSpans, 0u);
    EXPECT_FALSE(fenceAnyValidationContains(result, "ground rises through the spans"));
}

// ---- The skirt a kit models below its own pivot ----------------------------

namespace{

    // The measured fence kit's panel: the pivot IS the plant line, 0.5216 m of panel below it.
    SplinePiece fenceSkirtedPanelPiece(){
        SplinePiece piece;
        piece.center = Vector3(0.0f, 0.29776f, 0.0f);
        piece.halfExtents = Vector3(0.06f, 0.81937f, 1.2f); // 2.4 m long, 1.6387 m tall
        return piece;
    }

    // The same silhouette and footprint, modelled with its bottom ON the pivot.
    SplinePiece fenceFlushPanelPiece(){
        SplinePiece piece;
        piece.center = Vector3(0.0f, 0.81937f, 0.0f);
        piece.halfExtents = Vector3(0.06f, 0.81937f, 1.2f);
        return piece;
    }

    struct FenceGroundBump{
        // distance along the run
        float at = 0.0f;
        float height = 0.0f;
        float width = 0.0f;
    };

    // A flat centerline along -Z carrying raised-cosine humps. Put a crest MID-SPAN and narrower
    // than the station pitch: a crest at a station lifts the station instead. At the default
    // 2.4 m pitch on a 24 m run the mid-span crests are 1.2, 3.6, 6.0, ... 10.8.
    std::vector<SplineCenterSample> fenceBumpedRun(float length, uint32_t sampleCount, std::initializer_list<FenceGroundBump> bumps){
        std::vector<SplineCenterSample> center;
        for (uint32_t i = 0; i < sampleCount; ++i){
            const float d = length * static_cast<float>(i) / static_cast<float>(sampleCount - 1u);
            float y = 0.0f;
            for (const FenceGroundBump& bump : bumps){
                const float off = std::abs(d - bump.at);
                if (off < bump.width * 0.5f){
                    y = std::max(y, bump.height * 0.5f * (1.0f + std::cos(2.0f * FENCE_PI * off / bump.width)));
                }
            }
            SplineCenterSample s;
            s.position = Vector3(0.0f, y, -d);
            s.normal = Vector3(0.0f, 1.0f, 0.0f);
            center.push_back(s);
        }
        return center;
    }

    // No post mesh, which is what the measured fence kits ship.
    SplineFenceLayoutParams fenceSkirtRunParams(std::span<const float> boundaries, const SplinePiece& spanPiece){
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = std::span<const SplinePiece>(&spanPiece, 1u);
        return params;
    }

    struct FenceBuryReport{
        bool found = false;
        uint32_t samples = 0;
        uint32_t span = 0;
        float depth = 0.0f;
        float arcMetres = 0.0f;
        float x = 0.0f;
        float z = 0.0f;
    };

    // Reads the figures back out of the author-facing string.
    FenceBuryReport fenceParseBuryReport(const SplineFenceLayoutResult& result){
        FenceBuryReport report;
        for (const std::string& message : result.validation){
            if (message.find("ground rises through the spans") == std::string::npos){
                continue;
            }
            const size_t runAt = message.find("run ");
            const size_t worstAt = message.find("worst at span ");
            if (runAt == std::string::npos || worstAt == std::string::npos){
                continue;
            }
            unsigned from = 0;
            unsigned to = 0;
            unsigned samples = 0;
            if (std::sscanf(message.c_str() + runAt, "run %u->%u at %u sample(s)", &from, &to, &samples) != 3){
                continue;
            }
            unsigned span = 0;
            if (std::sscanf(message.c_str() + worstAt,
                            "worst at span %u, %f m deeper than the piece is planted for, %f m along the run at (%f, %f)",
                            &span, &report.depth, &report.arcMetres, &report.x, &report.z) != 5){
                continue;
            }
            report.samples = samples;
            report.span = span;
            report.found = true;
            return report;
        }
        return report;
    }

}

TEST(SplineFenceLayoutTests, GroundInsideTheKitsOwnSkirtIsNotBurial)
{
    // Ground 0.30 m over the plant line: past the 0.05 m tolerance, inside the 0.5216 m skirt.
    const auto center = fenceBumpedRun(24.0f, 241u, {{10.8f, 0.30f, 1.6f}});
    const float boundaries[2] = {0.0f, 240.0f};

    // Positive control: the same ground under a panel with NO skirt is burial.
    const SplinePiece flush = fenceFlushPanelPiece();
    const SplineFenceLayoutResult control = buildFenceLayout(center, fenceSkirtRunParams(boundaries, flush));
    ASSERT_FALSE(control.spans.empty());
    ASSERT_TRUE(fenceAnyValidationContains(control, "ground rises through the spans"))
        << "fixture must reach the case: a skirtless panel on this ground IS buried";

    const SplinePiece skirted = fenceSkirtedPanelPiece();
    const SplineFenceLayoutResult result = buildFenceLayout(center, fenceSkirtRunParams(boundaries, skirted));
    ASSERT_EQ(result.spans.size(), control.spans.size()) << "the two arms must differ in the skirt and nothing else";
    EXPECT_FALSE(fenceAnyValidationContains(result, "ground rises through the spans"))
        << "0.30 m of ground over the pivot is inside a 0.52 m skirt, by design";
}

TEST(SplineFenceLayoutTests, GroundPastTheSkirtIsStillBurial)
{
    // 0.80 m of ground over the plant line eats the whole skirt and 0.28 m of panel with it.
    const auto center = fenceBumpedRun(24.0f, 241u, {{10.8f, 0.80f, 1.6f}});
    const float boundaries[2] = {0.0f, 240.0f};
    const SplinePiece skirted = fenceSkirtedPanelPiece();

    const SplineFenceLayoutResult result = buildFenceLayout(center, fenceSkirtRunParams(boundaries, skirted));
    ASSERT_FALSE(result.spans.empty());
    ASSERT_TRUE(fenceAnyValidationContains(result, "ground rises through the spans"))
        << "ground past the skirt is swallowing panel, and must still be reported";

    // The depth reported is what is left AFTER the skirt is spent: 0.80 - 0.5216 m.
    const FenceBuryReport report = fenceParseBuryReport(result);
    ASSERT_TRUE(report.found);
    EXPECT_NEAR(report.depth, 0.278f, 0.05f) << "the message must charge the ground only for what the skirt does not cover";
}

TEST(SplineFenceLayoutTests, BoundsMinPlantingIsAllowedNoSkirt)
{
    // BoundsMin stands a piece's lowest vertex ON the surface, so it gets no allowance.
    const auto center = fenceBumpedRun(24.0f, 241u, {{10.8f, 0.30f, 1.6f}});
    const float boundaries[2] = {0.0f, 240.0f};
    const SplinePiece skirted = fenceSkirtedPanelPiece();

    SplineFenceLayoutParams params = fenceSkirtRunParams(boundaries, skirted);
    params.plantMode = SplinePlantMode::BoundsMin;
    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_FALSE(result.spans.empty());
    EXPECT_TRUE(fenceAnyValidationContains(result, "ground rises through the spans"))
        << "a piece standing ON the surface has no skirt to be buried into";
}

TEST(SplineFenceLayoutTests, TheBuryReportNamesTheWorstStationNotTheFirst)
{
    // Two bumps, the shallow one FIRST along the run; the station to add is at the deep one.
    const auto center = fenceBumpedRun(24.0f, 241u, {{6.0f, 0.30f, 1.6f}, {18.0f, 0.90f, 1.6f}});
    const float boundaries[2] = {0.0f, 240.0f};
    const SplinePiece flush = fenceFlushPanelPiece();

    const SplineFenceLayoutResult result = buildFenceLayout(center, fenceSkirtRunParams(boundaries, flush));
    ASSERT_FALSE(result.spans.empty());

    const FenceBuryReport report = fenceParseBuryReport(result);
    ASSERT_TRUE(report.found) << "the report must name the worst span, its depth and where it is";
    EXPECT_GE(report.samples, 2u) << "both bumps stand over their spans";
    EXPECT_GT(report.depth, 0.60f) << "0.30 m would mean the walk reported the FIRST bump, not the worst";
    EXPECT_LT(report.depth, 1.10f) << "no sample on this ground stands a metre over its span";
    EXPECT_NEAR(report.arcMetres, 18.0f, 2.0f) << "the deep bump sits 18 m along the run";
    EXPECT_NEAR(report.z, -18.0f, 2.0f);
    EXPECT_NEAR(report.x, 0.0f, 0.01f) << "the centerline runs along -Z at x = 0";
    EXPECT_GT(report.span, 4u) << "the shallow bump falls on span 2 or so";
}

TEST(SplineFenceLayoutTests, IndependentCompressionFloorKeepsLongRunsWithinStretchCap)
{
    const std::vector<SplineCenterSample> center = {
        {Vector3(0, 0, 0), Vector3(0, 1, 0), true}, {Vector3(0, 0, -.75f), Vector3(0, 1, 0), true},
        {Vector3(0, 0, -8.16f), Vector3(0, 1, 0), true}};
    const float boundaries[] = {0, 1, 2};
    const SplinePiece pieces[] = {fencePiece(5.0f)};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = pieces;
    params.spanMaxStretch = 1.25f;
    params.spanMinScale = .15f;
    const auto result = buildFenceLayout(center, params);
    EXPECT_TRUE(result.validation.empty());
    ASSERT_EQ(result.spans.size(), 3u);
    EXPECT_NEAR(result.spans[0].lengthScale, .15f, 1e-4f);
    EXPECT_NEAR(result.spans[1].lengthScale, .741f, 1e-4f);
    EXPECT_NEAR(result.spans[2].lengthScale, .741f, 1e-4f);
    EXPECT_EQ(result.spans[0].run, 0u);
    EXPECT_EQ(result.spans[1].run, 1u);
    EXPECT_NEAR(result.stations.back().pose.base.z, -8.16f, 1e-4f);
}

TEST(SplineFenceLayoutTests, ExplicitCompressionFailureNamesItsOwnFloor)
{
    const auto center = fenceStraightLine(1.0f, 3u);
    const float boundaries[] = {0, 2};
    const SplinePiece pieces[] = {fencePiece(5.0f)};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = pieces;
    params.spanMinScale = .5f;
    const auto result = buildFenceLayout(center, params);
    EXPECT_TRUE(fenceAnyValidationContains(result, "under the 0.500x floor"));
    EXPECT_TRUE(fenceAnyValidationContains(result, "explicit compression limit"));
    EXPECT_FALSE(fenceAnyValidationContains(result, "reciprocal"));
}

TEST(SplineFenceLayoutTests, InvalidCompressionFloorPreservesLegacyReciprocal)
{
    const auto center = fenceStraightLine(3.0f, 7u);
    const float boundaries[] = {0, 6};
    const SplinePiece pieces[] = {fencePiece(2.0f)};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = pieces;
    params.spanMaxStretch = 1.25f;
    for (const float floor : {0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}){
        params.spanMinScale = floor;
        const auto result = buildFenceLayout(center, params);
        EXPECT_TRUE(fenceAnyValidationContains(result, "under the 0.800x floor"));
        EXPECT_TRUE(fenceAnyValidationContains(result, "reciprocal"));
    }
    params.spanMinScale = 2.0f;
    EXPECT_TRUE(fenceAnyValidationContains(buildFenceLayout(center, params), "under the 1.000x floor"));
}

// ---- Crest row: the layout does not move when the row is armed ------------

namespace{

    // FNV-1a over the BIT PATTERNS of every float a layout places and every integer it keys
    // them by: "nothing moved" as one exact claim.
    struct FenceFingerprint{
        uint64_t value = 0xCBF29CE484222325ull;

        void mixBits(uint64_t v){
            for (int shift = 0; shift < 64; shift += 8){
                value ^= (v >> shift) & 0xFFull;
                value *= 0x100000001B3ull;
            }
        }
        void mixFloat(float v){
            mixBits(fenceFloatBits(v));
        }
        void mixVector(const Vector3& v){
            mixFloat(v.x);
            mixFloat(v.y);
            mixFloat(v.z);
        }
        void mixPose(const SplineLayoutPose& pose){
            mixVector(pose.position);
            mixVector(pose.right);
            mixVector(pose.up);
            mixVector(pose.forward);
            mixVector(pose.base);
            mixBits(pose.stationIndex);
        }
    };

    uint64_t fenceFingerprintStationsAndSpans(const SplineFenceLayoutResult& result){
        FenceFingerprint print;
        print.mixBits(result.stations.size());
        for (const SplineFenceStation& station : result.stations){
            print.mixPose(station.pose);
            print.mixBits(station.index);
            print.mixBits(station.knot);
            print.mixBits(station.isKnot ? 1u : 0u);
        }
        print.mixBits(result.spans.size());
        for (const SplineFenceSpan& span : result.spans){
            print.mixPose(span.pose);
            print.mixFloat(span.lengthScale);
            print.mixBits(span.poolSlot);
            print.mixBits(span.index);
            print.mixBits(span.run);
            print.mixBits(span.ordinalInRun);
            print.mixBits(span.isGate ? 1u : 0u);
        }
        print.mixBits(result.rollBorrowedSpans);
        return print.value;
    }

    // The crest row every identity fixture is rebuilt with: two crest lengths, a cap pool, a
    // typed pitch and a reservation, all armed at once.
    void fenceArmCrestRow(SplineFenceLayoutParams& params){
        static const SplinePiece crests[2] = {fencePiece(1.0f), fencePiece(0.6f)};
        static const SplinePiece caps[1] = {fencePiece(0.4f)};
        static const SplineFenceReservation reserved[1] = {{0u, 2.0f, 3.0f}};
        params.crestPieces = crests;
        params.capPieces = caps;
        params.reservations = reserved;
        // below the longest piece on purpose: the clamp report is then armed too
        params.crestPitch = 0.5f;
    }

    // What an identity fixture is built with beyond its own geometry.
    constexpr uint32_t FENCE_ARM_NOTHING = 0u;
    // fenceArmCrestRow: every crest-row feature at once
    constexpr uint32_t FENCE_ARM_CREST_ROW = 1u << 0;
    // a full, untouched override table and a gate pool
    constexpr uint32_t FENCE_ARM_EMPTY_OVERRIDE_TABLE = 1u << 1;

    void fenceArmFixture(SplineFenceLayoutParams& params, uint32_t arms){
        if ((arms & FENCE_ARM_CREST_ROW) != 0u){
            fenceArmCrestRow(params);
        }
        if ((arms & FENCE_ARM_EMPTY_OVERRIDE_TABLE) != 0u){
            static const SplineSpanOverride untouched[SPLINE_FENCE_MAX_SPAN_OVERRIDES] = {};
            static const SplinePiece gates[1] = {fencePiece(3.0f)};
            params.spanOverrides = untouched;
            params.gatePieces = gates;
        }
    }

    SplineFenceLayoutResult fenceFixtureStraightPanels(uint32_t arms){
        const auto center = fenceStraightLine(20.0f, 41u);
        const float boundaries[3] = {0.0f, 20.0f, 40.0f};
        const SplinePiece spans[1] = {fenceMeasuredKitPanel()};
        const SplinePiece posts[1] = {fenceMeasuredKitPost()};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        params.postPieces = posts;
        fenceArmFixture(params, arms);
        return buildFenceLayout(center, params);
    }

    SplineFenceLayoutResult fenceFixtureRampRacked(uint32_t arms){
        const auto center = fenceRamp(20.0f, 4.0f, 41u);
        const float boundaries[2] = {0.0f, 40.0f};
        const SplinePiece spans[1] = {fenceMeasuredKitPanel()};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        params.spanGrade = SplineSpanGrade::Racked;
        fenceArmFixture(params, arms);
        return buildFenceLayout(center, params);
    }

    SplineFenceLayoutResult fenceFixtureRampSteppedOnBoundsMin(uint32_t arms){
        const auto center = fenceRamp(20.0f, 4.0f, 41u);
        const float boundaries[2] = {0.0f, 40.0f};
        const SplinePiece spans[1] = {fenceMeasuredKitPanel()};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        params.spanGrade = SplineSpanGrade::Stepped;
        params.plantMode = SplinePlantMode::BoundsMin;
        fenceArmFixture(params, arms);
        return buildFenceLayout(center, params);
    }

    SplineFenceLayoutResult fenceFixtureCorner(uint32_t arms){
        const auto center = fenceCorneredL(6.0f, 6.0f, 0.5f);
        const float boundaries[3] = {0.0f, 12.0f, 24.0f};
        const SplinePiece spans[1] = {fencePiece(2.0f)};
        const SplinePiece posts[1] = {fenceMeasuredKitPost()};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        params.postPieces = posts;
        fenceArmFixture(params, arms);
        return buildFenceLayout(center, params);
    }

    SplineFenceLayoutResult fenceFixtureClosedSquare(uint32_t arms){
        const auto center = fenceClosedSquare(8.0f, 0.5f);
        const float boundaries[5] = {0.0f, 16.0f, 32.0f, 48.0f, 64.0f};
        const SplinePiece spans[1] = {fencePiece(4.0f)};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        params.closed = true;
        fenceArmFixture(params, arms);
        return buildFenceLayout(center, params);
    }

    SplineFenceLayoutResult fenceFixtureStationsOnly(uint32_t arms){
        const auto center = fenceStraightLine(12.0f, 25u);
        const float boundaries[2] = {0.0f, 24.0f};
        const SplinePiece posts[1] = {fenceMeasuredKitPost()};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.postPitch = 2.4f;
        params.postPieces = posts;
        fenceArmFixture(params, arms);
        return buildFenceLayout(center, params);
    }

    SplineFenceLayoutResult fenceFixtureMixedPoolSeeded(uint32_t arms){
        const auto center = fenceStraightLine(18.0f, 37u);
        const float boundaries[3] = {0.0f, 18.0f, 36.0f};
        const SplinePiece spans[3] = {fencePiece(1.5f), fencePiece(2.0f), fencePiece(3.0f)};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        params.seed = 7u;
        fenceArmFixture(params, arms);
        return buildFenceLayout(center, params);
    }

    SplineFenceLayoutResult fenceFixtureUnsatisfiableStretch(uint32_t arms){
        const auto center = fenceStraightLine(3.0f, 7u);
        const float boundaries[2] = {0.0f, 6.0f};
        const SplinePiece spans[1] = {fencePiece(5.0f)};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        params.spanMinScale = 0.5f;
        fenceArmFixture(params, arms);
        return buildFenceLayout(center, params);
    }

    // A level arc of `radius` metres turning `degrees` right toward +X from the origin, heading
    // -Z, sampled every `step` metres of arc. Chords of a curve are shorter than their arc, so a
    // fill of whole walls closes each run at a stretch other than 1.
    std::vector<SplineCenterSample> fenceLevelArc(float radius, float degrees, float step){
        const float arc = radius * degrees * FENCE_PI / 180.0f;
        const uint32_t steps = static_cast<uint32_t>(std::lround(arc / step));
        std::vector<SplineCenterSample> center;
        for (uint32_t i = 0; i <= steps; ++i){
            const float angle = arc * static_cast<float>(i) / static_cast<float>(steps) / radius;
            center.push_back({Vector3(radius - radius * std::cos(angle), 0.0f, -(radius * std::sin(angle))), Vector3(0, 1, 0), true});
        }
        return center;
    }

    // The castle kit's 5 m wall: 5 x 5 x 0.5 m, laid along local X and pivoted on its corner.
    SplinePiece fenceCastleWall(){
        SplinePiece b;
        b.halfExtents = Vector3(2.5f, 2.5f, 0.25f);
        b.center = Vector3(-2.5f, 2.5f, 0.0f);
        return b;
    }

    // A curved castle wall of three runs: whole 5 m walls over a 30 m, 150 degree arc, with
    // knots splitting it unevenly.
    std::vector<SplineCenterSample> fenceCurvedCastleCenter(){
        return fenceLevelArc(30.0f, 150.0f, 0.05f);
    }

    void fenceCurvedCastleBoundaries(const std::vector<SplineCenterSample>& center, float (&out)[4]){
        const float last = static_cast<float>(center.size() - 1u);
        out[0] = 0.0f;
        out[1] = std::round(last * 0.37f);
        out[2] = std::round(last * 0.63f);
        out[3] = last;
    }

    SplineFenceLayoutResult fenceFixtureCurvedCastle(uint32_t arms){
        const auto center = fenceCurvedCastleCenter();
        float boundaries[4];
        fenceCurvedCastleBoundaries(center, boundaries);
        const SplinePiece spans[1] = {fenceCastleWall()};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        fenceArmFixture(params, arms);
        return buildFenceLayout(center, params);
    }

    struct FenceIdentityFixture{
        const char* name;
        SplineFenceLayoutResult (*build)(uint32_t arms);
        // whether the armed build must actually lay a row
        bool carriesACrestRow;
        // the armed row, crest by crest
        std::span<const FenceCrestGolden> crestRow;
    };

    const FenceIdentityFixture FENCE_IDENTITY_FIXTURES[] = {
        {"straight panels with posts", fenceFixtureStraightPanels, true, fencegolden::FENCE_GOLDEN_STRAIGHT_PANELS},
        {"racked ramp", fenceFixtureRampRacked, true, fencegolden::FENCE_GOLDEN_RAMP_RACKED},
        {"stepped ramp planted on bounds-min", fenceFixtureRampSteppedOnBoundsMin, true, fencegolden::FENCE_GOLDEN_RAMP_STEPPED_BOUNDS_MIN},
        {"cornered L with a bisector", fenceFixtureCorner, true, fencegolden::FENCE_GOLDEN_CORNER},
        {"closed square", fenceFixtureClosedSquare, true, fencegolden::FENCE_GOLDEN_CLOSED_SQUARE},
        {"stations only, no span pool", fenceFixtureStationsOnly, false, {}},
        {"mixed span pool, seeded", fenceFixtureMixedPoolSeeded, true, fencegolden::FENCE_GOLDEN_MIXED_POOL_SEEDED},
        {"run whose stretch cannot be satisfied", fenceFixtureUnsatisfiableStretch, true, fencegolden::FENCE_GOLDEN_UNSATISFIABLE_STRETCH},
        {"curved castle wall, crest shorter than the wall", fenceFixtureCurvedCastle, true, fencegolden::FENCE_GOLDEN_CURVED_CASTLE},
    };

    // One coordinate of a crest against its golden, within cross-architecture rounding.
    void fenceExpectNearGolden(float actual, uint32_t goldenBits, const char* fixture, size_t crest, const char* what){
        float golden = 0.0f;
        std::memcpy(&golden, &goldenBits, sizeof(golden));
        EXPECT_LE(fenceUlpDistance(fenceFloatBits(actual), goldenBits), FENCE_GOLDEN_MAX_ULPS)
            << fixture << ", crest " << crest << " " << what << ": " << actual << " vs golden " << golden
            << " is beyond cross-architecture rounding, so the row really moved";
    }

    void fenceExpectNearGolden(const Vector3& actual, const uint32_t (&goldenBits)[3], const char* fixture, size_t crest, const char* what){
        fenceExpectNearGolden(actual.x, goldenBits[0], fixture, crest, what);
        fenceExpectNearGolden(actual.y, goldenBits[1], fixture, crest, what);
        fenceExpectNearGolden(actual.z, goldenBits[2], fixture, crest, what);
    }

}

// The crest row is decoration laid over a layout that is already solved: arming it must not
// move one station, one span, or one word the layout already said.
TEST(SplineFenceLayoutTests, EmptyCrestPoolLeavesEveryExistingFixtureByteIdentical)
{
    for (const FenceIdentityFixture& fixture : FENCE_IDENTITY_FIXTURES){
        const SplineFenceLayoutResult plain = fixture.build(FENCE_ARM_NOTHING);
        const SplineFenceLayoutResult armed = fixture.build(FENCE_ARM_CREST_ROW);
        EXPECT_TRUE(plain.crests.empty()) << fixture.name;
        EXPECT_EQ(!armed.crests.empty(), fixture.carriesACrestRow) << fixture.name;
        EXPECT_EQ(fenceFingerprintStationsAndSpans(plain), fenceFingerprintStationsAndSpans(armed)) << fixture.name;
        ASSERT_GE(armed.validation.size(), plain.validation.size()) << fixture.name;
        for (size_t i = 0; i < plain.validation.size(); ++i){
            EXPECT_EQ(armed.validation[i], plain.validation[i]) << fixture.name;
        }
    }
}

// The row each identity fixture lays is pinned crest by crest: which cells and pieces exactly,
// where they stand and which way they face in ULPs against OpenEngine's x86-64 capture, mirrored.
TEST(SplineFenceLayoutTests, TheArmedCrestRowOfEveryIdentityFixtureIsPinned)
{
    for (const FenceIdentityFixture& fixture : FENCE_IDENTITY_FIXTURES){
        const SplineFenceLayoutResult armed = fixture.build(FENCE_ARM_CREST_ROW);
        EXPECT_EQ(armed.crests.size(), fixture.crestRow.size()) << fixture.name;
        if (armed.crests.size() != fixture.crestRow.size()){
            continue;
        }
        for (size_t k = 0; k < armed.crests.size(); ++k){
            const SplineFenceCrest& crest = armed.crests[k];
            const FenceCrestGolden& golden = fixture.crestRow[k];
            EXPECT_EQ(crest.index, golden.index) << fixture.name << ", crest " << k;
            EXPECT_EQ(crest.run, golden.run) << fixture.name << ", crest " << k;
            EXPECT_EQ(crest.ordinalInRun, golden.ordinalInRun) << fixture.name << ", crest " << k;
            EXPECT_EQ(crest.poolSlot, golden.poolSlot) << fixture.name << ", crest " << k;
            EXPECT_EQ(crest.isCap, golden.isCap) << fixture.name << ", crest " << k;
            // no identity fixture's crest shares its wall's length: every piece is whole
            EXPECT_EQ(crest.lengthScale, 1.0f) << fixture.name << ", crest " << k;
            fenceExpectNearGolden(crest.pose.position, golden.positionBits, fixture.name, k, "position");
            fenceExpectNearGolden(crest.pose.base, golden.baseBits, fixture.name, k, "base");
            fenceExpectNearGolden(crest.pose.forward, golden.forwardBits, fixture.name, k, "forward");
            fenceExpectNearGolden(crest.pose.up, golden.upBits, fixture.name, k, "up");
        }
    }
}

// ---- Crest row: cells ------------------------------------------------------

namespace{

    // One 10 m run under 5 m spans, a cell every 2 m, a 1 m crest piece: cells at 1, 3, 5, 7, 9 m.
    struct FenceCrestFixture{
        std::vector<SplineCenterSample> center = fenceStraightLine(10.0f, 21u);
        float boundaries[2] = {0.0f, 20.0f};
        SplinePiece spans[1] = {fencePiece(5.0f)};
        SplinePiece crests[1] = {fencePiece(1.0f)};

        SplineFenceLayoutParams params() const{
            SplineFenceLayoutParams p;
            p.knotStations = boundaries;
            p.spanPieces = spans;
            p.crestPieces = crests;
            p.crestPitch = 2.0f;
            return p;
        }
    };

    // Cell identity in emission order (caps carry a cell's ordinal too, so they are left out).
    std::vector<uint32_t> fenceCellOrdinals(const SplineFenceLayoutResult& result){
        std::vector<uint32_t> out;
        for (const SplineFenceCrest& crest : result.crests){
            if (!crest.isCap){
                out.push_back(crest.index);
            }
        }
        return out;
    }

    // How far along a fixture that runs down -Z from the origin each cell stands.
    std::vector<float> fenceCellCentres(const SplineFenceLayoutResult& result){
        std::vector<float> out;
        for (const SplineFenceCrest& crest : result.crests){
            if (!crest.isCap){
                out.push_back(-crest.pose.position.z);
            }
        }
        return out;
    }

    const SplineFenceCrest* fenceFindCell(const SplineFenceLayoutResult& result, uint32_t index){
        for (const SplineFenceCrest& crest : result.crests){
            if (!crest.isCap && crest.index == index){
                return &crest;
            }
        }
        return nullptr;
    }

    std::vector<SplineFenceCrest> fenceCapsOf(const SplineFenceLayoutResult& result){
        std::vector<SplineFenceCrest> out;
        for (const SplineFenceCrest& crest : result.crests){
            if (crest.isCap){
                out.push_back(crest);
            }
        }
        return out;
    }

    // The crest fixture with a 2 m tower in the post pool, 1 m either side of each station, and
    // one crest piece of the given length at the given pitch.
    SplineFenceLayoutResult fenceCrestRowBetweenTowers(float crestLength, float pitch, bool towers){
        FenceCrestFixture fixture;
        fixture.crests[0] = fencePiece(crestLength);
        SplineFenceLayoutParams params = fixture.params();
        params.crestPitch = pitch;
        SplinePiece tower;
        tower.halfExtents = Vector3(1.0f, 1.0f, 1.0f);
        tower.center = Vector3(0.0f, 1.0f, 0.0f);
        const SplinePiece posts[1] = {tower};
        if (towers){
            params.postPieces = posts;
        }
        return buildFenceLayout(fixture.center, params);
    }

    size_t fenceEmptyCrestRowReports(const SplineFenceLayoutResult& result){
        size_t count = 0;
        for (const std::string& message : result.validation){
            count += message.find("crest row placed no crest pieces") != std::string::npos ? 1u : 0u;
        }
        return count;
    }

    void fenceExpectSameVector(const Vector3& a, const Vector3& b, const char* what){
        EXPECT_EQ(a.x, b.x) << what;
        EXPECT_EQ(a.y, b.y) << what;
        EXPECT_EQ(a.z, b.z) << what;
    }

    // Bit for bit: "this piece did not move" is an exact claim.
    void fenceExpectSameCrest(const SplineFenceCrest& a, const SplineFenceCrest& b, const char* what){
        EXPECT_EQ(a.index, b.index) << what;
        EXPECT_EQ(a.ordinalInRun, b.ordinalInRun) << what;
        EXPECT_EQ(a.run, b.run) << what;
        EXPECT_EQ(a.poolSlot, b.poolSlot) << what;
        EXPECT_EQ(a.isCap, b.isCap) << what;
        fenceExpectSameVector(a.pose.position, b.pose.position, what);
        fenceExpectSameVector(a.pose.right, b.pose.right, what);
        fenceExpectSameVector(a.pose.up, b.pose.up, what);
        fenceExpectSameVector(a.pose.forward, b.pose.forward, what);
        fenceExpectSameVector(a.pose.base, b.pose.base, what);
    }

    // Cell ordinals within one run.
    std::vector<uint32_t> fenceOrdinalsInRun(const SplineFenceLayoutResult& result, uint32_t run){
        std::vector<uint32_t> out;
        for (const SplineFenceCrest& crest : result.crests){
            if (!crest.isCap && crest.run == run){
                out.push_back(crest.ordinalInRun);
            }
        }
        return out;
    }

    std::vector<SplineFenceCrest> fenceCrestsOfRun(const SplineFenceLayoutResult& result, uint32_t run){
        std::vector<SplineFenceCrest> out;
        for (const SplineFenceCrest& crest : result.crests){
            if (crest.run == run){
                out.push_back(crest);
            }
        }
        return out;
    }

}

// 5 m battlements over 5 m walls, pitch left at 0: on a straight run the row registers to walls
// at their own length, meets flush, and the last cell closes exactly on the run's end.
TEST(SplineFenceLayoutTests, StraightCastleRowRegistersFlushAtScaleOne)
{
    const auto center = fenceStraightLine(20.0f, 41u);
    const float boundaries[2] = {0.0f, 40.0f};
    const SplinePiece spans[1] = {fencePiece(5.0f)};
    const SplinePiece crests[1] = {fencePiece(5.0f)};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.crestPieces = crests;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_EQ(result.crests.size(), 4u);
    for (uint32_t k = 0; k < 4u; ++k){
        const SplineFenceCrest& crest = result.crests[k];
        EXPECT_EQ(crest.index, k);
        EXPECT_EQ(crest.ordinalInRun, k);
        EXPECT_EQ(crest.run, 0u);
        EXPECT_FALSE(crest.isCap);
        // cell k is [5k, 5k+5) and the piece fills it: consecutive pieces meet
        EXPECT_NEAR(crest.pose.position.z, -(2.5f + 5.0f * static_cast<float>(k)), 1.0e-4f) << k;
        // on the top of the spans, which stand 1 m tall on the centerline
        EXPECT_NEAR(crest.pose.position.y, 1.0f, 1.0e-4f) << k;
        EXPECT_NEAR(fenceLength(crest.pose.forward), 1.0f, 1.0e-5f) << k;
        EXPECT_NEAR(crest.lengthScale, 1.0f, 1.0e-6f) << k;
    }
    EXPECT_TRUE(result.validation.empty());
}

// The fixtures of the closed repeat-layout study, asserted to its own expected ordinals and
// centres.
TEST(SplineFenceLayoutTests, CrestRowMatchesRepeatLayoutOnSharedFixtures)
{
    const FenceCrestFixture fixture;

    const SplineFenceLayoutResult plain = buildFenceLayout(fixture.center, fixture.params());
    EXPECT_EQ(fenceCellOrdinals(plain), (std::vector<uint32_t>{0u, 1u, 2u, 3u, 4u}));
    const std::vector<float> centres = fenceCellCentres(plain);
    ASSERT_EQ(centres.size(), 5u);
    for (uint32_t k = 0; k < 5u; ++k){
        EXPECT_NEAR(centres[k], 1.0f + 2.0f * static_cast<float>(k), 1.0e-4f) << k;
    }

    // A 0.2 m opening at 3.5 m: the cell centred at 3 m has its PIECE under it, and goes.
    SplineFenceLayoutParams params = fixture.params();
    const SplineFenceReservation narrow[1] = {{0u, 3.4f, 3.6f}};
    params.reservations = narrow;
    const SplineFenceLayoutResult cut = buildFenceLayout(fixture.center, params);
    EXPECT_EQ(fenceCellOrdinals(cut), (std::vector<uint32_t>{0u, 2u, 3u, 4u}));
    EXPECT_EQ(fenceFindCell(cut, 1u), nullptr);

    const SplineFenceReservation touching[1] = {{0u, 3.5f, 4.5f}};
    params.reservations = touching;
    EXPECT_EQ(fenceCellOrdinals(buildFenceLayout(fixture.center, params)), (std::vector<uint32_t>{0u, 1u, 2u, 3u, 4u}));

    const SplineFenceReservation nested[6] = {{0u, 4.0f, 6.0f}, {0u, 2.0f, 5.0f}, {0u, 2.5f, 3.0f},
                                              {0u, 4.0f, 6.0f}, {0u, 20.0f, 30.0f}, {0u, -5.0f, -1.0f}};
    params.reservations = nested;
    EXPECT_EQ(fenceCellOrdinals(buildFenceLayout(fixture.center, params)), (std::vector<uint32_t>{0u, 3u, 4u}));
}

// Touching a reservation's edge is not standing under it.
TEST(SplineFenceLayoutTests, CellTouchingAReservationIsKept)
{
    const FenceCrestFixture fixture;
    SplineFenceLayoutParams params = fixture.params();
    const SplineFenceReservation touching[1] = {{0u, 3.5f, 4.5f}};
    params.reservations = touching;
    EXPECT_EQ(fenceCellOrdinals(buildFenceLayout(fixture.center, params)), (std::vector<uint32_t>{0u, 1u, 2u, 3u, 4u}));

    // Half a millimetre of overlap is the same contact measured on a float axis.
    const SplineFenceReservation grazing[1] = {{0u, 3.4995f, 4.5f}};
    params.reservations = grazing;
    EXPECT_EQ(fenceCellOrdinals(buildFenceLayout(fixture.center, params)), (std::vector<uint32_t>{0u, 1u, 2u, 3u, 4u}));

    // Two millimetres is a piece standing under a tower, and goes.
    const SplineFenceReservation biting[1] = {{0u, 3.498f, 4.5f}};
    params.reservations = biting;
    EXPECT_EQ(fenceCellOrdinals(buildFenceLayout(fixture.center, params)), (std::vector<uint32_t>{0u, 2u, 3u, 4u}));
}

// Unsorted, overlapping and nested reservations answer as one merged stretch, and a zero-length
// one reserves nothing at all.
TEST(SplineFenceLayoutTests, OverlappingReservationsMergeAndZeroLengthOnesAreIgnored)
{
    const FenceCrestFixture fixture;
    SplineFenceLayoutParams params = fixture.params();
    const SplineFenceReservation nested[4] = {{0u, 4.0f, 6.0f}, {0u, 2.0f, 5.0f}, {0u, 2.5f, 3.0f}, {0u, 4.0f, 6.0f}};
    params.reservations = nested;
    EXPECT_EQ(fenceCellOrdinals(buildFenceLayout(fixture.center, params)), (std::vector<uint32_t>{0u, 3u, 4u}));

    const SplineFenceReservation empty[3] = {{0u, 3.0f, 3.0f}, {0u, 5.0f, 5.0f}, {0u, 9.0f, 9.0f}};
    params.reservations = empty;
    EXPECT_EQ(fenceCellOrdinals(buildFenceLayout(fixture.center, params)), (std::vector<uint32_t>{0u, 1u, 2u, 3u, 4u}));
}

// Adding a gate (here the stretch its piece occupies) takes the cells under it away and leaves
// every other cell exactly where it stood, under the ordinal and pick it already had.
TEST(SplineFenceLayoutTests, AddedGateRemovesCellsWithoutRephasingSurvivors)
{
    const FenceCrestFixture fixture;
    const SplinePiece crests[2] = {fencePiece(1.0f), fencePiece(0.8f)};
    SplineFenceLayoutParams params = fixture.params();
    params.crestPieces = crests;
    params.seed = 3u;
    const SplineFenceLayoutResult before = buildFenceLayout(fixture.center, params);
    ASSERT_EQ(before.crests.size(), 5u);

    const SplineFenceReservation gate[1] = {{0u, 2.0f, 6.0f}};
    params.reservations = gate;
    const SplineFenceLayoutResult after = buildFenceLayout(fixture.center, params);

    EXPECT_EQ(fenceCellOrdinals(after), (std::vector<uint32_t>{0u, 3u, 4u}));
    for (const SplineFenceCrest& crest : after.crests){
        const SplineFenceCrest* original = fenceFindCell(before, crest.index);
        ASSERT_NE(original, nullptr) << crest.index;
        fenceExpectSameCrest(crest, *original, "survivor of an added gate");
    }
}

// A station's own post reserves the run it stands on, so the row clears every tower the moment a
// post mesh exists.
TEST(SplineFenceLayoutTests, PostFootprintsReserveTheCrestRow)
{
    const FenceCrestFixture fixture;
    SplineFenceLayoutParams params = fixture.params();
    SplinePiece tower;
    tower.halfExtents = Vector3(1.0f, 1.0f, 1.0f); // a 2 m tower, 1 m either side
    tower.center = Vector3(0.0f, 1.0f, 0.0f);
    const SplinePiece posts[1] = {tower};
    params.postPieces = posts;

    // Stations stand at 0, 5 and 10 m, so the cells at 1, 5 and 9 m are under a tower.
    EXPECT_EQ(fenceCellOrdinals(buildFenceLayout(fixture.center, params)), (std::vector<uint32_t>{1u, 3u}));
}

// A bump under a bay: the draped centerline climbs over it, the span between the bay's two posts
// does not. The row fills the span's chord: the same cells, at the same places, a flat run between
// the same two posts gives, and no piece overlaps its neighbour or what stands at either end.
TEST(SplineFenceLayoutTests, CrestRowOverABumpFillsTheSpanItStandsOn)
{
    const auto flat = fenceStraightLine(10.0f, 201u);
    const auto bumped = fenceBumpedLine(10.0f, 3.0f, 7.0f, 2.0f, 201u);
    const float boundaries[2] = {0.0f, 200.0f};
    float drapedLength = 0.0f;
    for (size_t i = 1; i < bumped.size(); ++i){
        drapedLength += fenceLength(bumped[i].position - bumped[i - 1u].position);
    }
    ASSERT_GT(drapedLength, 11.0f) << "fixture must reach the case: the draped run has to be longer than its chord";
    // One span over the whole run, flat or draped.
    const SplinePiece spans[1] = {fencePiece(10.0f)};
    const SplinePiece crests[1] = {fencePiece(1.0f)};
    SplinePiece tower; // 2 m, 1 m either side of its station
    tower.halfExtents = Vector3(1.0f, 1.0f, 1.0f);
    tower.center = Vector3(0.0f, 1.0f, 0.0f);
    const SplinePiece towerPool[1] = {tower};
    constexpr float crestHalf = 0.5f;
    constexpr float towerHalf = 1.0f;
    constexpr float touching = 1.0e-3f;
    // A caller's piece reaching 1.5 m back from the closing knot, measured back from the end of
    // the span (from the end of the draped run, 1.85 m longer, it would cover no cell at all).
    constexpr float closingReserve = 1.5f;
    const SplineFenceReservation closing[1] = {{1u, -closingReserve, 0.0f}};

    struct Arm{
        const char* name;
        bool towers;
        std::span<const SplineFenceReservation> reserved;
        uint32_t cells;
        // how far the first and last crest's centre stand, at least, from the opening and the
        // closing station
        float openingClearance;
        float closingClearance;
    };
    const Arm arms[] = {
        {"bare", false, {}, 10u, crestHalf, crestHalf},
        {"towers", true, {}, 8u, towerHalf + crestHalf, towerHalf + crestHalf},
        {"closing reservation", false, closing, 8u, crestHalf, closingReserve + crestHalf},
    };

    for (const Arm& arm : arms){
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        params.crestPieces = crests;
        if (arm.towers){
            params.postPieces = towerPool;
        }
        params.reservations = arm.reserved;

        const SplineFenceLayoutResult level = buildFenceLayout(flat, params);
        const SplineFenceLayoutResult draped = buildFenceLayout(bumped, params);
        ASSERT_EQ(draped.spans.size(), 1u) << arm.name;
        ASSERT_EQ(draped.stations.size(), 2u) << arm.name;

        // The cell count and every place a flat run of the chord's length gives.
        EXPECT_EQ(level.crests.size(), arm.cells) << arm.name;
        EXPECT_EQ(draped.crests.size(), level.crests.size()) << arm.name;
        ASSERT_FALSE(draped.crests.empty()) << arm.name;
        for (size_t k = 0; k < std::min(draped.crests.size(), level.crests.size()); ++k){
            const SplineFenceCrest& crest = draped.crests[k];
            EXPECT_EQ(crest.ordinalInRun, level.crests[k].ordinalInRun) << arm.name << " " << k;
            EXPECT_NEAR(fenceLength(crest.pose.position - level.crests[k].pose.position), 0.0f, 1.0e-3f) << arm.name << " " << k;
        }

        // No two neighbours overlap, measured along the span they stand on.
        for (size_t k = 1; k < draped.crests.size(); ++k){
            const SplineFenceCrest& a = draped.crests[k - 1u];
            const SplineFenceCrest& b = draped.crests[k];
            EXPECT_GE(fenceDot(b.pose.base - a.pose.base, a.pose.forward), 2.0f * crestHalf - touching)
                << arm.name << ": crest " << k - 1u << " overlaps crest " << k;
        }
        // Nor whatever stands at either end of the bay.
        const SplineFenceCrest& first = draped.crests.front();
        const SplineFenceCrest& last = draped.crests.back();
        EXPECT_GE(fenceDot(first.pose.base - draped.stations.front().pose.base, first.pose.forward), arm.openingClearance - touching) << arm.name;
        EXPECT_GE(fenceDot(draped.stations.back().pose.base - last.pose.base, last.pose.forward), arm.closingClearance - touching) << arm.name;
    }
}

// A stepped span is levelled, so on a grade it stands shorter than the run it covers by the
// climb: 20 m of ground rising 4 m holds 20 one-metre pieces on its steps, and they butt.
TEST(SplineFenceLayoutTests, CrestRowOnSteppedSpansFillsTheirLevelledLength)
{
    const auto center = fenceRamp(20.0f, 4.0f, 41u);
    const float boundaries[2] = {0.0f, 40.0f};
    const SplinePiece spans[1] = {fencePiece(5.0f)};
    const SplinePiece crests[1] = {fencePiece(1.0f)};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.crestPieces = crests;
    params.spanGrade = SplineSpanGrade::Stepped;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    EXPECT_EQ(result.crests.size(), 20u);
    ASSERT_FALSE(result.crests.empty());
    EXPECT_NEAR(-result.crests.back().pose.base.z + 0.5f, 20.0f, 1.0e-3f);
    for (size_t k = 1; k < result.crests.size(); ++k){
        const SplineFenceCrest& a = result.crests[k - 1u];
        const SplineFenceCrest& b = result.crests[k];
        EXPECT_NEAR(fenceDot(b.pose.base - a.pose.base, a.pose.forward), 1.0f, 1.0e-3f)
            << "crest " << k - 1u << " and crest " << k << " must butt";
    }
}

// A crest pool that yields not one cell anywhere, on a run long enough to hold one bare, is
// reported once with the widest stretch a piece was offered and the piece length certain to fit.
TEST(SplineFenceLayoutTests, ACrestRowThatPlacesNothingSaysWhyOnce)
{
    constexpr float pieceLengthPitch = 0.0f; // crest pitch 0: the piece's own length

    // Stations at 0, 5 and 10 m with a 1 m half-footprint leave 3 m clear; 1.50 m always fits.
    const SplineFenceLayoutResult tooLong = fenceCrestRowBetweenTowers(3.5f, pieceLengthPitch, true);
    ASSERT_TRUE(fenceCellOrdinals(tooLong).empty()) << "fixture no longer reproduces an empty row";
    EXPECT_EQ(fenceEmptyCrestRowReports(tooLong), 1u) << "one dead row is one message, however many runs it spans";
    EXPECT_TRUE(fenceAnyValidationContains(tooLong, "the longest clear stretch is 3.00 m."));
    EXPECT_TRUE(fenceAnyValidationContains(tooLong, "A crest piece no longer than half of it, 1.50 m, always fits; use one, or "
                                                    "clear the Post pool."));
    EXPECT_FALSE(fenceAnyValidationContains(tooLong, "plant towers"));

    // A piece shorter than the stretch but longer than half of it still misses every cell.
    for (const float almost : {2.9f, 2.5f}){
        const SplineFenceLayoutResult result = fenceCrestRowBetweenTowers(almost, pieceLengthPitch, true);
        EXPECT_TRUE(fenceCellOrdinals(result).empty()) << almost;
        EXPECT_EQ(fenceEmptyCrestRowReports(result), 1u) << almost;
        EXPECT_TRUE(fenceAnyValidationContains(result, "no longer than half of it, 1.50 m")) << almost;
    }

    // The bound the report names places cells, and the report goes quiet.
    const SplineFenceLayoutResult atTheBound = fenceCrestRowBetweenTowers(1.5f, pieceLengthPitch, true);
    EXPECT_FALSE(fenceCellOrdinals(atTheBound).empty());
    EXPECT_EQ(fenceEmptyCrestRowReports(atTheBound), 0u);

    // The control: no post to reserve the run places cells and says nothing.
    const SplineFenceLayoutResult clear = fenceCrestRowBetweenTowers(3.5f, pieceLengthPitch, false);
    EXPECT_FALSE(fenceCellOrdinals(clear).empty());
    EXPECT_EQ(fenceEmptyCrestRowReports(clear), 0u);

    // A run shorter than its piece places nothing whether or not posts stand on it, silently.
    for (const bool towers : {false, true}){
        const SplineFenceLayoutResult tooBig = fenceCrestRowBetweenTowers(12.0f, pieceLengthPitch, towers);
        EXPECT_TRUE(fenceCellOrdinals(tooBig).empty()) << towers;
        EXPECT_EQ(fenceEmptyCrestRowReports(tooBig), 0u) << towers;
    }
    // A crest pitch longer than the 10 m run.
    const SplineFenceLayoutResult pitchCeiling = fenceCrestRowBetweenTowers(1.0f, 20.0f, true);
    EXPECT_TRUE(fenceCellOrdinals(pitchCeiling).empty());
    EXPECT_EQ(fenceEmptyCrestRowReports(pitchCeiling), 0u);
}

// A typed crest pitch decides the bound once it is more than half the stretch; a pitch the
// stretch cannot hold at all is named as the thing to lower.
TEST(SplineFenceLayoutTests, TheEmptyCrestRowReportNamesAPitchThatBinds)
{
    const SplineFenceLayoutResult slack = fenceCrestRowBetweenTowers(2.0f, 2.6f, true);
    ASSERT_TRUE(fenceCellOrdinals(slack).empty()) << "fixture no longer reproduces an empty row";
    EXPECT_TRUE(fenceAnyValidationContains(slack, "At a Crest Pitch of 2.60 m, a crest piece no longer than 0.40 m (the stretch "
                                                  "less the pitch) always fits"));
    EXPECT_FALSE(fenceCellOrdinals(fenceCrestRowBetweenTowers(0.4f, 2.6f, true)).empty()) << "the bound the report names must place a cell";

    const SplineFenceLayoutResult noRoom = fenceCrestRowBetweenTowers(3.0f, 6.0f, true);
    ASSERT_TRUE(fenceCellOrdinals(noRoom).empty()) << "fixture no longer reproduces an empty row";
    EXPECT_TRUE(fenceAnyValidationContains(noRoom, "Crest Pitch 6.00 m leaves no room for a piece in it; lower the pitch, or clear "
                                                   "the Post pool."));
}

// A pitch typed below the longest piece would overlap the pieces: it is raised and reported once.
TEST(SplineFenceLayoutTests, PitchBelowTheLongestPieceIsClampedAndReportedOnce)
{
    const auto center = fenceStraightLine(20.0f, 41u);
    const float boundaries[3] = {0.0f, 20.0f, 40.0f};
    const SplinePiece spans[1] = {fencePiece(5.0f)};
    const SplinePiece crests[1] = {fencePiece(0.78f)};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.crestPieces = crests;
    params.crestPitch = 0.5f;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    size_t reports = 0;
    for (const std::string& message : result.validation){
        reports += message.find("Crest Pitch") != std::string::npos ? 1u : 0u;
    }
    EXPECT_EQ(reports, 1u);
    EXPECT_TRUE(fenceAnyValidationContains(result, "Crest Pitch 0.50 m is below the longest crest piece, 0.78 m"));
    // Raised to the piece's own length: the first cell stands at half of it.
    ASSERT_FALSE(result.crests.empty());
    EXPECT_NEAR(result.crests.front().pose.position.z, -0.39f, 1.0e-4f);
}

// A pool of mixed lengths pitches on the LONGEST piece, and a shorter pick is centred in its cell.
TEST(SplineFenceLayoutTests, MixedLengthCrestPoolPitchesOnTheLongestPiece)
{
    const FenceCrestFixture fixture;
    const SplinePiece crests[2] = {fencePiece(0.6f), fencePiece(1.0f)};
    SplineFenceLayoutParams params = fixture.params();
    params.crestPieces = crests;
    params.crestPitch = 0.0f;

    const SplineFenceLayoutResult result = buildFenceLayout(fixture.center, params);
    ASSERT_EQ(result.crests.size(), 10u);
    bool sawShort = false;
    bool sawLong = false;
    for (uint32_t k = 0; k < 10u; ++k){
        // pitch 1 m from the longest piece: cell k is [k, k+1), centre k + 0.5
        EXPECT_NEAR(result.crests[k].pose.position.z, -(0.5f + static_cast<float>(k)), 1.0e-4f) << k;
        sawShort = sawShort || result.crests[k].poolSlot == 0u;
        sawLong = sawLong || result.crests[k].poolSlot == 1u;
    }
    EXPECT_TRUE(sawShort) << "fixture must reach the case: the short piece has to be drawn";
    EXPECT_TRUE(sawLong) << "fixture must reach the case: the long piece has to be drawn";
}

// A pitch longer than the run gives no cell, no message and no noise.
TEST(SplineFenceLayoutTests, RunShorterThanAPitchTakesACapOrNothing)
{
    const auto center = fenceStraightLine(1.5f, 7u);
    const float boundaries[2] = {0.0f, 6.0f};
    const SplinePiece spans[1] = {fencePiece(1.5f)};
    const SplinePiece crests[1] = {fencePiece(2.0f)};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.crestPieces = crests;

    const SplineFenceLayoutResult bare = buildFenceLayout(center, params);
    EXPECT_TRUE(bare.crests.empty());
    EXPECT_TRUE(bare.validation.empty());

    // A cap that fits closes the whole stretch, flush against its opening.
    const SplinePiece caps[1] = {fencePiece(1.0f)};
    params.capPieces = caps;
    const SplineFenceLayoutResult capped = buildFenceLayout(center, params);
    ASSERT_EQ(capped.crests.size(), 1u);
    EXPECT_TRUE(capped.crests.front().isCap);
    EXPECT_EQ(capped.crests.front().ordinalInRun, 0u);
    EXPECT_NEAR(capped.crests.front().pose.position.z, -0.5f, 1.0e-4f);
    // And it faces the edge it closes against: a half-turn, travel being -Z.
    EXPECT_NEAR(capped.crests.front().pose.forward.z, 1.0f, 1.0e-5f);
    EXPECT_NEAR(capped.crests.front().pose.right.x, -1.0f, 1.0e-5f);
    EXPECT_NEAR(fenceDot(capped.crests.front().pose.right, capped.crests.front().pose.forward.crossProduct(capped.crests.front().pose.up)),
                1.0f, 1.0e-5f);
    EXPECT_TRUE(capped.validation.empty());

    // A cap longer than the stretch is an offer the run cannot take, and not worth a message.
    const SplinePiece oversized[1] = {fencePiece(1.8f)};
    params.capPieces = oversized;
    const SplineFenceLayoutResult refused = buildFenceLayout(center, params);
    EXPECT_TRUE(refused.crests.empty());
    EXPECT_TRUE(refused.validation.empty());
}

// ---- Crest row: caps -------------------------------------------------------

// Each remainder takes the longest cap that fits it, flush against the cells.
TEST(SplineFenceLayoutTests, CapFillsTheRemainderWithTheLongestPieceThatFits)
{
    const FenceCrestFixture fixture;
    const SplinePiece caps[3] = {fencePiece(0.3f), fencePiece(0.4f), fencePiece(0.6f)};
    SplineFenceLayoutParams params = fixture.params();
    params.capPieces = caps;

    const SplineFenceLayoutResult result = buildFenceLayout(fixture.center, params);
    EXPECT_EQ(fenceCellOrdinals(result), (std::vector<uint32_t>{0u, 1u, 2u, 3u, 4u}));
    const std::vector<SplineFenceCrest> placed = fenceCapsOf(result);
    ASSERT_EQ(placed.size(), 2u);
    // The cells hold [0.5, 9.5], so each remainder is 0.5 m: the 0.4 m cap is the longest fit.
    EXPECT_EQ(placed[0].poolSlot, 1u);
    EXPECT_EQ(placed[1].poolSlot, 1u);
    EXPECT_NEAR(placed[0].pose.position.z, -0.3f, 1.0e-4f);
    EXPECT_NEAR(placed[1].pose.position.z, -9.7f, 1.0e-4f);
    // Emitted in run order, between which the row's own cells stand.
    EXPECT_TRUE(result.crests.front().isCap);
    EXPECT_TRUE(result.crests.back().isCap);
}

// A cap faces the edge it closes: at a run's start a half-turn about the span's up, never a mirror.
TEST(SplineFenceLayoutTests, CapFacesTheEdgeItClosesWithoutMirroring)
{
    const FenceCrestFixture fixture;
    const SplinePiece caps[1] = {fencePiece(0.4f)};
    SplineFenceLayoutParams params = fixture.params();
    params.capPieces = caps;

    const std::vector<SplineFenceCrest> placed = fenceCapsOf(buildFenceLayout(fixture.center, params));
    ASSERT_EQ(placed.size(), 2u);
    const SplineFenceCrest& opening = placed[0];
    const SplineFenceCrest& closing = placed[1];

    EXPECT_NEAR(opening.pose.forward.z, 1.0f, 1.0e-5f);
    EXPECT_NEAR(opening.pose.right.x, -1.0f, 1.0e-5f);
    EXPECT_NEAR(opening.pose.up.y, 1.0f, 1.0e-5f);
    EXPECT_NEAR(closing.pose.forward.z, -1.0f, 1.0e-5f);
    EXPECT_NEAR(closing.pose.right.x, 1.0f, 1.0e-5f);
    for (const SplineFenceCrest& cap : placed){
        EXPECT_NEAR(fenceDot(cap.pose.right, cap.pose.forward.crossProduct(cap.pose.up)), 1.0f, 1.0e-5f)
            << "a cap is turned around, never mirrored";
    }
}

// A cap answers to the cell it stands in.
TEST(SplineFenceLayoutTests, CapInheritsTheOrdinalOfItsCell)
{
    const auto center = fenceStraightLine(9.0f, 19u);
    const float boundaries[2] = {0.0f, 18.0f};
    const SplinePiece spans[1] = {fencePiece(4.5f)};
    const SplinePiece crests[1] = {fencePiece(1.0f)};
    const SplinePiece caps[2] = {fencePiece(0.4f), fencePiece(1.2f)};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.crestPieces = crests;
    params.capPieces = caps;
    params.crestPitch = 2.0f;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    // Cell 4 reaches 9.5 m and is dropped whole; the 1.5 m left over takes the 1.2 m cap, which
    // stands in cell 4. The half-metre inside cell 0 takes the 0.4 m cap, in cell 0.
    EXPECT_EQ(fenceCellOrdinals(result), (std::vector<uint32_t>{0u, 1u, 2u, 3u}));
    const std::vector<SplineFenceCrest> placedCaps = fenceCapsOf(result);
    ASSERT_EQ(placedCaps.size(), 2u);
    EXPECT_EQ(placedCaps[0].poolSlot, 0u);
    EXPECT_EQ(placedCaps[0].ordinalInRun, 0u);
    EXPECT_EQ(placedCaps[0].index, 0u);
    EXPECT_NEAR(placedCaps[0].pose.position.z, -0.3f, 1.0e-4f);
    EXPECT_EQ(placedCaps[1].poolSlot, 1u);
    EXPECT_EQ(placedCaps[1].ordinalInRun, 4u);
    EXPECT_EQ(placedCaps[1].index, 4u);
    EXPECT_NEAR(placedCaps[1].pose.position.z, -8.1f, 1.0e-4f);
}

// ---- Crest row: edits ------------------------------------------------------

// A tower planted at a knot stands on both of the runs that meet there: one signed interval
// about the knot says it.
TEST(SplineFenceLayoutTests, ReservationAboutASharedPointReachesBothRuns)
{
    const auto center = fenceStraightLine(20.0f, 41u);
    const float boundaries[3] = {0.0f, 20.0f, 40.0f};
    const SplinePiece spans[1] = {fencePiece(5.0f)};
    const SplinePiece crests[1] = {fencePiece(1.0f)};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.crestPieces = crests;
    params.crestPitch = 2.0f;

    // A 2 m tower at the shared knot.
    const SplineFenceReservation tower[1] = {{1u, -1.0f, 1.0f}};
    params.reservations = tower;
    const SplineFenceLayoutResult shared = buildFenceLayout(center, params);
    EXPECT_EQ(fenceOrdinalsInRun(shared, 0u), (std::vector<uint32_t>{0u, 1u, 2u, 3u}));
    EXPECT_EQ(fenceOrdinalsInRun(shared, 1u), (std::vector<uint32_t>{1u, 2u, 3u, 4u}));

    // The last knot of an open spline is a closing end only.
    const SplineFenceReservation end[1] = {{2u, -1.0f, 0.0f}};
    params.reservations = end;
    const SplineFenceLayoutResult closing = buildFenceLayout(center, params);
    EXPECT_EQ(fenceOrdinalsInRun(closing, 0u), (std::vector<uint32_t>{0u, 1u, 2u, 3u, 4u}));
    EXPECT_EQ(fenceOrdinalsInRun(closing, 1u), (std::vector<uint32_t>{0u, 1u, 2u, 3u}));

    // On a closed spline the first knot is both ends of the loop.
    const auto square = fenceClosedSquare(8.0f, 0.5f);
    const float loop[5] = {0.0f, 16.0f, 32.0f, 48.0f, 64.0f};
    const SplinePiece loopSpans[1] = {fencePiece(4.0f)};
    SplineFenceLayoutParams closed;
    closed.knotStations = loop;
    closed.closed = true;
    closed.spanPieces = loopSpans;
    closed.crestPieces = crests;
    closed.crestPitch = 2.0f;
    const SplineFenceReservation corner[1] = {{0u, -1.0f, 1.0f}};
    closed.reservations = corner;
    const SplineFenceLayoutResult looped = buildFenceLayout(square, closed);
    EXPECT_EQ(fenceOrdinalsInRun(looped, 0u), (std::vector<uint32_t>{1u, 2u, 3u}));
    EXPECT_EQ(fenceOrdinalsInRun(looped, 3u), (std::vector<uint32_t>{0u, 1u, 2u}));
    EXPECT_EQ(fenceOrdinalsInRun(looped, 1u), (std::vector<uint32_t>{0u, 1u, 2u, 3u}));
}

// Trimming a run's far end changes how many cells it holds and nothing else.
TEST(SplineFenceLayoutTests, FarEndTrimLeavesSurvivorsByteIdentical)
{
    const auto center = fenceStraightLine(20.0f, 41u);
    const float boundaries[3] = {0.0f, 20.0f, 40.0f};
    const auto trimmed = fenceStraightLine(19.0f, 39u);
    const float trimmedBoundaries[3] = {0.0f, 20.0f, 38.0f};
    const SplinePiece spans[1] = {fencePiece(5.0f)};
    const SplinePiece crests[1] = {fencePiece(1.0f)};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.crestPieces = crests;
    params.crestPitch = 2.0f;
    const SplineFenceLayoutResult before = buildFenceLayout(center, params);

    SplineFenceLayoutParams shorter = params;
    shorter.knotStations = trimmedBoundaries;
    const SplineFenceLayoutResult after = buildFenceLayout(trimmed, shorter);

    const std::vector<SplineFenceCrest> firstBefore = fenceCrestsOfRun(before, 0u);
    const std::vector<SplineFenceCrest> firstAfter = fenceCrestsOfRun(after, 0u);
    ASSERT_EQ(firstBefore.size(), firstAfter.size());
    for (size_t i = 0; i < firstBefore.size(); ++i){
        fenceExpectSameCrest(firstAfter[i], firstBefore[i], "run before the trimmed one");
    }

    // The trimmed run loses its last cell and re-phases none of the rest.
    const std::vector<SplineFenceCrest> lastBefore = fenceCrestsOfRun(before, 1u);
    const std::vector<SplineFenceCrest> lastAfter = fenceCrestsOfRun(after, 1u);
    ASSERT_EQ(lastBefore.size(), 5u);
    ASSERT_EQ(lastAfter.size(), 4u);
    for (size_t i = 0; i < lastAfter.size(); ++i){
        EXPECT_EQ(lastAfter[i].index, lastBefore[i].index) << i;
        EXPECT_EQ(lastAfter[i].ordinalInRun, lastBefore[i].ordinalInRun) << i;
        EXPECT_FLOAT_EQ(lastAfter[i].pose.position.z, lastBefore[i].pose.position.z) << i;
    }
}

// Adding or deleting a knot at the end of a spline leaves every other run alone, down to the bit.
TEST(SplineFenceLayoutTests, AddingOrDeletingAnAuthoredPointLeavesOtherRunsByteIdentical)
{
    const auto center = fenceStraightLine(30.0f, 61u);
    const float twoRuns[3] = {0.0f, 20.0f, 40.0f};
    const float threeRuns[4] = {0.0f, 20.0f, 40.0f, 60.0f};
    const SplinePiece spans[1] = {fencePiece(5.0f)};
    const SplinePiece crests[2] = {fencePiece(1.0f), fencePiece(0.8f)};

    SplineFenceLayoutParams params;
    params.knotStations = twoRuns;
    params.spanPieces = spans;
    params.crestPieces = crests;
    params.crestPitch = 2.0f;
    params.seed = 11u;
    const SplineFenceLayoutResult before = buildFenceLayout(center, params);

    SplineFenceLayoutParams extended = params;
    extended.knotStations = threeRuns;
    const SplineFenceLayoutResult after = buildFenceLayout(center, extended);

    for (uint32_t run = 0; run < 2u; ++run){
        const std::vector<SplineFenceCrest> was = fenceCrestsOfRun(before, run);
        const std::vector<SplineFenceCrest> is = fenceCrestsOfRun(after, run);
        ASSERT_EQ(was.size(), is.size()) << run;
        for (size_t i = 0; i < was.size(); ++i){
            fenceExpectSameCrest(is[i], was[i], "run untouched by an added knot");
        }
    }
    EXPECT_EQ(fenceCrestsOfRun(after, 2u).size(), 5u);
}

// Dragging the knot a run OPENS at re-lays that run and leaves the run before it alone.
TEST(SplineFenceLayoutTests, OpeningPointDragRelaysOnlyItsRun)
{
    const auto center = fenceStraightLine(20.0f, 41u);
    const float boundaries[3] = {0.0f, 20.0f, 40.0f};
    const float dragged[3] = {0.0f, 22.0f, 40.0f};
    const SplinePiece spans[1] = {fencePiece(5.0f)};
    const SplinePiece crests[1] = {fencePiece(1.0f)};

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.crestPieces = crests;
    params.crestPitch = 2.0f;
    const SplineFenceLayoutResult before = buildFenceLayout(center, params);

    SplineFenceLayoutParams moved = params;
    moved.knotStations = dragged;
    const SplineFenceLayoutResult after = buildFenceLayout(center, moved);

    // The first run still lays its cells from its own opening station at 0 m.
    const std::vector<SplineFenceCrest> firstBefore = fenceCrestsOfRun(before, 0u);
    const std::vector<SplineFenceCrest> firstAfter = fenceCrestsOfRun(after, 0u);
    ASSERT_EQ(firstBefore.size(), 5u);
    ASSERT_EQ(firstAfter.size(), 5u);
    for (size_t i = 0; i < firstAfter.size(); ++i){
        EXPECT_EQ(firstAfter[i].ordinalInRun, firstBefore[i].ordinalInRun) << i;
        EXPECT_FLOAT_EQ(firstAfter[i].pose.position.z, firstBefore[i].pose.position.z) << i;
    }

    // The second run re-lays from where its opening station now stands: one half-pitch past 11 m.
    const std::vector<SplineFenceCrest> secondAfter = fenceCrestsOfRun(after, 1u);
    ASSERT_FALSE(secondAfter.empty());
    EXPECT_NEAR(secondAfter.front().pose.position.z, -12.0f, 1.0e-4f);
    EXPECT_EQ(secondAfter.front().ordinalInRun, 0u);
}

// ---- Crest row: limits -----------------------------------------------------

// A cell whose footprint closes exactly on a decimal pitch keeps its place.
TEST(SplineFenceLayoutTests, DecimalPitchBoundariesKeepTheirCell)
{
    for (const float pitch : {0.3f, 0.5f, 0.6f, 1.5f, 2.4f}){
        const auto center = fenceStraightLine(12.0f, 49u);
        const float boundaries[2] = {0.0f, 48.0f};
        const SplinePiece spans[1] = {fencePiece(6.0f)};
        const SplinePiece crests[1] = {fencePiece(pitch)};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        params.crestPieces = crests;

        const SplineFenceLayoutResult result = buildFenceLayout(center, params);
        const size_t expected = static_cast<size_t>(std::lround(12.0f / pitch));
        ASSERT_EQ(result.crests.size(), expected) << pitch;
        EXPECT_NEAR(-result.crests.back().pose.position.z + pitch * 0.5f, 12.0f, 1.0e-3f) << pitch;
    }
}

// A cell's global ordinal is its place along the whole spline counted before anything removed it.
TEST(SplineFenceLayoutTests, GlobalIndexCountsEveryCellOfTheRunBefore)
{
    const auto center = fenceStraightLine(20.0f, 41u);
    const float boundaries[3] = {0.0f, 20.0f, 40.0f};
    const SplinePiece spans[1] = {fencePiece(5.0f)};
    const SplinePiece crests[1] = {fencePiece(0.5f)};
    // Nine pieces of structure (four stations, four spans, the closing station) and room for
    // three of the row.
    const SplineFenceReservation gate[1] = {{0u, 0.0f, 9.5f}};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.crestPieces = crests;
    params.reservations = gate;
    params.maxPieces = 12u;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_EQ(result.stations.size() + result.spans.size(), 9u);
    ASSERT_EQ(result.crests.size(), 3u);
    // The gate clears all of the first run but its last half metre: the twentieth cell of twenty.
    EXPECT_EQ(result.crests[0].run, 0u);
    EXPECT_EQ(result.crests[0].ordinalInRun, 19u);
    EXPECT_EQ(result.crests[0].index, 19u);
    EXPECT_NEAR(result.crests[0].pose.position.z, -9.75f, 1.0e-4f);
    // So the second run's first cell is the twenty-first.
    EXPECT_EQ(result.crests[1].run, 1u);
    EXPECT_EQ(result.crests[1].ordinalInRun, 0u);
    EXPECT_EQ(result.crests[1].index, 20u);
    EXPECT_EQ(result.crests[2].index, 21u);
    EXPECT_TRUE(fenceAnyValidationContains(result, "the rest of the crest row is not built"));
}

// The row is emitted after every station and span, so one shared budget cuts decoration first.
TEST(SplineFenceLayoutTests, CrestsAreEmittedAfterSpansUnderOneBudget)
{
    const FenceCrestFixture fixture;
    SplineFenceLayoutParams params = fixture.params();
    params.maxPieces = 5u; // 3 stations and 2 spans, and nothing left over

    const SplineFenceLayoutResult result = buildFenceLayout(fixture.center, params);
    EXPECT_EQ(result.stations.size(), 3u);
    EXPECT_EQ(result.spans.size(), 2u);
    EXPECT_TRUE(result.crests.empty());
    EXPECT_TRUE(fenceAnyValidationContains(result, "the rest of the crest row is not built"));

    // Room for two cells: what fits is built and the cut is still reported.
    params.maxPieces = 7u;
    const SplineFenceLayoutResult partial = buildFenceLayout(fixture.center, params);
    EXPECT_EQ(partial.stations.size(), 3u);
    EXPECT_EQ(partial.spans.size(), 2u);
    EXPECT_EQ(fenceCellOrdinals(partial), (std::vector<uint32_t>{0u, 1u}));
    EXPECT_TRUE(fenceAnyValidationContains(partial, "the rest of the crest row is not built"));

    // A budget that structure itself cannot meet reports the cut once, as the cut it is.
    params.maxPieces = 3u;
    const SplineFenceLayoutResult starved = buildFenceLayout(fixture.center, params);
    EXPECT_EQ(starved.stations.size() + starved.spans.size(), 3u);
    EXPECT_TRUE(starved.crests.empty());
    EXPECT_FALSE(fenceAnyValidationContains(starved, "crest row"));
    EXPECT_TRUE(fenceAnyValidationContains(starved, "the rest of the spline is not built"));
}

// The row stands on the tops of the spans, so asking for one without a span pool is reported.
TEST(SplineFenceLayoutTests, CrestRowWithoutSpansIsReported)
{
    const auto center = fenceStraightLine(10.0f, 21u);
    const float boundaries[2] = {0.0f, 20.0f};
    const SplinePiece crests[1] = {fencePiece(1.0f)};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.crestPieces = crests;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    EXPECT_TRUE(result.crests.empty());
    EXPECT_TRUE(fenceAnyValidationContains(result, "the span pool is empty"));
}

// ---- Crest row: frames -----------------------------------------------------

// The row stands on the span mesh's TOP under either plant mode: on the measured panel 1.1172 m
// above the pivot, not the 1.6388 m the piece is authored tall.
TEST(SplineFenceLayoutTests, CrestSitsOnTheSpanMeshTopUnderEitherPlantMode)
{
    const auto center = fenceStraightLine(10.0f, 21u);
    const float boundaries[2] = {0.0f, 20.0f};
    const SplinePiece spans[1] = {fenceMeasuredKitPanel()};
    const SplinePiece crests[1] = {fencePiece(1.0f)};
    const float topAbovePivot = fenceMeasuredKitPanel().center.y + fenceMeasuredKitPanel().halfExtents.y;
    ASSERT_NEAR(topAbovePivot, 1.1172f, 1.0e-4f) << "fixture must carry a skirt";

    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.crestPieces = crests;
    params.crestPitch = 2.0f;

    // Pivot plants the pivot on the ground and buries the skirt.
    params.plantMode = SplinePlantMode::Pivot;
    const SplineFenceLayoutResult planted = buildFenceLayout(center, params);
    ASSERT_FALSE(planted.crests.empty());
    ASSERT_FALSE(planted.spans.empty());
    EXPECT_NEAR(planted.spans.front().pose.position.y, 0.0f, 1.0e-4f);
    for (const SplineFenceCrest& crest : planted.crests){
        EXPECT_NEAR(crest.pose.position.y, planted.spans.front().pose.position.y + topAbovePivot, 1.0e-3f);
    }

    // BoundsMin lifts the piece until its lowest vertex rests on the ground; the row follows.
    params.plantMode = SplinePlantMode::BoundsMin;
    const SplineFenceLayoutResult lifted = buildFenceLayout(center, params);
    ASSERT_EQ(lifted.crests.size(), planted.crests.size());
    EXPECT_NEAR(lifted.spans.front().pose.position.y, fenceMeasuredKitPanel().skirtDepth(), 1.0e-4f);
    for (const SplineFenceCrest& crest : lifted.crests){
        EXPECT_NEAR(crest.pose.position.y, lifted.spans.front().pose.position.y + topAbovePivot, 1.0e-3f);
    }
    EXPECT_NEAR(lifted.crests.front().pose.position.y, fenceMeasuredKitPanel().height(), 1.0e-3f);
    EXPECT_NEAR(lifted.crests.front().pose.position.y - planted.crests.front().pose.position.y, fenceMeasuredKitPanel().skirtDepth(), 1.0e-3f);
}

// A crest piece on the pitch grid is whole on every grade: its basis stays orthonormal and runs
// along the span it stands on.
TEST(SplineFenceLayoutTests, CrestPiecesStayRigidOnEveryGrade)
{
    for (const SplineSpanGrade grade : {SplineSpanGrade::Racked, SplineSpanGrade::Stepped}){
        const auto center = fenceRamp(20.0f, 6.0f, 41u);
        const float boundaries[2] = {0.0f, 40.0f};
        const SplinePiece spans[1] = {fencePiece(5.0f)};
        const SplinePiece crests[1] = {fencePiece(2.0f)};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        params.crestPieces = crests;
        params.spanGrade = grade;

        const SplineFenceLayoutResult result = buildFenceLayout(center, params);
        ASSERT_FALSE(result.crests.empty());
        ASSERT_FALSE(result.spans.empty());
        // one span piece on a straight ramp: every span shares one frame
        const SplineFenceSpan& span = result.spans.front();
        const Vector3 alongSpan = span.pose.forward * (1.0f / fenceLength(span.pose.forward));
        for (const SplineFenceCrest& crest : result.crests){
            const SplineLayoutPose& pose = crest.pose;
            EXPECT_NEAR(fenceLength(pose.forward), 1.0f, 1.0e-5f);
            EXPECT_NEAR(fenceLength(pose.right), 1.0f, 1.0e-5f);
            EXPECT_NEAR(fenceLength(pose.up), 1.0f, 1.0e-5f);
            EXPECT_NEAR(fenceDot(pose.right, pose.up), 0.0f, 1.0e-5f);
            EXPECT_NEAR(fenceDot(pose.right, pose.forward), 0.0f, 1.0e-5f);
            EXPECT_NEAR(fenceDot(pose.right, pose.forward.crossProduct(pose.up)), 1.0f, 1.0e-5f);
            EXPECT_NEAR(fenceDot(pose.forward, alongSpan), 1.0f, 1.0e-4f);
            EXPECT_NEAR(fenceDot(pose.up, span.pose.up), 1.0f, 1.0e-4f);
        }
    }
}

// ---- Crest row: registered to the spans, and the joins they meet at ---------

namespace{

    // The castle kit's 5 m battlement: the wall's length and depth, 1 m tall, laid along local X
    // and corner-pivoted like the wall it stands on.
    SplinePiece fenceCastleBattlement(){
        SplinePiece b;
        b.halfExtents = Vector3(2.5f, 0.5f, 0.25f);
        b.center = Vector3(-2.5f, 0.5f, 0.0f);
        return b;
    }

    // Where a wall's top stands above its stations' base line under the default Pivot planting.
    constexpr float FENCE_CASTLE_WALL_TOP = 5.0f;

    // The two ends of a piece laid along its pose's forward at `lengthScale`, on the line `lift`
    // above the pose's base.
    void fencePieceEnds(const SplineLayoutPose& pose, float lift, float length, float lengthScale, Vector3& tail, Vector3& head){
        const Vector3 centre = pose.base + pose.up * lift;
        const Vector3 half = pose.forward * (length * lengthScale * 0.5f);
        tail = centre - half;
        head = centre + half;
    }

    // How far a point stands from the nearest wall top of the row, each top taken as the segment
    // between its two ends.
    float fenceDistanceToNearestWallTop(const SplineFenceLayoutResult& result, const Vector3& point){
        float nearest = std::numeric_limits<float>::max();
        for (const SplineFenceSpan& span : result.spans){
            Vector3 tail;
            Vector3 head;
            fencePieceEnds(span.pose, FENCE_CASTLE_WALL_TOP, fenceCastleWall().length(), span.lengthScale, tail, head);
            const Vector3 along = head - tail;
            const float t = std::clamp(fenceDot(point - tail, along) / fenceDot(along, along), 0.0f, 1.0f);
            nearest = std::min(nearest, fenceLength(point - (tail + along * t)));
        }
        return nearest;
    }

    // A piece-local point through the TRS the piece is emitted with.
    Vector3 fenceLocalToWorld(const SplineLayoutPose& pose, const SplinePiece& piece, float lengthScale, const Vector3& local){
        return fenceToWorld(makePiecePose(pose, piece, lengthScale), local);
    }

    Vector3 fenceGroundBisector(const Vector3& a, const Vector3& b){
        const Vector3 groundA = Vector3(a.x, 0.0f, a.z).normalized();
        const Vector3 groundB = Vector3(b.x, 0.0f, b.z).normalized();
        return (groundA + groundB).normalized();
    }

    // Walks one end's cut plane as the cut variant is built from its quantised key (its across
    // extent at the piece's foot and a metre up) into the world, and asserts every point lies on
    // the join plane: vertical through the station, perpendicular to `bisector`.
    void fenceExpectEndOnJoinPlane(const SplineLayoutPose& pose, float lengthScale, const SplinePiece& piece,
                                   const SplineFenceEndPlane& start, const SplineFenceEndPlane& end, bool atStart,
                                   const Vector3& station, const Vector3& bisector, float tolerance, const char* what){
        const SplineMitreShape shape = quantizeMitreShape(lengthScale, start, end);
        const SplineMitrePlane plane = mitreEndCutPlane(piece, shape, atStart);
        const int along = piece.axis() == SplinePieceAxis::X ? 0 : 2;
        const int across = 2 - along;
        const float foot = piece.center.y - piece.halfExtents.y;
        for (const float side : {-1.0f, 0.0f, 1.0f}){
            for (const float height : {0.0f, std::min(piece.height(), 1.0f)}){
                Vector3 local = piece.center;
                local[across] += side * piece.halfThickness();
                local.y = foot + height;
                local[along] = (plane.offset - plane.normal[across] * local[across] - plane.normal[1] * local.y) / plane.normal[along];
                const Vector3 world = fenceLocalToWorld(pose, piece, lengthScale, local);
                EXPECT_NEAR(fenceDot(world - station, bisector), 0.0f, tolerance) << what << ": side " << side << ", height " << height;
            }
        }
    }

    // Two straight legs of `leg` metres meeting at a knot, the second turned `degrees` right
    // (toward +X) from the first, which runs -Z; sampled every `step` metres.
    std::vector<SplineCenterSample> fenceTurnedCorner(float leg, float degrees, float step){
        const float turn = degrees * FENCE_PI / 180.0f;
        const Vector3 corner(0.0f, 0.0f, -leg);
        const Vector3 second(std::sin(turn), 0.0f, -std::cos(turn));
        const uint32_t steps = static_cast<uint32_t>(std::lround(leg / step));
        std::vector<SplineCenterSample> center;
        for (uint32_t i = 0; i <= steps; ++i){
            center.push_back({Vector3(0.0f, 0.0f, -(step * static_cast<float>(i))), Vector3(0, 1, 0), true});
        }
        for (uint32_t i = 1; i <= steps; ++i){
            center.push_back({corner + second * (step * static_cast<float>(i)), Vector3(0, 1, 0), true});
        }
        return center;
    }

    // A post of `width` metres square and `height` tall, centred on its station.
    SplinePiece fenceSquarePost(float width, float height){
        SplinePiece b;
        b.halfExtents = Vector3(width * 0.5f, height * 0.5f, width * 0.5f);
        b.center = Vector3(0.0f, height * 0.5f, 0.0f);
        return b;
    }

}

// A curved castle wall: whole 5 m walls closing each run at 0.97x and 1.02x, and 5 m
// battlements. Registered, each battlement is its wall's twin: the same frame, the same scale,
// and so the same two ends; the row closes wherever the walls do, mitred joins included.
TEST(SplineFenceLayoutTests, CrestRegisteredToItsSpanClosesTheRowAsTheWallsDo)
{
    constexpr float touching = 1.0e-3f;
    const auto curve = fenceCurvedCastleCenter();
    float curveBoundaries[4];
    fenceCurvedCastleBoundaries(curve, curveBoundaries);

    const SplinePiece spans[1] = {fenceCastleWall()};
    const SplinePiece crests[1] = {fenceCastleBattlement()};
    SplineFenceLayoutParams params;
    params.knotStations = curveBoundaries;
    params.spanPieces = spans;
    params.crestPieces = crests;
    params.spanGrade = SplineSpanGrade::Racked;
    const SplineFenceLayoutResult result = buildFenceLayout(curve, params);

    // The fixture has to reach the case: walls stretched off 1.
    bool reaches = false;
    for (const SplineFenceSpan& span : result.spans){
        reaches = reaches || std::abs(span.lengthScale - 1.0f) > 0.01f;
    }
    ASSERT_TRUE(reaches);

    // No battlement end stands off the wall under it.
    for (const SplineFenceCrest& crest : result.crests){
        Vector3 tail;
        Vector3 head;
        fencePieceEnds(crest.pose, 0.0f, fenceCastleBattlement().length(), crest.lengthScale, tail, head);
        EXPECT_LE(fenceDistanceToNearestWallTop(result, tail), touching) << "crest " << crest.index;
        EXPECT_LE(fenceDistanceToNearestWallTop(result, head), touching) << "crest " << crest.index;
    }

    // One battlement per wall, numbered as the wall is.
    ASSERT_EQ(result.crests.size(), result.spans.size());
    size_t mitredJoints = 0;
    for (size_t k = 0; k < result.crests.size(); ++k){
        const SplineFenceCrest& crest = result.crests[k];
        const SplineFenceSpan& span = result.spans[k];
        EXPECT_FALSE(crest.isCap) << k;
        EXPECT_EQ(crest.index, span.index) << k;
        EXPECT_EQ(crest.run, span.run) << k;
        EXPECT_EQ(crest.ordinalInRun, span.ordinalInRun) << k;
        EXPECT_EQ(crest.lengthScale, span.lengthScale) << k;

        Vector3 crestTail;
        Vector3 crestHead;
        fencePieceEnds(crest.pose, 0.0f, fenceCastleBattlement().length(), crest.lengthScale, crestTail, crestHead);
        Vector3 wallTail;
        Vector3 wallHead;
        fencePieceEnds(span.pose, FENCE_CASTLE_WALL_TOP, fenceCastleWall().length(), span.lengthScale, wallTail, wallHead);
        EXPECT_LE(fenceLength(crestTail - wallTail), touching) << k;
        EXPECT_LE(fenceLength(crestHead - wallHead), touching) << k;

        // The joint closes: a square join end to end, a mitred one on the one join plane.
        if (k + 1u < result.crests.size()){
            const SplineFenceCrest& next = result.crests[k + 1u];
            Vector3 nextTail;
            Vector3 nextHead;
            fencePieceEnds(next.pose, 0.0f, fenceCastleBattlement().length(), next.lengthScale, nextTail, nextHead);
            if (!crest.end.mitred){
                EXPECT_LE(fenceLength(nextTail - crestHead), touching) << k;
                continue;
            }
            ASSERT_TRUE(next.start.mitred) << k;
            ++mitredJoints;
            const Vector3 station = result.stations[k + 1u].pose.base;
            const Vector3 bisector = fenceGroundBisector(span.pose.forward, result.spans[k + 1u].pose.forward);
            fenceExpectEndOnJoinPlane(crest.pose, crest.lengthScale, fenceCastleBattlement(), crest.start, crest.end, false, station,
                                      bisector, touching, "closing crest");
            fenceExpectEndOnJoinPlane(next.pose, next.lengthScale, fenceCastleBattlement(), next.start, next.end, true, station,
                                      bisector, touching, "opening crest");
        }
    }
    // The curve's bare knots are mitred joints.
    EXPECT_GT(mitredJoints, 0u);
}

// Posts stand at a registered row's joins exactly where they stand at the walls': a post covers
// a registered crest's join as it covers its wall's, and the row keeps one crest per wall.
TEST(SplineFenceLayoutTests, RegisteredCrestRowStandsBetweenItsPosts)
{
    constexpr float touching = 1.0e-3f;
    const auto curve = fenceCurvedCastleCenter();
    float curveBoundaries[4];
    fenceCurvedCastleBoundaries(curve, curveBoundaries);
    const auto corner = fenceTurnedCorner(20.0f, 45.0f, 0.05f);
    const float cornerLast = static_cast<float>(corner.size() - 1u);
    const float cornerBoundaries[3] = {0.0f, 400.0f, cornerLast};

    struct Arm{
        const char* name;
        const std::vector<SplineCenterSample>* center;
        std::span<const float> boundaries;
        SplinePiece post;
    };
    const Arm arms[] = {
        {"curve, 0.2 m post", &curve, curveBoundaries, fenceSquarePost(0.2f, 6.0f)},
        {"curve, castle tower", &curve, curveBoundaries, fenceSquarePost(2.444f, 8.0f)},
        {"45 degree corner, 0.2 m post", &corner, cornerBoundaries, fenceSquarePost(0.2f, 6.0f)},
    };

    const SplinePiece spans[1] = {fenceCastleWall()};
    const SplinePiece crests[1] = {fenceCastleBattlement()};
    const SplinePiece caps[1] = {fencePiece(0.4f)};
    for (const Arm& arm : arms){
        SplineFenceLayoutParams params;
        params.knotStations = arm.boundaries;
        params.spanPieces = spans;
        params.crestPieces = crests;
        params.capPieces = caps;
        params.postPieces = std::span<const SplinePiece>(&arm.post, 1u);
        const SplineFenceLayoutResult result = buildFenceLayout(*arm.center, params);

        // A registered row leaves no remainder beside a post for a cap.
        EXPECT_TRUE(fenceCapsOf(result).empty()) << arm.name;
        EXPECT_EQ(result.crests.size(), result.spans.size()) << arm.name;
        if (result.crests.size() != result.spans.size()){
            continue;
        }
        for (size_t k = 0; k < result.crests.size(); ++k){
            const SplineFenceCrest& crest = result.crests[k];
            const SplineFenceSpan& span = result.spans[k];
            EXPECT_EQ(crest.ordinalInRun, span.ordinalInRun) << arm.name << " " << k;
            EXPECT_EQ(crest.lengthScale, span.lengthScale) << arm.name << " " << k;
            Vector3 crestTail;
            Vector3 crestHead;
            fencePieceEnds(crest.pose, 0.0f, fenceCastleBattlement().length(), crest.lengthScale, crestTail, crestHead);
            Vector3 wallTail;
            Vector3 wallHead;
            fencePieceEnds(span.pose, FENCE_CASTLE_WALL_TOP, fenceCastleWall().length(), span.lengthScale, wallTail, wallHead);
            EXPECT_LE(fenceLength(crestTail - wallTail), touching) << arm.name << " " << k;
            EXPECT_LE(fenceLength(crestHead - wallHead), touching) << arm.name << " " << k;
        }
    }
}

// Registration is all or nothing per run, and only where the lengths agree with no air asked
// for; a run that fails either keeps the pitch grid for every cell.
TEST(SplineFenceLayoutTests, CrestRowKeepsThePitchGridUnlessEverySpanRegisters)
{
    const SplinePiece crests[1] = {fencePiece(5.0f)};

    // A wall past its stretch cap still registers: 7 m holds one 5 m wall at 1.4x.
    {
        const auto center = fenceStraightLine(7.0f, 15u);
        const float boundaries[2] = {0.0f, 14.0f};
        const SplinePiece spans[1] = {fencePiece(5.0f)};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        params.crestPieces = crests;
        const SplineFenceLayoutResult result = buildFenceLayout(center, params);
        ASSERT_EQ(result.spans.size(), 1u);
        ASSERT_GT(result.spans.front().lengthScale, params.spanMaxStretch);
        ASSERT_EQ(result.crests.size(), 1u);
        EXPECT_EQ(result.crests.front().lengthScale, result.spans.front().lengthScale);
        EXPECT_NEAR(result.crests.front().pose.position.z, -3.5f, 1.0e-4f);
    }

    // A pool of 5 m and 3 m walls: no wall of the run registers, the 5 m ones included.
    {
        const auto center = fenceStraightLine(40.0f, 81u);
        const float boundaries[2] = {0.0f, 80.0f};
        const SplinePiece spans[2] = {fencePiece(5.0f), fencePiece(3.0f)};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        params.crestPieces = crests;
        const SplineFenceLayoutResult result = buildFenceLayout(center, params);
        bool drawsBoth[2] = {false, false};
        for (const SplineFenceSpan& span : result.spans){
            drawsBoth[span.poolSlot] = true;
        }
        ASSERT_TRUE(drawsBoth[0] && drawsBoth[1]) << "the run must mix both walls";
        ASSERT_FALSE(result.crests.empty());
        for (const SplineFenceCrest& crest : result.crests){
            EXPECT_EQ(crest.lengthScale, 1.0f) << crest.index;
            EXPECT_NEAR(crest.pose.position.z, -(2.5f + 5.0f * static_cast<float>(crest.ordinalInRun)), 1.0e-3f) << crest.index;
        }
    }

    // Air between the pieces: a typed 6 m pitch over 5 m walls asks for the pitch.
    {
        const auto center = fenceStraightLine(20.0f, 41u);
        const float boundaries[2] = {0.0f, 40.0f};
        const SplinePiece spans[1] = {fencePiece(5.0f)};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        params.crestPieces = crests;
        params.crestPitch = 6.0f;
        const SplineFenceLayoutResult result = buildFenceLayout(center, params);
        ASSERT_EQ(result.crests.size(), 3u);
        for (const SplineFenceCrest& crest : result.crests){
            EXPECT_EQ(crest.lengthScale, 1.0f) << crest.index;
            EXPECT_NEAR(crest.pose.position.z, -(3.0f + 6.0f * static_cast<float>(crest.ordinalInRun)), 1.0e-3f) << crest.index;
        }
    }
}

// A registered row answers to reservations and caps exactly as the pitch grid does.
TEST(SplineFenceLayoutTests, ReservedWallTakesNoRegisteredCrestAndCapsCloseBesideIt)
{
    const auto center = fenceStraightLine(20.0f, 41u);
    const float boundaries[2] = {0.0f, 40.0f};
    const SplinePiece spans[1] = {fencePiece(5.0f)};
    const SplinePiece crests[1] = {fencePiece(5.0f)};
    const SplinePiece caps[1] = {fencePiece(1.0f)};
    const SplineFenceReservation reserved[1] = {{0u, 6.0f, 7.0f}};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.crestPieces = crests;
    params.capPieces = caps;
    params.reservations = reserved;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    EXPECT_EQ(fenceCellOrdinals(result), (std::vector<uint32_t>{0u, 2u, 3u}));
    const std::vector<SplineFenceCrest> capped = fenceCapsOf(result);
    ASSERT_EQ(capped.size(), 2u);
    // The 1 m between the first wall and the reservation, and the 3 m between the reservation and
    // the third wall: both in wall 1's cell. Travel is -Z.
    EXPECT_NEAR(capped[0].pose.position.z, -5.5f, 1.0e-4f);
    EXPECT_LT(capped[0].pose.forward.z, 0.0f);
    EXPECT_NEAR(capped[1].pose.position.z, -9.5f, 1.0e-4f);
    EXPECT_GT(capped[1].pose.forward.z, 0.0f);
    for (const SplineFenceCrest& cap : capped){
        EXPECT_EQ(cap.ordinalInRun, 1u);
        EXPECT_EQ(cap.lengthScale, 1.0f);
    }

    // On compressed walls the span cells and the pitch grid part ways: 18 m holds four 5 m walls
    // at 0.9x, cells of 4.5 m; the cap closing below a reservation at 9.5-10.5 m stands at 9.2 m,
    // in wall 2 (9-13.5 m).
    const auto compressed = fenceStraightLine(18.0f, 37u);
    const float compressedBoundaries[2] = {0.0f, 36.0f};
    const SplinePiece shortCaps[1] = {fencePiece(0.4f)};
    const SplineFenceReservation middle[1] = {{0u, 9.5f, 10.5f}};
    params.knotStations = compressedBoundaries;
    params.capPieces = shortCaps;
    params.reservations = middle;
    const SplineFenceLayoutResult squeezed = buildFenceLayout(compressed, params);
    ASSERT_EQ(squeezed.spans.size(), 4u);
    ASSERT_NEAR(squeezed.spans.front().lengthScale, 0.9f, 1.0e-4f);
    EXPECT_EQ(fenceCellOrdinals(squeezed), (std::vector<uint32_t>{0u, 1u, 3u}));
    const std::vector<SplineFenceCrest> squeezedCaps = fenceCapsOf(squeezed);
    ASSERT_EQ(squeezedCaps.size(), 2u);
    EXPECT_NEAR(squeezedCaps[0].pose.position.z, -9.2f, 1.0e-4f);
    EXPECT_EQ(squeezedCaps[0].ordinalInRun, 2u);
    EXPECT_NEAR(squeezedCaps[1].pose.position.z, -13.3f, 1.0e-4f);
    EXPECT_EQ(squeezedCaps[1].ordinalInRun, 2u);
}

// A registered row stands on its walls and the posts reserve nothing against it, so only a
// caller's reservation can empty it, and the report names that and nothing else.
TEST(SplineFenceLayoutTests, AnEmptyRegisteredCrestRowNamesOnlyTheReservation)
{
    const auto center = fenceStraightLine(20.0f, 41u);
    const float boundaries[2] = {0.0f, 40.0f};
    const SplinePiece spans[1] = {fencePiece(5.0f)};
    const SplinePiece crests[1] = {fencePiece(5.0f)};
    const SplinePiece posts[1] = {fenceSquarePost(0.2f, 6.0f)};
    const SplineFenceReservation everyWall[4] = {{0u, 2.0f, 3.0f}, {0u, 7.0f, 8.0f}, {0u, 12.0f, 13.0f}, {0u, 17.0f, 18.0f}};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.crestPieces = crests;

    // The control: nothing reserved, one crest per wall and no report.
    params.postPieces = posts;
    const SplineFenceLayoutResult clear = buildFenceLayout(center, params);
    EXPECT_EQ(fenceCellOrdinals(clear), (std::vector<uint32_t>{0u, 1u, 2u, 3u}));
    EXPECT_EQ(fenceEmptyCrestRowReports(clear), 0u);

    params.reservations = everyWall;
    for (const bool withPosts : {false, true}){
        params.postPieces = withPosts ? std::span<const SplinePiece>(posts) : std::span<const SplinePiece>();
        const SplineFenceLayoutResult result = buildFenceLayout(center, params);
        ASSERT_TRUE(fenceCellOrdinals(result).empty()) << withPosts;
        EXPECT_EQ(fenceEmptyCrestRowReports(result), 1u) << withPosts;
        EXPECT_TRUE(fenceAnyValidationContains(result, "a reserved stretch reaches every wall they stand on; move what reserves "
                                                       "the run."))
            << withPosts;
        EXPECT_FALSE(fenceAnyValidationContains(result, "always fits")) << withPosts;
        EXPECT_FALSE(fenceAnyValidationContains(result, "clear the Post pool")) << withPosts;
    }
}

// A node several splines meet at faces one way in every one of their layouts: the caller says
// which, and the fence's own bisector yields to it.
TEST(SplineFenceLayoutTests, SharedFacingAtAnAuthoredPointOverridesTheBisector)
{
    const auto center = fenceCorneredL(6.0f, 6.0f, 0.5f);
    const float boundaries[3] = {0.0f, 12.0f, 24.0f};
    const SplinePiece spans[1] = {fencePiece(2.0f)};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;

    const SplineFenceLayoutResult bisected = buildFenceLayout(center, params);
    ASSERT_GT(bisected.stations.size(), 3u);
    const SplineFenceStation& corner = bisected.stations[3];
    ASSERT_TRUE(corner.isKnot);
    // The corner turns from -Z to +X: its own answer is the 45 degree bisector.
    EXPECT_NEAR(corner.pose.forward.x, 0.70710678f, 1.0e-4f);
    EXPECT_NEAR(corner.pose.forward.z, -0.70710678f, 1.0e-4f);

    const Vector3 shared[3] = {Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, -1.0f), Vector3(0.0f, 0.0f, 0.0f)};
    params.knotForwards = shared;
    const SplineFenceLayoutResult forced = buildFenceLayout(center, params);
    EXPECT_NEAR(forced.stations[3].pose.forward.x, 0.0f, 1.0e-4f);
    EXPECT_NEAR(forced.stations[3].pose.forward.z, -1.0f, 1.0e-4f);
    // A zero entry keeps the fence's own answer, which for an end station is its run's chord.
    EXPECT_NEAR(forced.stations.front().pose.forward.z, -1.0f, 1.0e-4f);
}

// ---- Span joins: the mitre at bare joins -----------------------------------

namespace{

    // Every span of a result ends square.
    bool fenceNoSpanIsMitred(const SplineFenceLayoutResult& result){
        return std::none_of(result.spans.begin(), result.spans.end(), [](const SplineFenceSpan& span){ return span.start.mitred || span.end.mitred; });
    }

    size_t fenceMitredJoinCount(const SplineFenceLayoutResult& result){
        return static_cast<size_t>(std::count_if(result.spans.begin(), result.spans.end(), [](const SplineFenceSpan& span){ return span.end.mitred; }));
    }

    // The castle wall laid on a level arc with no posts, one run of whole walls.
    SplineFenceLayoutResult fenceBareCastleArc(float radius, float degrees, std::span<const SplinePiece> crests = {}){
        const auto center = fenceLevelArc(radius, degrees, 0.05f);
        const float boundaries[2] = {0.0f, static_cast<float>(center.size() - 1u)};
        const SplinePiece spans[1] = {fenceCastleWall()};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        params.crestPieces = crests;
        return buildFenceLayout(center, params);
    }

    // Two 5 m legs meeting at bare knot 1, the second turned `degrees` to the right, one span of
    // `piece` on each.
    SplineFenceLayoutResult fenceBareTurnedCorner(float degrees, const SplinePiece& piece){
        const auto center = fenceTurnedCorner(5.0f, degrees, 0.05f);
        const float last = static_cast<float>(center.size() - 1u);
        const float boundaries[3] = {0.0f, last * 0.5f, last};
        const SplinePiece spans[1] = {piece};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        return buildFenceLayout(center, params);
    }

}

// A post covers its join: the same corner mitres with the post pool empty and stays square with
// it filled.
TEST(SplineFenceLayoutTests, CoveredJoinCarriesNoMitre)
{
    const auto center = fenceCorneredL(6.0f, 6.0f, 0.5f);
    const float boundaries[3] = {0.0f, 12.0f, 24.0f};
    const SplinePiece spans[1] = {fencePiece(2.0f)};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    const SplineFenceLayoutResult bare = buildFenceLayout(center, params);
    EXPECT_EQ(fenceMitredJoinCount(bare), 1u) << "the control: the bare corner mitres";

    const SplinePiece posts[1] = {fenceMeasuredKitPost()};
    params.postPieces = posts;
    const SplineFenceLayoutResult posted = buildFenceLayout(center, params);
    EXPECT_TRUE(fenceNoSpanIsMitred(posted));
}

// On a curve no post covers, each interior join is cut on one vertical plane through the station,
// perpendicular to the bisector of the two chords; each span is laid longer by (t/2) tan(|turn|/2)
// at every mitred end; and the outer faces of the two spans meet on the plane.
TEST(SplineFenceLayoutTests, UncoveredArcJoinMeetsOnTheBisector)
{
    const SplineFenceLayoutResult result = fenceBareCastleArc(30.0f, 60.0f);
    const SplinePiece wall = fenceCastleWall();
    const float half = wall.halfThickness();
    ASSERT_GE(result.spans.size(), 4u);
    ASSERT_EQ(result.stations.size(), result.spans.size() + 1u);
    EXPECT_FALSE(result.spans.front().start.mitred);
    EXPECT_FALSE(result.spans.back().end.mitred);

    for (size_t k = 0; k + 1u < result.spans.size(); ++k){
        const SplineFenceSpan& before = result.spans[k];
        const SplineFenceSpan& after = result.spans[k + 1u];
        ASSERT_TRUE(before.end.mitred) << k;
        ASSERT_TRUE(after.start.mitred) << k;
        const Vector3 station = result.stations[k + 1u].pose.base;
        const Vector3 bisector = fenceGroundBisector(before.pose.forward, after.pose.forward);
        fenceExpectEndOnJoinPlane(before.pose, before.lengthScale, wall, before.start, before.end, false, station, bisector, 5e-4f, "closing end");
        fenceExpectEndOnJoinPlane(after.pose, after.lengthScale, wall, after.start, after.end, true, station, bisector, 5e-4f, "opening end");
    }

    for (size_t k = 0; k < result.spans.size(); ++k){
        const SplineFenceSpan& span = result.spans[k];
        const float chord = fenceLength(result.stations[k + 1u].pose.base - result.stations[k].pose.base);
        float overhangs = 0.0f;
        for (const SplineFenceEndPlane* end : {&span.start, &span.end}){
            if (end->mitred){
                overhangs += half * std::tan(0.5f * std::abs(end->turnRadians));
            }
        }
        EXPECT_NEAR(span.lengthScale * wall.length(), chord + overhangs, 1e-4f) << "span " << k;
    }

    // The arc turns right, so each join's outside is its left: the outer bottom corners of the
    // two ends coincide on the plane.
    for (size_t k = 0; k + 1u < result.spans.size(); ++k){
        const SplineFenceSpan& before = result.spans[k];
        const SplineFenceSpan& after = result.spans[k + 1u];
        ASSERT_GT(before.end.turnRadians, 0.0f);
        // X-laid wall: local +Z is the right of travel, so the left face is at local -Z.
        const Vector3 closingCorner(wall.center.x + wall.halfExtents.x, 0.0f, -half);
        const Vector3 openingCorner(wall.center.x - wall.halfExtents.x, 0.0f, -half);
        const Vector3 a = fenceLocalToWorld(before.pose, wall, before.lengthScale, closingCorner);
        const Vector3 b = fenceLocalToWorld(after.pose, wall, after.lengthScale, openingCorner);
        EXPECT_LT(fenceLength(a - b), 5e-4f) << "join " << k << " still shows a notch";
    }
}

// Which way a join turns decides which face is outside: the planes' turns take the sign of the
// fence's own turn, positive to the right, on both ends.
TEST(SplineFenceLayoutTests, MitreSignFollowsTheTurn)
{
    const SplineFenceLayoutResult right = fenceBareCastleArc(30.0f, 60.0f);
    auto left = fenceLevelArc(30.0f, 60.0f, 0.05f);
    for (SplineCenterSample& sample : left){
        sample.position.x = -sample.position.x;
    }
    const float boundaries[2] = {0.0f, static_cast<float>(left.size() - 1u)};
    const SplinePiece spans[1] = {fenceCastleWall()};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    const SplineFenceLayoutResult mirrored = buildFenceLayout(left, params);
    ASSERT_EQ(right.spans.size(), mirrored.spans.size());
    for (size_t k = 0; k + 1u < right.spans.size(); ++k){
        EXPECT_GT(right.spans[k].end.turnRadians, 0.0f);
        EXPECT_GT(right.spans[k + 1u].start.turnRadians, 0.0f);
        EXPECT_LT(mirrored.spans[k].end.turnRadians, 0.0f);
        EXPECT_LT(mirrored.spans[k + 1u].start.turnRadians, 0.0f);
        EXPECT_NEAR(right.spans[k].end.turnRadians, -mirrored.spans[k].end.turnRadians, 1e-5f);
    }
}

// A bare right angle: both spans are cut on the 45 degree plane through the corner, each laid
// half its thickness longer (tan 45 = 1).
TEST(SplineFenceLayoutTests, ABareRightAngleIsCutOnItsBisector)
{
    const auto center = fenceCorneredL(10.0f, 10.0f, 0.05f);
    const float boundaries[3] = {0.0f, 200.0f, 400.0f};
    const SplinePiece spans[1] = {fenceCastleWall()};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_EQ(result.spans.size(), 4u);
    EXPECT_EQ(fenceMitredJoinCount(result), 1u) << "the straight joins stay square";
    const SplineFenceSpan& before = result.spans[1];
    const SplineFenceSpan& after = result.spans[2];
    ASSERT_TRUE(before.end.mitred);
    ASSERT_TRUE(after.start.mitred);
    EXPECT_NEAR(before.end.turnRadians, FENCE_PI * 0.5f, 1e-4f);
    EXPECT_NEAR(before.lengthScale * 5.0f, 5.0f + 0.25f, 1e-4f);
    EXPECT_NEAR(after.lengthScale * 5.0f, 5.0f + 0.25f, 1e-4f);
    const Vector3 bisector = Vector3(1.0f, 0.0f, -1.0f).normalized();
    fenceExpectEndOnJoinPlane(before.pose, before.lengthScale, fenceCastleWall(), before.start, before.end, false, Vector3(0.0f, 0.0f, -10.0f),
                              bisector, 5e-4f, "closing end");
    fenceExpectEndOnJoinPlane(after.pose, after.lengthScale, fenceCastleWall(), after.start, after.end, true, Vector3(0.0f, 0.0f, -10.0f),
                              bisector, 5e-4f, "opening end");
}

// Past the mitre limit a join keeps square ends and says where to plant a post.
TEST(SplineFenceLayoutTests, ATurnPastTheMitreLimitStaysSquareAndIsReported)
{
    const SplineFenceLayoutResult sharp = fenceBareTurnedCorner(150.0f, fenceCastleWall());
    ASSERT_EQ(sharp.spans.size(), 2u);
    EXPECT_TRUE(fenceNoSpanIsMitred(sharp));
    EXPECT_NEAR(sharp.spans[0].lengthScale, 1.0f, 1e-5f) << "a square end is not laid longer";
    size_t reports = 0;
    for (const std::string& message : sharp.validation){
        if (message.find("mitre limit") == std::string::npos){
            continue;
        }
        ++reports;
        EXPECT_NE(message.find("knot 1 turns 150 degrees"), std::string::npos) << message;
        EXPECT_NE(message.find("plant a post"), std::string::npos) << message;
    }
    EXPECT_EQ(reports, 1u);

    const SplineFenceLayoutResult within = fenceBareTurnedCorner(SPLINE_MITRE_LIMIT_DEGREES - 1.0f, fenceCastleWall());
    EXPECT_EQ(fenceMitredJoinCount(within), 1u);
    EXPECT_FALSE(fenceAnyValidationContains(within, "mitre limit"));
    const SplineFenceLayoutResult past = fenceBareTurnedCorner(SPLINE_MITRE_LIMIT_DEGREES + 1.0f, fenceCastleWall());
    EXPECT_TRUE(fenceNoSpanIsMitred(past));
    EXPECT_TRUE(fenceAnyValidationContains(past, "mitre limit"));
}

// The notch floor: a turn whose square ends would leave under a millimetre between the outer
// faces is chord noise and stays square; one just over it is mitred.
TEST(SplineFenceLayoutTests, ANotchUnderAMillimetreStaysSquare)
{
    // fencePiece(): 0.1 m thick, so 1 mm of notch is a turn of 2 * atan(0.01) = 1.146 degrees.
    EXPECT_TRUE(fenceNoSpanIsMitred(fenceBareTurnedCorner(1.0f, fencePiece(5.0f))));
    EXPECT_EQ(fenceMitredJoinCount(fenceBareTurnedCorner(1.3f, fencePiece(5.0f))), 1u);
}

// A closed square with no posts mitres all four corners, the loop's shared station included.
TEST(SplineFenceLayoutTests, TheClosedSquareMitresItsFourCorners)
{
    const SplineFenceLayoutResult result = fenceFixtureClosedSquare(FENCE_ARM_NOTHING);
    ASSERT_EQ(result.spans.size(), 8u);
    EXPECT_EQ(fenceMitredJoinCount(result), 4u);
    for (const SplineFenceSpan& span : result.spans){
        EXPECT_EQ(span.start.mitred, span.ordinalInRun == 0u);
        EXPECT_EQ(span.end.mitred, span.ordinalInRun == 1u);
        if (span.end.mitred){
            EXPECT_NEAR(std::abs(span.end.turnRadians), FENCE_PI * 0.5f, 1e-4f);
        }
    }
}

// A closed square stopped by the piece budget after its first two sides: the walls mitred
// against walls that are never emitted end square, while the corner the emitted sides share stays
// mitred.
TEST(SplineFenceLayoutTests, AWallThePieceBudgetLeavesWithoutItsNeighbourEndsSquare)
{
    const auto center = fenceClosedSquare(8.0f, 0.5f);
    const float boundaries[5] = {0.0f, 16.0f, 32.0f, 48.0f, 64.0f};
    const SplinePiece spans[1] = {fencePiece(4.0f)};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.closed = true;
    params.maxPieces = 8u;
    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_EQ(result.spans.size(), 4u);
    EXPECT_FALSE(result.spans.front().start.mitred);
    EXPECT_FALSE(result.spans.back().end.mitred);
    EXPECT_NEAR(result.spans.front().lengthScale, 1.0f, 1e-4f);
    EXPECT_NEAR(result.spans.back().lengthScale, 1.0f, 1e-4f);
    EXPECT_TRUE(result.spans[1].end.mitred);
    EXPECT_TRUE(result.spans[2].start.mitred);
}

// A crest registered to its span takes the span's two planes in its own frame; a crest on the
// pitch grid never reaches a join and stays whole.
TEST(SplineFenceLayoutTests, ARegisteredCrestCarriesItsSpansPlanes)
{
    const SplinePiece battlements[1] = {fenceCastleBattlement()};
    const SplineFenceLayoutResult result = fenceBareCastleArc(30.0f, 60.0f, battlements);
    ASSERT_EQ(result.crests.size(), result.spans.size());
    for (size_t k = 0; k < result.crests.size(); ++k){
        const SplineFenceCrest& crest = result.crests[k];
        const SplineFenceSpan& span = result.spans[k];
        EXPECT_EQ(crest.start.mitred, span.start.mitred);
        EXPECT_EQ(crest.end.mitred, span.end.mitred);
        EXPECT_EQ(crest.lengthScale, span.lengthScale);
        if (!crest.end.mitred){
            continue;
        }
        EXPECT_NEAR(crest.end.stationLocalY, -FENCE_CASTLE_WALL_TOP, 1e-5f);
        const Vector3 station = result.stations[k + 1u].pose.base;
        const Vector3 bisector = fenceGroundBisector(span.pose.forward, result.spans[k + 1u].pose.forward);
        fenceExpectEndOnJoinPlane(crest.pose, crest.lengthScale, fenceCastleBattlement(), crest.start, crest.end, false, station, bisector, 5e-4f,
                                  "crest closing end");
        fenceExpectEndOnJoinPlane(result.crests[k + 1u].pose, result.crests[k + 1u].lengthScale, fenceCastleBattlement(), result.crests[k + 1u].start,
                                  result.crests[k + 1u].end, true, station, bisector, 5e-4f, "crest opening end");
    }

    // A 1 m battlement is shorter than the wall, so the row keeps its pitch grid: whole pieces.
    const SplinePiece shortBattlements[1] = {fencePiece(1.0f)};
    const SplineFenceLayoutResult grid = fenceBareCastleArc(30.0f, 60.0f, shortBattlements);
    ASSERT_FALSE(grid.crests.empty());
    for (const SplineFenceCrest& crest : grid.crests){
        EXPECT_FALSE(crest.start.mitred || crest.end.mitred);
        EXPECT_EQ(crest.lengthScale, 1.0f);
    }
}

// The fixtures that carry a post at every join, or run straight, keep square ends everywhere;
// the bare closed square and the bare curved castle are the two that mitre.
TEST(SplineFenceLayoutTests, FixturesWithPostsOrStraightRunsKeepSquareEnds)
{
    for (const FenceIdentityFixture& fixture : FENCE_IDENTITY_FIXTURES){
        const std::string name = fixture.name;
        const bool bareTurns = name == "closed square" || name == "curved castle wall, crest shorter than the wall";
        for (const uint32_t arms : {FENCE_ARM_NOTHING, FENCE_ARM_CREST_ROW}){
            const SplineFenceLayoutResult result = fixture.build(arms);
            EXPECT_EQ(fenceNoSpanIsMitred(result), !bareTurns) << fixture.name;
            for (const SplineFenceCrest& crest : result.crests){
                EXPECT_FALSE(crest.start.mitred || crest.end.mitred) << fixture.name;
            }
        }
    }

    const auto center = fenceStraightLine(20.0f, 41u);
    const float boundaries[3] = {0.0f, 20.0f, 40.0f};
    const SplinePiece spans[1] = {fenceCastleWall()};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    const SplineFenceLayoutResult straight = buildFenceLayout(center, params);
    ASSERT_EQ(straight.spans.size(), 4u);
    EXPECT_TRUE(fenceNoSpanIsMitred(straight));
    for (const SplineFenceSpan& span : straight.spans){
        EXPECT_NEAR(span.lengthScale, 1.0f, 1e-5f);
    }
}

// ---- Span overrides: gates and pinned pieces --------------------------------

namespace{

    // The castle kit's gate: a 7.35 m piece among 5 m walls, planted and pivoted as the walls are.
    SplinePiece fenceCastleGate(){
        SplinePiece b;
        b.halfExtents = Vector3(3.675f, 3.0f, 0.4f);
        b.center = Vector3(-3.675f, 3.0f, 0.0f);
        return b;
    }

    // A straight run of `length` metres of 5 m castle walls down -Z, with the given overrides and
    // gate pool, and an optional crest row.
    SplineFenceLayoutResult fenceStraightCastleRun(float length, std::span<const SplineSpanOverride> overrides, std::span<const SplinePiece> gates,
                                                   std::span<const SplinePiece> crests = {}, float crestPitch = 0.0f,
                                                   std::span<const SplineFenceReservation> reserved = {}){
        const uint32_t samples = static_cast<uint32_t>(std::lround(length / 0.5f)) + 1u;
        const auto center = fenceStraightLine(length, samples);
        const float boundaries[2] = {0.0f, static_cast<float>(samples - 1u)};
        const SplinePiece spans[1] = {fenceCastleWall()};
        SplineFenceLayoutParams params;
        params.knotStations = boundaries;
        params.spanPieces = spans;
        params.gatePieces = gates;
        params.spanOverrides = overrides;
        params.crestPieces = crests;
        params.crestPitch = crestPitch;
        params.reservations = reserved;
        return buildFenceLayout(center, params);
    }

    const SplineFenceSpan* fenceSpanAtOrdinal(const SplineFenceLayoutResult& result, uint32_t run, uint32_t ordinal){
        for (const SplineFenceSpan& span : result.spans){
            if (span.run == run && span.ordinalInRun == ordinal){
                return &span;
            }
        }
        return nullptr;
    }

    // A crest's footprint as distances along a -Z straight run: its base centred, its length
    // scaled.
    void fenceCrestFootprint(const SplineFenceCrest& crest, const SplinePiece& piece, float& outLo, float& outHi){
        const float half = piece.length() * crest.lengthScale * 0.5f;
        outLo = -crest.pose.base.z - half;
        outHi = -crest.pose.base.z + half;
    }

}

// With no override set, a fence that carries the whole override table and a gate pool lays out
// exactly as one that carries neither, bit for bit.
TEST(SplineFenceLayoutTests, UntouchedOverrideTableLeavesEveryExistingFixtureByteIdentical)
{
    for (const FenceIdentityFixture& fixture : FENCE_IDENTITY_FIXTURES){
        for (const uint32_t arms : {FENCE_ARM_NOTHING, FENCE_ARM_CREST_ROW}){
            const SplineFenceLayoutResult plain = fixture.build(arms);
            const SplineFenceLayoutResult armed = fixture.build(arms | FENCE_ARM_EMPTY_OVERRIDE_TABLE);
            EXPECT_EQ(fenceFingerprintStationsAndSpans(plain), fenceFingerprintStationsAndSpans(armed)) << fixture.name;
            ASSERT_EQ(plain.crests.size(), armed.crests.size()) << fixture.name;
            for (size_t k = 0; k < plain.crests.size(); ++k){
                fenceExpectSameCrest(armed.crests[k], plain.crests[k], fixture.name);
                EXPECT_EQ(armed.crests[k].lengthScale, plain.crests[k].lengthScale) << fixture.name;
            }
            EXPECT_EQ(armed.validation, plain.validation) << fixture.name;
            for (const SplineFenceSpan& span : armed.spans){
                EXPECT_FALSE(span.isGate) << fixture.name;
            }
        }
    }
}

// A gate draws its gate piece at its own length; the run's one shared stretch closes around it.
TEST(SplineFenceLayoutTests, GateDrawsItsPieceAtItsOwnLengthAndTheRunClosesAroundIt)
{
    const SplinePiece gates[1] = {fenceCastleGate()};
    const SplineSpanOverride overrides[1] = {{0u, 1u, SplineSpanOverrideKind::Gate, 0u}};
    const SplineFenceLayoutResult result = fenceStraightCastleRun(20.0f, overrides, gates);

    // 5 + 7.35 + 5 + 5 = 22.35 m of pieces close 20 m at 0.895.
    ASSERT_EQ(result.spans.size(), 4u);
    const float stretch = 20.0f / 22.35f;
    for (const SplineFenceSpan& span : result.spans){
        EXPECT_EQ(span.isGate, span.ordinalInRun == 1u) << span.ordinalInRun;
        EXPECT_NEAR(span.lengthScale, stretch, 1e-4f) << span.ordinalInRun;
    }
    EXPECT_EQ(result.spans[1].poolSlot, 0u);
    const float gateStart = -result.stations[1].pose.base.z;
    const float gateEnd = -result.stations[2].pose.base.z;
    EXPECT_NEAR(gateEnd - gateStart, 7.35f * stretch, 1e-3f);
    EXPECT_NEAR(result.stations.back().pose.base.z, -20.0f, 1e-4f);
    EXPECT_TRUE(result.validation.empty());
}

// A gate over an empty gate pool is an opening, and the ordinals after it do not move.
TEST(SplineFenceLayoutTests, GateOverAnEmptyGatePoolLeavesAnOpeningThatKeepsItsOrdinal)
{
    const SplineSpanOverride overrides[1] = {{0u, 1u, SplineSpanOverrideKind::Gate, 0u}};
    const SplineFenceLayoutResult open = fenceStraightCastleRun(20.0f, overrides, {});
    const SplineFenceLayoutResult walled = fenceStraightCastleRun(20.0f, {}, {});

    ASSERT_EQ(walled.spans.size(), 4u);
    ASSERT_EQ(open.spans.size(), 3u);
    EXPECT_EQ(fenceSpanAtOrdinal(open, 0u, 1u), nullptr);
    for (const SplineFenceSpan& span : open.spans){
        EXPECT_FALSE(span.isGate);
        EXPECT_EQ(span.index, span.ordinalInRun) << "an opening renumbered the spans after it";
        const SplineFenceSpan* same = fenceSpanAtOrdinal(walled, 0u, span.ordinalInRun);
        ASSERT_NE(same, nullptr);
        fenceExpectSameVector(span.pose.position, same->pose.position, "wall beside an opening");
    }
    EXPECT_EQ(open.stations.size(), walled.stations.size());
    EXPECT_TRUE(open.validation.empty()) << "an empty gate pool is an opening, not a mistake";
}

// A gate's footprint is its span, and it reserves the crest row the way a caller's reservation
// over the same stretch does.
TEST(SplineFenceLayoutTests, GateFootprintReservesTheCrestRow)
{
    const SplinePiece gates[1] = {fenceCastleGate()};
    const SplinePiece crests[1] = {fencePiece(1.0f)};
    const SplineSpanOverride overrides[1] = {{0u, 1u, SplineSpanOverrideKind::Gate, 0u}};
    const SplineFenceLayoutResult gated = fenceStraightCastleRun(20.0f, overrides, gates, crests, 2.0f);
    const float gateStart = -gated.stations[1].pose.base.z;
    const float gateEnd = -gated.stations[2].pose.base.z;

    ASSERT_FALSE(gated.crests.empty());
    for (const SplineFenceCrest& crest : gated.crests){
        float lo = 0.0f;
        float hi = 0.0f;
        fenceCrestFootprint(crest, fencePiece(1.0f), lo, hi);
        EXPECT_FALSE(hi > gateStart + 1.0e-3f && lo < gateEnd - 1.0e-3f)
            << "crest " << crest.index << " at [" << lo << ", " << hi << "] stands over the gate at [" << gateStart << ", " << gateEnd << "]";
    }

    // The control: a caller's reservation over the gate's stretch on the same run.
    const SplineFenceReservation reserved[1] = {{0u, gateStart, gateEnd}};
    const SplineFenceLayoutResult control = fenceStraightCastleRun(20.0f, overrides, gates, crests, 2.0f, reserved);
    EXPECT_EQ(fenceCellOrdinals(gated), fenceCellOrdinals(control));
    for (const SplineFenceCrest& crest : gated.crests){
        const SplineFenceCrest* same = fenceFindCell(control, crest.index);
        if (!crest.isCap && same){
            fenceExpectSameCrest(crest, *same, "cell beside a gate");
        }
    }
}

// On a row registered to its walls the gate's cell is its span, and only that cell goes.
TEST(SplineFenceLayoutTests, GateOnARegisteredRunRemovesOnlyItsOwnCell)
{
    const SplinePiece gates[1] = {fenceCastleGate()};
    const SplinePiece battlements[1] = {fenceCastleBattlement()};
    const SplineSpanOverride overrides[1] = {{0u, 1u, SplineSpanOverrideKind::Gate, 0u}};
    const SplineFenceLayoutResult result = fenceStraightCastleRun(20.0f, overrides, gates, battlements);

    ASSERT_EQ(result.spans.size(), 4u);
    std::vector<uint32_t> ordinals;
    for (const SplineFenceCrest& crest : result.crests){
        ASSERT_FALSE(crest.isCap);
        ordinals.push_back(crest.ordinalInRun);
        const SplineFenceSpan* wall = fenceSpanAtOrdinal(result, 0u, crest.ordinalInRun);
        ASSERT_NE(wall, nullptr);
        EXPECT_EQ(crest.lengthScale, wall->lengthScale) << "the run left its registration";
    }
    EXPECT_EQ(ordinals, (std::vector<uint32_t>{0u, 2u, 3u}));
}

// A registered row whose only wall is a gate places nothing, and says a reservation took it.
TEST(SplineFenceLayoutTests, AGateOnEveryRegisteredWallNamesTheReservation)
{
    const SplinePiece gates[1] = {fenceCastleGate()};
    const SplinePiece battlements[1] = {fenceCastleBattlement()};
    const SplineSpanOverride overrides[1] = {{0u, 0u, SplineSpanOverrideKind::Gate, 0u}};
    const SplineFenceLayoutResult gated = fenceStraightCastleRun(5.0f, overrides, gates, battlements);
    ASSERT_EQ(gated.spans.size(), 1u);
    ASSERT_TRUE(gated.spans[0].isGate);
    EXPECT_TRUE(gated.crests.empty());
    EXPECT_TRUE(fenceAnyValidationContains(gated, "a reserved stretch reaches every wall"));

    // The control: the same run without the gate lays its crest and says nothing.
    const SplineFenceLayoutResult walled = fenceStraightCastleRun(5.0f, {}, gates, battlements);
    EXPECT_EQ(walled.crests.size(), 1u);
    EXPECT_EQ(fenceEmptyCrestRowReports(walled), 0u);
}

// A pinned span draws the slot its override names, and every other span keeps its pick.
TEST(SplineFenceLayoutTests, PinnedSpanDrawsItsSlotAndLeavesEveryOtherPickUnchanged)
{
    const auto center = fenceStraightLine(24.0f, 49u);
    const float boundaries[2] = {0.0f, 48.0f};
    // Three pieces of one length, so the pin cannot change the count.
    const SplinePiece spans[3] = {fencePiece(2.0f), fencePiece(2.0f, 1.5f), fencePiece(2.0f, 2.0f)};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.seed = 11u;
    const SplineFenceLayoutResult seeded = buildFenceLayout(center, params);
    ASSERT_EQ(seeded.spans.size(), 12u);
    const uint32_t pinnedSlot = (seeded.spans[5].poolSlot + 1u) % 3u;

    const SplineSpanOverride overrides[1] = {{0u, 5u, SplineSpanOverrideKind::ExplicitPiece, static_cast<uint8_t>(pinnedSlot)}};
    params.spanOverrides = overrides;
    const SplineFenceLayoutResult pinned = buildFenceLayout(center, params);
    ASSERT_EQ(pinned.spans.size(), seeded.spans.size());
    for (size_t k = 0; k < pinned.spans.size(); ++k){
        EXPECT_EQ(pinned.spans[k].poolSlot, k == 5u ? pinnedSlot : seeded.spans[k].poolSlot) << k;
        EXPECT_FALSE(pinned.spans[k].isGate);
    }
    EXPECT_TRUE(pinned.validation.empty());
}

// An override the layout cannot apply says so, once, and never vanishes.
TEST(SplineFenceLayoutTests, OverridesTheLayoutCannotApplyAreReportedNeverDropped)
{
    const SplinePiece gates[1] = {fenceCastleGate()};
    const SplineSpanOverride overrides[5] = {
        {0u, 9u, SplineSpanOverrideKind::Gate, 0u},
        {4u, 0u, SplineSpanOverrideKind::Gate, 0u},
        {0u, 1u, SplineSpanOverrideKind::Gate, 3u},
        {0u, 2u, SplineSpanOverrideKind::ExplicitPiece, 5u},
        {0u, 2u, SplineSpanOverrideKind::Gate, 0u},
    };
    const SplineFenceLayoutResult result = fenceStraightCastleRun(20.0f, overrides, gates);

    EXPECT_TRUE(fenceAnyValidationContains(result, "a span override names span 9 of run 0->1, which has 4 span(s) \xE2\x80\x94 it places "
                                                   "nothing; move it or remove it"));
    EXPECT_TRUE(fenceAnyValidationContains(result, "a span override names knot 4, which opens no run \xE2\x80\x94 it places nothing"));
    EXPECT_TRUE(fenceAnyValidationContains(result, "the gate on span 1 of run 0->1 names Gate 4, but the Gate pool holds 1 \xE2\x80\x94 "
                                                   "the span is left open"));
    EXPECT_TRUE(fenceAnyValidationContains(result, "the explicit piece on span 2 of run 0->1 names Span 6, but the Span pool holds 1 \xE2\x80\x94 "
                                                   "it keeps the fence's own pick"));
    EXPECT_TRUE(fenceAnyValidationContains(result, "two span overrides name span 2 of run 0->1 \xE2\x80\x94 the first is used"));
    EXPECT_EQ(result.validation.size(), 5u);

    // The missing gate piece left an opening, and the first of the two overrides on span 2 won.
    EXPECT_EQ(fenceSpanAtOrdinal(result, 0u, 1u), nullptr);
    const SplineFenceSpan* span2 = fenceSpanAtOrdinal(result, 0u, 2u);
    ASSERT_NE(span2, nullptr);
    EXPECT_FALSE(span2->isGate);
}

// A gate longer than its whole run is built at the compression it needs, and reported.
TEST(SplineFenceLayoutTests, GateLongerThanItsRunIsBuiltAtTheRequiredStretchAndReported)
{
    SplinePiece longGate = fenceCastleGate();
    longGate.halfExtents.x = 4.0f;
    longGate.center.x = -4.0f;
    const SplinePiece gates[1] = {longGate};
    const SplineSpanOverride overrides[1] = {{0u, 0u, SplineSpanOverrideKind::Gate, 0u}};
    const SplineFenceLayoutResult result = fenceStraightCastleRun(5.0f, overrides, gates);

    ASSERT_EQ(result.spans.size(), 1u);
    EXPECT_TRUE(result.spans[0].isGate);
    EXPECT_NEAR(result.spans[0].lengthScale, 5.0f / 8.0f, 1e-4f);
    EXPECT_TRUE(fenceAnyValidationContains(result, "needs 0.625x, under the 0.800x floor"));
}

// A gate is a span at a join no post covers: its end is cut on the join's bisector plane. An
// opening has nothing to meet, so the wall beside it keeps a square end.
TEST(SplineFenceLayoutTests, AGateIsMitredAtABareBendAndAnOpeningLeavesItsNeighbourSquare)
{
    const auto center = fenceTurnedCorner(5.0f, 60.0f, 0.05f);
    const float last = static_cast<float>(center.size() - 1u);
    const float boundaries[3] = {0.0f, last * 0.5f, last};
    const SplinePiece spans[1] = {fencePiece(5.0f)};
    const SplinePiece gates[1] = {fencePiece(5.0f, 2.0f)};
    const SplineSpanOverride overrides[1] = {{0u, 0u, SplineSpanOverrideKind::Gate, 0u}};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.gatePieces = gates;
    params.spanOverrides = overrides;
    const SplineFenceLayoutResult gated = buildFenceLayout(center, params);
    ASSERT_EQ(gated.spans.size(), 2u);
    ASSERT_TRUE(gated.spans[0].isGate);
    EXPECT_TRUE(gated.spans[0].end.mitred);
    EXPECT_TRUE(gated.spans[1].start.mitred);

    params.gatePieces = {};
    const SplineFenceLayoutResult open = buildFenceLayout(center, params);
    ASSERT_EQ(open.spans.size(), 1u);
    EXPECT_FALSE(open.spans[0].start.mitred);
    EXPECT_FALSE(open.spans[0].end.mitred);
}

// Each run reports how many spans its fill holds, an opening included, by the knot opening it.
TEST(SplineFenceLayoutTests, RunSpanCountsCountEachRunsFillOpeningsIncluded)
{
    const SplineSpanOverride overrides[1] = {{0u, 1u, SplineSpanOverrideKind::Gate, 0u}};
    const SplineFenceLayoutResult open = fenceStraightCastleRun(20.0f, overrides, {});
    ASSERT_EQ(open.spans.size(), 3u);
    EXPECT_EQ(open.runSpanCounts, (std::vector<uint32_t>{4u}));

    const SplineFenceLayoutResult twoRuns = fenceFixtureStraightPanels(FENCE_ARM_NOTHING);
    ASSERT_EQ(twoRuns.runSpanCounts.size(), 2u);
    uint32_t spans0 = 0;
    uint32_t spans1 = 0;
    for (const SplineFenceSpan& span : twoRuns.spans){
        (span.run == 0u ? spans0 : spans1)++;
    }
    EXPECT_EQ(twoRuns.runSpanCounts[0], spans0);
    EXPECT_EQ(twoRuns.runSpanCounts[1], spans1);
}

// An override on a run whose two knots coincide says so rather than vanishing with the run.
TEST(SplineFenceLayoutTests, OverridesOnACoincidentRunAreReported)
{
    const auto center = fenceStraightLine(10.0f, 21u);
    const float boundaries[3] = {0.0f, 0.0f, 20.0f};
    const SplinePiece spans[1] = {fenceCastleWall()};
    const SplineSpanOverride overrides[1] = {{0u, 0u, SplineSpanOverrideKind::Gate, 0u}};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.spanOverrides = overrides;
    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    EXPECT_TRUE(fenceAnyValidationContains(result, "are coincident"));
    EXPECT_TRUE(fenceAnyValidationContains(result, "1 span override(s) on run 0->1 place nothing \xE2\x80\x94 the run has no length; move knot 1 or "
                                                   "remove them"));
}

// Only walls decide whether a run registers to its spans, and only a wall's slot indexes the span
// pool: a gate's slot names a gate pool piece.
TEST(SplineFenceLayoutTests, RegistrationReadsNoGateSlotFromTheSpanPool)
{
    const auto center = fenceStraightLine(20.0f, 41u);
    const float boundaries[2] = {0.0f, 40.0f};
    SplinePiece shortWall = fenceCastleWall();
    shortWall.halfExtents.x = 2.45f;
    shortWall.center.x = -2.45f;
    const SplinePiece spans[2] = {fenceCastleWall(), shortWall};
    SplinePiece shortGate = fenceCastleGate();
    shortGate.halfExtents.x = 2.0f;
    shortGate.center.x = -2.0f;
    const SplinePiece gates[2] = {fenceCastleGate(), shortGate};
    const SplinePiece battlements[1] = {fenceCastleBattlement()};
    const SplineSpanOverride overrides[4] = {
        {0u, 0u, SplineSpanOverrideKind::ExplicitPiece, 0u},
        {0u, 1u, SplineSpanOverrideKind::Gate, 1u},
        {0u, 2u, SplineSpanOverrideKind::ExplicitPiece, 0u},
        {0u, 3u, SplineSpanOverrideKind::ExplicitPiece, 0u},
    };
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.gatePieces = gates;
    params.spanOverrides = overrides;
    params.crestPieces = battlements;
    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_EQ(result.spans.size(), 4u);
    ASSERT_TRUE(result.spans[1].isGate);
    EXPECT_EQ(result.spans[1].poolSlot, 1u);
    for (const SplineFenceSpan& span : result.spans){
        EXPECT_EQ(span.poolSlot, span.isGate ? 1u : 0u);
    }
    std::vector<uint32_t> ordinals;
    for (const SplineFenceCrest& crest : result.crests){
        ordinals.push_back(crest.ordinalInRun);
        const SplineFenceSpan* wall = fenceSpanAtOrdinal(result, 0u, crest.ordinalInRun);
        ASSERT_NE(wall, nullptr);
        EXPECT_EQ(crest.lengthScale, wall->lengthScale) << "the run left its registration";
    }
    EXPECT_EQ(ordinals, (std::vector<uint32_t>{0u, 2u, 3u}));
}

// A registered crest wider than its wall reaches across a bare join on the wall's side; a gate
// stands in a reserved cell with no crest on it, so its side takes no crest reach.
TEST(SplineFenceLayoutTests, AGateSideOfABareJoinTakesNoCrestReach)
{
    const auto center = fenceTurnedCorner(5.0f, 60.0f, 0.05f);
    const float last = static_cast<float>(center.size() - 1u);
    const float boundaries[3] = {0.0f, last * 0.5f, last};
    const SplinePiece spans[1] = {fencePiece(5.0f)};
    const SplinePiece gates[1] = {fencePiece(5.0f, 2.0f)};
    SplinePiece wideCrest = fencePiece(5.0f, 0.5f);
    wideCrest.halfExtents.x = 0.6f;
    const SplinePiece crests[1] = {wideCrest};
    const SplineSpanOverride overrides[1] = {{1u, 0u, SplineSpanOverrideKind::Gate, 0u}};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.gatePieces = gates;
    params.spanOverrides = overrides;
    const SplineFenceLayoutResult bare = buildFenceLayout(center, params);
    params.crestPieces = crests;
    const SplineFenceLayoutResult crested = buildFenceLayout(center, params);
    ASSERT_EQ(bare.spans.size(), 2u);
    ASSERT_EQ(crested.spans.size(), 2u);
    ASSERT_TRUE(crested.spans[1].isGate);
    ASSERT_TRUE(crested.spans[1].start.mitred);
    EXPECT_EQ(crested.spans[1].lengthScale, bare.spans[1].lengthScale) << "the gate took a crest's reach it has no crest for";
    // The control: the wall on the other side does carry its crest's reach.
    EXPECT_GT(crested.spans[0].lengthScale, bare.spans[0].lengthScale);
}

// ---- S18 AC7: a gate override lands on the requested span, and the TRS poses ---

// A three-knot fence of 2 m spans with a 1 m gate pool; the override names span 2 of the run
// opening at knot 1, and exactly that span becomes the gate, between that run's stations 2 and 3.
TEST(SplineFenceLayoutTests, GateOverrideLandsOnTheRequestedSpan)
{
    const auto center = fenceStraightLine(20.0f, 41u);
    const float boundaries[3] = {0.0f, 20.0f, 40.0f};
    const SplinePiece spans[1] = {fencePiece(2.0f)};
    const SplinePiece gates[1] = {fencePiece(1.0f)};
    const SplineSpanOverride overrides[1] = {{1u, 2u, SplineSpanOverrideKind::Gate, 0u}};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.spanPieces = spans;
    params.gatePieces = gates;
    params.spanOverrides = overrides;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_EQ(result.runSpanCounts.size(), 2u);
    ASSERT_EQ(result.stations.size(), result.spans.size() + 1u);
    const auto gateCount = std::count_if(result.spans.begin(), result.spans.end(), [](const SplineFenceSpan& s){ return s.isGate; });
    ASSERT_EQ(gateCount, 1);
    const auto gate = std::find_if(result.spans.begin(), result.spans.end(), [](const SplineFenceSpan& s){ return s.isGate; });
    EXPECT_EQ(gate->run, 1u);
    EXPECT_EQ(gate->ordinalInRun, 2u);
    EXPECT_EQ(gate->poolSlot, 0u);

    // Run 0 holds runSpanCounts[0] spans and as many stations before run 1's opening station.
    const uint32_t opening = result.runSpanCounts[0];
    ASSERT_LT(opening + 3u, result.stations.size());
    EXPECT_TRUE(result.stations[opening].isKnot);
    EXPECT_EQ(result.stations[opening].knot, 1u);
    EXPECT_EQ(gate->station, opening + 2u);
    const Vector3& a = result.stations[opening + 2u].pose.base;
    const Vector3& b = result.stations[opening + 3u].pose.base;
    const Vector3 mid = (a + b) * 0.5f;
    EXPECT_NEAR(fenceLength(gate->pose.base - mid), 0.0f, 1.0e-3f);
    // between the two stations along travel, and laid to fill exactly that gap
    EXPECT_LT(gate->pose.base.z, a.z);
    EXPECT_GT(gate->pose.base.z, b.z);
    EXPECT_NEAR(gate->lengthScale * gates[0].length(), fenceLength(b - a), 1.0e-3f);
    EXPECT_TRUE(result.validation.empty());

    // Nothing else is a gate: every other span draws the span pool.
    for (const SplineFenceSpan& span : result.spans){
        if (&span == &*gate){
            continue;
        }
        EXPECT_FALSE(span.isGate);
    }
}

// Every TRS pose a racked fence over a slope emits lays its piece's run axis on its frame's
// forward and its +Y on the frame's up (posts and crests run along Z, the panels along X), with
// the mesh origin on the frame position and the stretch on the run axis only.
TEST(SplineFenceLayoutTests, PiecePosesOrientRunAxisAlongForward)
{
    const auto center = fenceRamp(20.0f, 4.0f, 41u);
    const float boundaries[2] = {0.0f, 40.0f};
    SplinePiece post = fenceMeasuredKitPost();
    post.poolIndex = 10u;
    SplinePiece panel = fenceMeasuredKitPanel();
    panel.poolIndex = 20u;
    SplinePiece crest = fencePiece(1.0f);
    crest.poolIndex = 30u;
    ASSERT_EQ(post.axis(), SplinePieceAxis::Z);
    ASSERT_EQ(panel.axis(), SplinePieceAxis::X);
    ASSERT_EQ(crest.axis(), SplinePieceAxis::Z);
    const SplinePiece posts[1] = {post};
    const SplinePiece spans[1] = {panel};
    const SplinePiece crests[1] = {crest};
    SplineFenceLayoutParams params;
    params.knotStations = boundaries;
    params.postPieces = posts;
    params.spanPieces = spans;
    params.crestPieces = crests;
    params.crestPitch = 2.0f;
    params.spanGrade = SplineSpanGrade::Racked;

    const SplineFenceLayoutResult result = buildFenceLayout(center, params);
    ASSERT_FALSE(result.stations.empty());
    ASSERT_FALSE(result.spans.empty());
    ASSERT_FALSE(result.crests.empty());
    // the racked spans really pitch
    EXPECT_GT(result.spans.front().pose.forward.y, 0.1f);

    std::vector<SplinePiecePose> poses;
    appendFencePiecePoses(result, params, poses);
    ASSERT_EQ(poses.size(), result.stations.size() + result.spans.size() + result.crests.size());

    const Vector3 unitX(1.0f, 0.0f, 0.0f);
    const Vector3 unitY(0.0f, 1.0f, 0.0f);
    const Vector3 unitZ(0.0f, 0.0f, 1.0f);
    const auto expectOriented = [&](const SplinePiecePose& pose, const SplineLayoutPose& frame, const SplinePiece& piece, float lengthScale,
                                    SplinePieceRole role, const char* what, size_t k){
        EXPECT_EQ(pose.role, role) << what << " " << k;
        EXPECT_EQ(pose.poolIndex, piece.poolIndex) << what << " " << k;
        const Vector3 runAxis = piece.axis() == SplinePieceAxis::X ? unitX : unitZ;
        const Vector3 along = pose.rotation * runAxis;
        const Vector3 up = pose.rotation * unitY;
        EXPECT_NEAR(along.x, frame.forward.x, 1.0e-5f) << what << " " << k;
        EXPECT_NEAR(along.y, frame.forward.y, 1.0e-5f) << what << " " << k;
        EXPECT_NEAR(along.z, frame.forward.z, 1.0e-5f) << what << " " << k;
        EXPECT_NEAR(up.x, frame.up.x, 1.0e-5f) << what << " " << k;
        EXPECT_NEAR(up.y, frame.up.y, 1.0e-5f) << what << " " << k;
        EXPECT_NEAR(up.z, frame.up.z, 1.0e-5f) << what << " " << k;
        EXPECT_NEAR(fenceLength(pose.position - frame.position), 0.0f, 1.0e-4f) << what << " " << k;
        const float runScale = piece.axis() == SplinePieceAxis::X ? pose.scale.x : pose.scale.z;
        const float acrossScale = piece.axis() == SplinePieceAxis::X ? pose.scale.z : pose.scale.x;
        EXPECT_NEAR(runScale, lengthScale, 1.0e-6f) << what << " " << k;
        EXPECT_NEAR(acrossScale, 1.0f, 1.0e-6f) << what << " " << k;
        EXPECT_NEAR(pose.scale.y, 1.0f, 1.0e-6f) << what << " " << k;
    };

    size_t at = 0;
    for (size_t k = 0; k < result.stations.size(); ++k){
        expectOriented(poses[at], result.stations[k].pose, post, 1.0f, SplinePieceRole::Post, "post", k);
        EXPECT_EQ(poses[at].stationIndex, result.stations[k].index);
        at++;
    }
    for (size_t k = 0; k < result.spans.size(); ++k){
        expectOriented(poses[at], result.spans[k].pose, panel, result.spans[k].lengthScale, SplinePieceRole::Span, "span", k);
        EXPECT_EQ(poses[at].ordinal, result.spans[k].index);
        at++;
    }
    for (size_t k = 0; k < result.crests.size(); ++k){
        expectOriented(poses[at], result.crests[k].pose, crest, result.crests[k].lengthScale, SplinePieceRole::Crest, "crest", k);
        EXPECT_EQ(poses[at].ordinal, result.crests[k].index);
        at++;
    }
}
