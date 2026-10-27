// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineLayout/Tests/SplineStationJitterTests.cpp and
// GoldenUlps.h, Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License.
// See NOTICE.md.

// Per-station variation (S18 R8): the hash itself, then the station walk's jitter, dropout and
// end taper. OpenEngine is left-handed; every world-space scenario here is its mirror (z
// negated, and a piece's across-travel local coordinate with it), so the captured goldens
// carry over with the sign of every z flipped.

#include "SplineTileLayout.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    // Goldens are float bit patterns captured on x86-64. libm rounds sin/cos differently per
    // architecture, so a value from textually identical arithmetic can land a few ULP away on
    // arm64: compare in ULPs. A visible layout change moves a value by thousands of ULPs.
    constexpr uint32_t JITTER_MAX_GOLDEN_ULPS = 64;

    uint32_t jitterFloatBits(float value){
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    // how many representable floats lie between two bit patterns (+0 and -0 are one point)
    uint64_t jitterUlpDistance(uint32_t aBits, uint32_t bBits){
        constexpr uint32_t signBit = 0x80000000u;
        const auto ordered = [](uint32_t bits) -> int64_t{
            const int64_t magnitude = static_cast<int64_t>(bits & ~signBit);
            return (bits & signBit) != 0u ? -magnitude : magnitude;
        };
        const int64_t a = ordered(aBits);
        const int64_t b = ordered(bBits);
        return static_cast<uint64_t>(a > b ? a - b : b - a);
    }

    // Curving in XZ, climbing in Y, with a tilted surface normal: a golden over it is sensitive
    // to the station walk, the chord yaw, the slope blend and the footprint centering alike.
    std::vector<SplineCenterSample> jitterGoldenScenario(){
        std::vector<SplineCenterSample> center;
        constexpr int samples = 41;
        for (int i = 0; i < samples; i++){
            const float t = static_cast<float>(i) / static_cast<float>(samples - 1);
            SplineCenterSample s;
            s.position = Vector3(4.0f * t * t, 1.5f * t, -(20.0f * t));
            s.normal = normalizedOrFallback(Vector3(0.12f, 1.0f, 0.20f * t), Vector3(0.0f, 1.0f, 0.0f));
            center.push_back(s);
        }
        return center;
    }

    // the corner-pivoted, roughly square footprint of the measured kit tile
    SplineTileFrameParams jitterGoldenParams(){
        SplineTileFrameParams p;
        p.spacing = 2.0f;
        p.fit = SplinePlacementFit::FitToLength;
        p.alignToSurfaceNormal = true;
        p.slopeBlend = 1.0f;
        p.maxTiltDegrees = 35.0f;
        p.meshBoundsCenter = Vector3(-1.2f, 0.05f, 1.1f);
        p.meshBoundsHalfExtents = Vector3(1.19f, 0.05f, 1.31f);
        p.plantMode = SplinePlantMode::BoundsMin;
        p.maxTiles = 2048;
        return p;
    }

    // a flat straight run along -Z (the engine's forward), for the tests reasoning in distance
    std::vector<SplineCenterSample> jitterFlatLine(float length, uint32_t sampleCount){
        std::vector<SplineCenterSample> center;
        for (uint32_t i = 0; i < sampleCount; i++){
            SplineCenterSample s;
            s.position = Vector3(0.0f, 0.0f, -(length * static_cast<float>(i) / static_cast<float>(sampleCount - 1)));
            center.push_back(s);
        }
        return center;
    }

    SplineTileFrameParams jitterFlatParams(float spacing){
        SplineTileFrameParams p;
        p.spacing = spacing;
        p.fit = SplinePlacementFit::FitToLength;
        p.maxTiles = 2048;
        return p;
    }

    std::vector<SplineLayoutPose> jitterFrames(const std::vector<SplineCenterSample>& center, const SplineTileFrameParams& params){
        std::vector<SplineLayoutPose> out;
        buildTileFrames(center, params, out);
        return out;
    }

    bool jitterSameBits(const Vector3& a, const Vector3& b){
        return jitterFloatBits(a.x) == jitterFloatBits(b.x) && jitterFloatBits(a.y) == jitterFloatBits(b.y) &&
               jitterFloatBits(a.z) == jitterFloatBits(b.z);
    }

    bool jitterPoseBitsEqual(const SplineLayoutPose& a, const SplineLayoutPose& b){
        return jitterSameBits(a.position, b.position) && jitterSameBits(a.right, b.right) && jitterSameBits(a.up, b.up) &&
               jitterSameBits(a.forward, b.forward) && jitterSameBits(a.base, b.base) && a.stationIndex == b.stationIndex;
    }

}

// ---- The hash ----------------------------------------------------------------------------

TEST(SplineJitterTests, DrawsArePureFunctionsOfSeedChannelAndOrdinal){
    static_assert(splineJitterUnit(7u, SplineJitterChannel::Yaw, 3u) == splineJitterUnit(7u, SplineJitterChannel::Yaw, 3u));
    float lo = 1.0f;
    float hi = 0.0f;
    uint32_t sameAcrossChannels = 0;
    for (uint32_t i = 0; i < 4096u; i++){
        const float u = splineJitterUnit(11u, SplineJitterChannel::Spacing, i);
        lo = std::min(lo, u);
        hi = std::max(hi, u);
        if (u == splineJitterUnit(11u, SplineJitterChannel::Dropout, i)){
            sameAcrossChannels++;
        }
        const float s = splineJitterSigned(11u, SplineJitterChannel::Spacing, i);
        EXPECT_EQ(s, u * 2.0f - 1.0f);
    }
    EXPECT_GE(lo, 0.0f);
    EXPECT_LT(hi, 1.0f);
    // 4096 draws over [0, 1) reach close to both ends
    EXPECT_LT(lo, 0.01f);
    EXPECT_GT(hi, 0.99f);
    EXPECT_LT(sameAcrossChannels, 4u) << "channels are not salted apart";
    // the pinned mix: a toolchain or library change that moved it would reshuffle every scene
    EXPECT_EQ(splineSelectionMix(0u), 0xE220A8397B1DCDAFull);
}

TEST(SplineJitterTests, EqualWeightsPickExactlyAsTheUniformSelect){
    const float equal[3] = {2.0f, 2.0f, 2.0f};
    for (uint32_t i = 0; i < 512u; i++){
        const uint32_t weighted = splinePoolSelectWeighted(5u, SplinePoolRole::Span, i, 3u, [&equal](uint32_t k){ return equal[k]; });
        EXPECT_EQ(weighted, splinePoolSelect(5u, SplinePoolRole::Span, i, 3u));
    }
}

TEST(SplineJitterTests, WeightsSkewThePickInProportion){
    const float weights[3] = {1.0f, 0.0f, 3.0f};
    uint32_t counts[3] = {0, 0, 0};
    constexpr uint32_t draws = 8000u;
    for (uint32_t i = 0; i < draws; i++){
        counts[splinePoolSelectWeighted(9u, SplinePoolRole::Tile, i, 3u, [&weights](uint32_t k){ return weights[k]; })]++;
    }
    EXPECT_EQ(counts[1], 0u) << "a zero weight is never picked";
    EXPECT_NEAR(static_cast<float>(counts[2]) / static_cast<float>(draws), 0.75f, 0.03f);

    // unusable weights count as zero; all-unusable picks uniformly
    const float poisoned[2] = {std::numeric_limits<float>::quiet_NaN(), -1.0f};
    for (uint32_t i = 0; i < 64u; i++){
        EXPECT_EQ(splinePoolSelectWeighted(9u, SplinePoolRole::Tile, i, 2u, [&poisoned](uint32_t k){ return poisoned[k]; }),
                  splinePoolSelect(9u, SplinePoolRole::Tile, i, 2u));
    }
}

// ---- The zero-behaviour-change contract ----------------------------------------------------

// With every variation knob at its default the walk is what it was BEFORE the knobs existed,
// to the bit. The positions were captured by OpenEngine from its tile layout before the knobs
// (Apps/Editor/Source/Placement/TileLayout.cpp at f865d7f0f) on the unmirrored scenario; the
// mirror flips the sign bit of every z and leaves x and y alone.
TEST(SplineJitterTests, DefaultKnobsReproduceThePreChangeLayoutExactly){
    constexpr uint32_t goldenPositionBits[][3] = {
        {0xBF9A5E38, 0x3E14317D, 0xBF8AB9EA}, {0xBF98F922, 0x3E8FC41B, 0x3F8025EE},
        {0xBF8E65CD, 0x3ED726F6, 0x40467374}, {0xBF71B795, 0x3F0FA401, 0x40A63EF9},
        {0xBF310384, 0x3F3407AF, 0x40E8FBA1}, {0xBEB63DFE, 0x3F58B121, 0x4115A9DA},
        {0x3D767770, 0x3F7D91C9, 0x413696A7}, {0x3F0DE6A8, 0x3F914D2B, 0x4157387F},
        {0x3F8FCF9A, 0x3FA3DD88, 0x4177850D}, {0x3FE1E037, 0x3FB67220, 0x418BB9C1},
        {0x401EBB92, 0x3FC8827E, 0x419B5D40},
    };
    constexpr size_t goldenCount = sizeof(goldenPositionBits) / sizeof(goldenPositionBits[0]);
    constexpr uint32_t mirrorZ = 0x80000000u;

    const std::vector<SplineLayoutPose> poses = jitterFrames(jitterGoldenScenario(), jitterGoldenParams());
    ASSERT_EQ(poses.size(), goldenCount) << "the default station count moved; the knobs are supposed to be inert by default";
    for (size_t i = 0; i < goldenCount; i++){
        EXPECT_LE(jitterUlpDistance(jitterFloatBits(poses[i].position.x), goldenPositionBits[i][0]), JITTER_MAX_GOLDEN_ULPS)
            << "station " << i << " x";
        EXPECT_LE(jitterUlpDistance(jitterFloatBits(poses[i].position.y), goldenPositionBits[i][1]), JITTER_MAX_GOLDEN_ULPS)
            << "station " << i << " y";
        EXPECT_LE(jitterUlpDistance(jitterFloatBits(poses[i].position.z), goldenPositionBits[i][2] ^ mirrorZ), JITTER_MAX_GOLDEN_ULPS)
            << "station " << i << " z";
    }
}

// The positive control for the golden: the same comparison MUST be able to fail.
TEST(SplineJitterTests, TheGoldenComparisonCanActuallyFail){
    SplineTileFrameParams jittered = jitterGoldenParams();
    jittered.seed = 1234u;
    jittered.spacingJitterMetres = 0.6f;

    const std::vector<SplineLayoutPose> base = jitterFrames(jitterGoldenScenario(), jitterGoldenParams());
    const std::vector<SplineLayoutPose> moved = jitterFrames(jitterGoldenScenario(), jittered);
    ASSERT_EQ(base.size(), moved.size()) << "spacing jitter must not change the station COUNT";

    bool anyMoved = false;
    for (size_t i = 0; i < base.size(); i++){
        anyMoved = anyMoved || !jitterPoseBitsEqual(base[i], moved[i]);
    }
    EXPECT_TRUE(anyMoved) << "jitter changed nothing, so the golden test proves nothing";
}

// every knob explicitly zeroed is the same as every knob left default
TEST(SplineJitterTests, ExplicitZeroKnobsMatchTheDefaults){
    SplineTileFrameParams zeroed = jitterGoldenParams();
    zeroed.seed = 4242u;
    zeroed.spacingJitterMetres = 0.0f;
    zeroed.yawJitterDegrees = 0.0f;
    zeroed.lateralJitterMetres = 0.0f;
    zeroed.dropoutChance = 0.0f;
    zeroed.endTaperMetres = 0.0f;

    const std::vector<SplineLayoutPose> defaults = jitterFrames(jitterGoldenScenario(), jitterGoldenParams());
    const std::vector<SplineLayoutPose> explicitZeros = jitterFrames(jitterGoldenScenario(), zeroed);
    ASSERT_EQ(defaults.size(), explicitZeros.size());
    for (size_t i = 0; i < defaults.size(); i++){
        EXPECT_TRUE(jitterPoseBitsEqual(defaults[i], explicitZeros[i])) << "station " << i;
    }
}

// ---- Determinism ---------------------------------------------------------------------------

TEST(SplineJitterTests, SameSeedReproducesTheLayoutExactly){
    SplineTileFrameParams p = jitterGoldenParams();
    p.seed = 777u;
    p.spacingJitterMetres = 0.5f;
    p.yawJitterDegrees = 15.0f;
    p.lateralJitterMetres = 0.3f;
    p.dropoutChance = 0.2f;
    p.endTaperMetres = 4.0f;

    const std::vector<SplineLayoutPose> first = jitterFrames(jitterGoldenScenario(), p);
    const std::vector<SplineLayoutPose> second = jitterFrames(jitterGoldenScenario(), p);
    ASSERT_EQ(first.size(), second.size());
    ASSERT_FALSE(first.empty());
    for (size_t i = 0; i < first.size(); i++){
        EXPECT_TRUE(jitterPoseBitsEqual(first[i], second[i])) << "pose " << i;
    }
}

TEST(SplineJitterTests, ADifferentSeedGivesADifferentLayout){
    SplineTileFrameParams a = jitterGoldenParams();
    a.seed = 1u;
    a.spacingJitterMetres = 0.5f;
    a.lateralJitterMetres = 0.3f;
    SplineTileFrameParams b = a;
    b.seed = 2u;

    const std::vector<SplineLayoutPose> first = jitterFrames(jitterGoldenScenario(), a);
    const std::vector<SplineLayoutPose> second = jitterFrames(jitterGoldenScenario(), b);
    ASSERT_EQ(first.size(), second.size());
    bool anyDiffer = false;
    for (size_t i = 0; i < first.size(); i++){
        anyDiffer = anyDiffer || !jitterPoseBitsEqual(first[i], second[i]);
    }
    EXPECT_TRUE(anyDiffer) << "the seed does not reach the jitter";
}

// ---- Spacing jitter ------------------------------------------------------------------------

// a station may slide, but never past a neighbour: reordering would hand pieces the wrong chord
TEST(SplineJitterTests, SpacingJitterNeverReordersStations){
    for (const float asked : {0.5f, 1.0f, 5.0f, 1000.0f}){
        SplineTileFrameParams p = jitterFlatParams(2.0f);
        p.seed = 31u;
        p.spacingJitterMetres = asked;
        const std::vector<SplineLayoutPose> poses = jitterFrames(jitterFlatLine(40.0f, 81u), p);
        ASSERT_GE(poses.size(), 2u);
        for (size_t i = 1; i < poses.size(); i++){
            EXPECT_LE(poses[i].base.z, poses[i - 1].base.z) << "asked " << asked << ": station " << i << " slid past its neighbour";
        }
    }
}

// however much jitter is asked for, no station moves further than half the spacing
TEST(SplineJitterTests, SpacingJitterIsClampedToHalfTheSpacing){
    constexpr float spacing = 2.0f;
    constexpr float length = 40.0f;
    SplineTileFrameParams p = jitterFlatParams(spacing);
    p.seed = 5u;
    p.spacingJitterMetres = 100.0f;

    const std::vector<SplineLayoutPose> jittered = jitterFrames(jitterFlatLine(length, 81u), p);
    const std::vector<SplineLayoutPose> nominal = jitterFrames(jitterFlatLine(length, 81u), jitterFlatParams(spacing));
    ASSERT_EQ(jittered.size(), nominal.size());

    bool anyActuallyMoved = false;
    for (size_t i = 0; i < jittered.size(); i++){
        const float moved = std::abs(jittered[i].base.z - nominal[i].base.z);
        EXPECT_LE(moved, spacing * 0.5f + 1.0e-3f) << "station " << i;
        anyActuallyMoved = anyActuallyMoved || moved > 1.0e-3f;
    }
    EXPECT_TRUE(anyActuallyMoved) << "the clamp swallowed the whole effect";
}

// ---- Dropout -------------------------------------------------------------------------------

// Removing stations must not disturb the ones that remain: a survivor keeps its ordinal and
// its pose is bit-identical to the pose it had when nothing was dropped.
TEST(SplineJitterTests, DropoutLeavesSurvivorsBitIdenticalAndKeepsTheirOrdinals){
    SplineTileFrameParams full = jitterFlatParams(2.0f);
    full.seed = 91u;
    full.spacingJitterMetres = 0.4f;
    full.lateralJitterMetres = 0.25f;
    full.yawJitterDegrees = 10.0f;

    SplineTileFrameParams thinned = full;
    thinned.dropoutChance = 0.35f;

    const std::vector<SplineLayoutPose> all = jitterFrames(jitterFlatLine(60.0f, 121u), full);
    const std::vector<SplineLayoutPose> some = jitterFrames(jitterFlatLine(60.0f, 121u), thinned);
    ASSERT_FALSE(all.empty());
    ASSERT_LT(some.size(), all.size()) << "dropout removed nothing";
    ASSERT_FALSE(some.empty()) << "dropout removed everything";

    for (size_t i = 0; i < some.size(); i++){
        if (i > 0){
            EXPECT_GT(some[i].stationIndex, some[i - 1].stationIndex);
        }
        ASSERT_LT(some[i].stationIndex, all.size());
        EXPECT_TRUE(jitterPoseBitsEqual(some[i], all[some[i].stationIndex]))
            << "survivor at ordinal " << some[i].stationIndex << " moved when its neighbours left";
    }
}

// stationIndex is the ordinal along the spline, NOT the index in the result
TEST(SplineJitterTests, StationIndexIsTheOrdinalNotTheArrayIndex){
    SplineTileFrameParams p = jitterFlatParams(2.0f);
    p.seed = 3u;
    p.dropoutChance = 0.5f;
    const std::vector<SplineLayoutPose> poses = jitterFrames(jitterFlatLine(60.0f, 121u), p);
    ASSERT_GE(poses.size(), 2u);

    bool anyDiverges = false;
    for (size_t i = 0; i < poses.size(); i++){
        anyDiverges = anyDiverges || poses[i].stationIndex != static_cast<uint32_t>(i);
    }
    EXPECT_TRUE(anyDiverges) << "with half the stations dropped, ordinals must have outrun array indices";
}

// with no dropout the two coincide
TEST(SplineJitterTests, WithoutDropoutTheOrdinalIsTheArrayIndex){
    const std::vector<SplineLayoutPose> poses = jitterFrames(jitterGoldenScenario(), jitterGoldenParams());
    ASSERT_FALSE(poses.empty());
    for (size_t i = 0; i < poses.size(); i++){
        EXPECT_EQ(poses[i].stationIndex, static_cast<uint32_t>(i));
    }
}

TEST(SplineJitterTests, FullDropoutPlacesNothingAndNoDropoutPlacesEverything){
    SplineTileFrameParams none = jitterFlatParams(2.0f);
    none.seed = 8u;
    none.dropoutChance = 0.0f;
    EXPECT_EQ(jitterFrames(jitterFlatLine(40.0f, 81u), none).size(), 21u);

    SplineTileFrameParams all = none;
    all.dropoutChance = 1.0f;
    EXPECT_TRUE(jitterFrames(jitterFlatLine(40.0f, 81u), all).empty());
}

// ---- End taper -----------------------------------------------------------------------------

// the taper thins toward the ends and leaves the middle alone: measure both halves
TEST(SplineJitterTests, EndTaperThinsTheEndsAndLeavesTheMiddleIntact){
    constexpr float length = 60.0f;
    constexpr float taper = 15.0f;
    SplineTileFrameParams p = jitterFlatParams(2.0f);
    p.seed = 17u;
    p.endTaperMetres = taper;

    const std::vector<SplineLayoutPose> tapered = jitterFrames(jitterFlatLine(length, 121u), p);
    const std::vector<SplineLayoutPose> square = jitterFrames(jitterFlatLine(length, 121u), jitterFlatParams(2.0f));
    ASSERT_LT(tapered.size(), square.size()) << "the taper removed nothing";

    const auto inZone = [](const std::vector<SplineLayoutPose>& poses, bool nearEnds){
        return std::count_if(poses.begin(), poses.end(), [&](const SplineLayoutPose& t){
            const float along = -t.base.z;
            const float fromEnd = std::min(along, length - along);
            return nearEnds ? (fromEnd < taper) : (fromEnd >= taper);
        });
    };

    EXPECT_EQ(inZone(tapered, false), inZone(square, false)) << "the taper reached past its own zone into the middle";
    EXPECT_LT(inZone(tapered, true), inZone(square, true));
}

// keep probability hits zero AT each end, which is what "peters out" means
TEST(SplineJitterTests, TaperedRunsDoNotStartOrEndOnAStation){
    constexpr float length = 40.0f;
    SplineTileFrameParams p = jitterFlatParams(2.0f);
    p.seed = 23u;
    p.endTaperMetres = length * 0.5f;

    const std::vector<SplineLayoutPose> poses = jitterFrames(jitterFlatLine(length, 81u), p);
    ASSERT_FALSE(poses.empty());
    EXPECT_LT(poses.front().base.z, 0.0f);
    EXPECT_GT(poses.back().base.z, -length);
}

// dropout and taper compose on the KEEP probability
TEST(SplineJitterTests, DropoutAndTaperCompose){
    SplineTileFrameParams taperOnly = jitterFlatParams(2.0f);
    taperOnly.seed = 44u;
    taperOnly.endTaperMetres = 12.0f;

    SplineTileFrameParams dropOnly = jitterFlatParams(2.0f);
    dropOnly.seed = 44u;
    dropOnly.dropoutChance = 0.3f;

    SplineTileFrameParams both = taperOnly;
    both.dropoutChance = 0.3f;

    const size_t taperCount = jitterFrames(jitterFlatLine(60.0f, 121u), taperOnly).size();
    const size_t dropCount = jitterFrames(jitterFlatLine(60.0f, 121u), dropOnly).size();
    const size_t bothCount = jitterFrames(jitterFlatLine(60.0f, 121u), both).size();
    EXPECT_LE(bothCount, taperCount);
    EXPECT_LE(bothCount, dropCount);
}

// ---- Yaw and lateral jitter ----------------------------------------------------------------

// yaw jitter turns pieces without corrupting the frame they are turned in
TEST(SplineJitterTests, YawJitterTurnsTilesAndKeepsTheBasisOrthonormal){
    SplineTileFrameParams p = jitterFlatParams(2.0f);
    p.seed = 61u;
    p.yawJitterDegrees = 20.0f;

    const std::vector<SplineLayoutPose> turned = jitterFrames(jitterFlatLine(40.0f, 81u), p);
    const std::vector<SplineLayoutPose> straight = jitterFrames(jitterFlatLine(40.0f, 81u), jitterFlatParams(2.0f));
    ASSERT_EQ(turned.size(), straight.size());

    bool anyTurned = false;
    for (size_t i = 0; i < turned.size(); i++){
        const SplineLayoutPose& t = turned[i];
        EXPECT_NEAR(t.right.dotProduct(t.right), 1.0f, 1.0e-4f) << "pose " << i;
        EXPECT_NEAR(t.up.dotProduct(t.up), 1.0f, 1.0e-4f) << "pose " << i;
        EXPECT_NEAR(t.forward.dotProduct(t.forward), 1.0f, 1.0e-4f) << "pose " << i;
        EXPECT_NEAR(t.right.dotProduct(t.forward), 0.0f, 1.0e-4f) << "pose " << i;
        EXPECT_NEAR(t.right.dotProduct(t.up), 0.0f, 1.0e-4f) << "pose " << i;
        // still right-handed: right = forward x up
        EXPECT_GT(t.forward.crossProduct(t.up).dotProduct(t.right), 0.999f) << "pose " << i;

        const float turn = std::acos(std::clamp(t.forward.dotProduct(straight[i].forward), -1.0f, 1.0f)) * 180.0f / 3.14159265358979f;
        EXPECT_LE(turn, 20.0f + 1.0e-2f) << "pose " << i << " turned past the cap";
        anyTurned = anyTurned || turn > 1.0f;
    }
    EXPECT_TRUE(anyTurned) << "yaw jitter turned nothing";
}

// lateral jitter moves ACROSS travel: on a straight -Z run X moves and the along coordinate not
TEST(SplineJitterTests, LateralJitterMovesAcrossTravelNotAlongIt){
    SplineTileFrameParams p = jitterFlatParams(2.0f);
    p.seed = 71u;
    p.lateralJitterMetres = 0.75f;

    const std::vector<SplineLayoutPose> slid = jitterFrames(jitterFlatLine(40.0f, 81u), p);
    const std::vector<SplineLayoutPose> centred = jitterFrames(jitterFlatLine(40.0f, 81u), jitterFlatParams(2.0f));
    ASSERT_EQ(slid.size(), centred.size());

    bool anySlid = false;
    for (size_t i = 0; i < slid.size(); i++){
        EXPECT_NEAR(slid[i].base.z, centred[i].base.z, 1.0e-4f) << "pose " << i << " moved along the path, not across it";
        const float across = std::abs(slid[i].base.x - centred[i].base.x);
        EXPECT_LE(across, 0.75f + 1.0e-3f) << "pose " << i;
        anySlid = anySlid || across > 1.0e-3f;
    }
    EXPECT_TRUE(anySlid) << "lateral jitter moved nothing";
}

// a constant lateral offset is on the right of travel: +X when travelling -Z
TEST(SplineJitterTests, LateralOffsetIsOnTheRightOfTravel){
    SplineTileFrameParams p = jitterFlatParams(2.0f);
    p.lateralOffset = 1.5f;
    const std::vector<SplineLayoutPose> poses = jitterFrames(jitterFlatLine(20.0f, 41u), p);
    ASSERT_FALSE(poses.empty());
    for (const SplineLayoutPose& pose : poses){
        EXPECT_NEAR(pose.base.x, 1.5f, 1.0e-5f);
        EXPECT_NEAR((pose.base - Vector3(0.0f, 0.0f, pose.base.z)).dotProduct(pose.right), 1.5f, 1.0e-5f);
    }
}

// ---- Hostile input -------------------------------------------------------------------------

// a poisoned knob means "no variation", never a NaN pose
TEST(SplineJitterTests, NonFiniteKnobsAreIgnoredRatherThanPropagated){
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    for (const float poison : {nan, inf, -inf}){
        SplineTileFrameParams p = jitterGoldenParams();
        p.seed = 12u;
        p.spacingJitterMetres = poison;
        p.yawJitterDegrees = poison;
        p.lateralJitterMetres = poison;
        p.lateralOffset = poison;
        p.dropoutChance = poison;
        p.endTaperMetres = poison;

        const std::vector<SplineLayoutPose> poses = jitterFrames(jitterGoldenScenario(), p);
        const std::vector<SplineLayoutPose> clean = jitterFrames(jitterGoldenScenario(), jitterGoldenParams());
        ASSERT_EQ(poses.size(), clean.size()) << "a poisoned knob changed the station count";
        for (size_t i = 0; i < poses.size(); i++){
            ASSERT_TRUE(std::isfinite(poses[i].position.x) && std::isfinite(poses[i].position.y) && std::isfinite(poses[i].position.z))
                << "pose " << i << " is non-finite";
            EXPECT_TRUE(jitterPoseBitsEqual(poses[i], clean[i])) << "pose " << i;
        }
    }
}

// a negative knob is not an invitation to jitter backwards by its magnitude
TEST(SplineJitterTests, NegativeKnobsAreTreatedAsZero){
    SplineTileFrameParams p = jitterGoldenParams();
    p.seed = 13u;
    p.spacingJitterMetres = -3.0f;
    p.yawJitterDegrees = -30.0f;
    p.lateralJitterMetres = -1.0f;
    p.dropoutChance = -0.5f;
    p.endTaperMetres = -10.0f;

    const std::vector<SplineLayoutPose> poses = jitterFrames(jitterGoldenScenario(), p);
    const std::vector<SplineLayoutPose> clean = jitterFrames(jitterGoldenScenario(), jitterGoldenParams());
    ASSERT_EQ(poses.size(), clean.size());
    for (size_t i = 0; i < poses.size(); i++){
        EXPECT_TRUE(jitterPoseBitsEqual(poses[i], clean[i])) << "pose " << i;
    }
}
