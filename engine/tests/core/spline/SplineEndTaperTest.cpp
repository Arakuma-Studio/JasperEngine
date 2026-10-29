// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineGeometry/Tests/SplineEndTaperTests.cpp, Copyright
// (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

// The end taper (S18 R6): off must be EXACTLY off (a run with no taper sweeps the same vertices to
// the bit), and on must actually reach nothing. Runs go -Z (SplineGeometryTestSupport.h).

#include "SplineGeometryTestSupport.h"
#include "spline/SplineEndTaper.h"

#include <cmath>
#include <limits>
#include <span>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::spline;
using namespace jasperengine::spline::geometrytest;

namespace{

    // a straight run along -Z, one station every `step` metres, both sides at `halfWidth`
    std::vector<SplineStation> taperStraightStations(uint32_t count, float step, float halfWidth){
        std::vector<SplineStation> stations(count);
        for (uint32_t i = 0; i < count; i++){
            const float along = step * static_cast<float>(i);
            SplineStation& s = stations[i];
            s.position = Vector3(0.0f, 5.0f, -along);
            s.forward = Vector3(0.0f, 0.0f, -1.0f);
            s.right = Vector3(1.0f, 0.0f, 0.0f);
            s.up = Vector3(0.0f, 1.0f, 0.0f);
            s.halfWidthLeft = halfWidth;
            s.halfWidthRight = halfWidth;
            s.distance = along;
        }
        return stations;
    }

    // every float a station carries that the taper may touch, compared by bits
    void expectTaperStationsBitIdentical(std::span<const SplineStation> actual, std::span<const SplineStation> expected,
                                         const char* what){
        ASSERT_EQ(actual.size(), expected.size()) << what;
        for (size_t i = 0; i < actual.size(); i++){
            EXPECT_TRUE(bitsEqual(actual[i].halfWidthLeft, expected[i].halfWidthLeft)) << what << ": station " << i << " left half-width moved";
            EXPECT_TRUE(bitsEqual(actual[i].halfWidthRight, expected[i].halfWidthRight)) << what << ": station " << i << " right half-width moved";
            EXPECT_TRUE(bitsEqual(actual[i].position, expected[i].position)) << what << ": station " << i << " moved";
            EXPECT_TRUE(bitsEqual(actual[i].distance, expected[i].distance)) << what << ": station " << i << " distance moved";
        }
    }

}

// an off taper is not a scale of one applied cheaply: it is no arithmetic at all
TEST(SplineEndTaperTests, AZeroTaperLeavesEveryStationBitIdentical){
    const std::vector<SplineStation> reference = taperStraightStations(12u, 2.3f, 3.7f);
    std::vector<SplineStation> stations = reference;

    SplineEndTaperParams params;
    params.taperMetres = 0.0f;
    params.heightDrop = 0.45f;
    applyEndTaper(stations, params);

    expectTaperStationsBitIdentical(stations, reference, "zero taper");
}

TEST(SplineEndTaperTests, AZeroTaperSweepsTheSameVerticesBitForBit){
    const SplineProfile profile = flatRibbon(4.5f);
    ASSERT_TRUE(profile.isValid());

    const std::vector<SplineStation> untouched = taperStraightStations(12u, 2.3f, 3.7f);
    std::vector<SplineStation> tapered = untouched;

    SplineEndTaperParams params;
    params.taperMetres = 0.0f;
    params.heightDrop = 0.45f;
    applyEndTaper(tapered, params);

    SplineStripParams stripParams;
    stripParams.widthScale = SplineProfileScale::LateralOnly;
    stripParams.uOriginMetres = 0.0f;
    const SplineStripMesh expected = buildSplineStrip(profile, untouched, stripParams);
    const SplineStripMesh actual = buildSplineStrip(profile, tapered, stripParams);

    ASSERT_TRUE(expected.isValid());
    ASSERT_EQ(actual.vertices.size(), expected.vertices.size());
    ASSERT_EQ(actual.indices, expected.indices);
    for (size_t i = 0; i < expected.vertices.size(); i++){
        EXPECT_TRUE(vertexBitsEqual(actual.vertices[i], expected.vertices[i])) << "vertex " << i;
    }
}

// the run must actually reach nothing: a pinch that stops short is a narrower square cut
TEST(SplineEndTaperTests, BothOpenEndsPinchToNothing){
    constexpr float halfWidth = 3.0f;
    std::vector<SplineStation> stations = taperStraightStations(41u, 1.0f, halfWidth);

    SplineEndTaperParams params;
    params.taperMetres = 8.0f;
    applyEndTaper(stations, params);

    EXPECT_FLOAT_EQ(stations.front().halfWidthLeft, 0.0f);
    EXPECT_FLOAT_EQ(stations.front().halfWidthRight, 0.0f);
    EXPECT_FLOAT_EQ(stations.back().halfWidthLeft, 0.0f);
    EXPECT_FLOAT_EQ(stations.back().halfWidthRight, 0.0f);
    // the middle of a 40 m run is nowhere near either 8 m taper
    EXPECT_FLOAT_EQ(stations[20].halfWidthLeft, halfWidth);
    EXPECT_FLOAT_EQ(stations[20].halfWidthRight, halfWidth);
}

// monotone in, monotone out: a pinch widening anywhere would read as a bulge at the river mouth
TEST(SplineEndTaperTests, TheTaperNarrowsMonotonicallyIntoEachEnd){
    constexpr float halfWidth = 3.0f;
    constexpr float taper = 8.0f;
    std::vector<SplineStation> stations = taperStraightStations(41u, 1.0f, halfWidth);

    SplineEndTaperParams params;
    params.taperMetres = taper;
    applyEndTaper(stations, params);

    for (size_t i = 0; i + 1u < stations.size(); i++){
        if (stations[i].distance < taper){
            EXPECT_LE(stations[i].halfWidthLeft, stations[i + 1u].halfWidthLeft) << "head taper widens backwards at station " << i;
        }
        if (stations.back().distance - stations[i + 1u].distance < taper){
            EXPECT_GE(stations[i].halfWidthLeft, stations[i + 1u].halfWidthLeft) << "tail taper widens forwards at station " << i;
        }
    }
    // every width stays inside the band it started in
    for (const SplineStation& s : stations){
        EXPECT_GE(s.halfWidthLeft, 0.0f);
        EXPECT_LE(s.halfWidthLeft, halfWidth);
    }
}

// smooth, not linear: a linear ramp arrives at the boundary with the taper's full slope, a
// smoothstep with none of it
TEST(SplineEndTaperTests, ThePinchMeetsTheRunWithoutACrease){
    constexpr float halfWidth = 3.0f;
    constexpr float taper = 8.0f;
    std::vector<SplineStation> stations = taperStraightStations(41u, 1.0f, halfWidth);

    SplineEndTaperParams params;
    params.taperMetres = taper;
    applyEndTaper(stations, params);

    // one metre inside the boundary a linear taper is at 7/8 of the width
    EXPECT_GT(stations[7].halfWidthLeft, halfWidth * (7.0f / 8.0f))
        << "the join is as steep as a linear ramp; smoothstep is what removes the crease";
    EXPECT_LT(stations[7].halfWidthLeft, halfWidth);
    // and the first step off zero is shallower than a linear ramp too
    EXPECT_LT(stations[1].halfWidthLeft, halfWidth * (1.0f / 8.0f));
}

// the pinch tucks under the surface it came from (along the station's up)
TEST(SplineEndTaperTests, ThePinchedRingSinksByTheHeightDrop){
    constexpr float drop = 0.35f;
    constexpr float taper = 8.0f;
    const std::vector<SplineStation> reference = taperStraightStations(41u, 1.0f, 3.0f);
    std::vector<SplineStation> stations = reference;

    SplineEndTaperParams params;
    params.taperMetres = taper;
    params.heightDrop = drop;
    applyEndTaper(stations, params);

    EXPECT_FLOAT_EQ(stations.front().position.y, reference.front().position.y - drop);
    EXPECT_FLOAT_EQ(stations.back().position.y, reference.back().position.y - drop);
    // untapered stations keep their draped height exactly
    EXPECT_FLOAT_EQ(stations[20].position.y, reference[20].position.y);
    // the sink follows the pinch, so a half-pinched ring is half-sunk at most
    EXPECT_GT(stations[4].position.y, reference[4].position.y - drop);
    EXPECT_LT(stations[4].position.y, reference[4].position.y);
    // and only along up: a level run's ring does not move along the run or across it
    EXPECT_EQ(stations.front().position.x, reference.front().position.x);
    EXPECT_EQ(stations.front().position.z, reference.front().position.z);
}

// a closed loop has no end to taper: pinching its seam would cut it open
TEST(SplineEndTaperTests, AClosedLoopIsLeftAlone){
    const std::vector<SplineStation> reference = taperStraightStations(12u, 2.3f, 3.7f);
    std::vector<SplineStation> stations = reference;

    SplineEndTaperParams params;
    params.taperMetres = 8.0f;
    params.heightDrop = 0.45f;
    params.closedLoop = true;
    applyEndTaper(stations, params);

    expectTaperStationsBitIdentical(stations, reference, "closed loop");
}

// on a run shorter than two tapers each station takes the nearer end, so the pinches meet
TEST(SplineEndTaperTests, AShortRunLetsItsTwoPinchesMeet){
    constexpr float halfWidth = 3.0f;
    std::vector<SplineStation> stations = taperStraightStations(11u, 1.0f, halfWidth); // 10 m against 8 m a side

    SplineEndTaperParams params;
    params.taperMetres = 8.0f;
    applyEndTaper(stations, params);

    EXPECT_FLOAT_EQ(stations.front().halfWidthLeft, 0.0f);
    EXPECT_FLOAT_EQ(stations.back().halfWidthLeft, 0.0f);
    for (const SplineStation& s : stations){
        EXPECT_GE(s.halfWidthLeft, 0.0f);
        EXPECT_LE(s.halfWidthLeft, halfWidth);
    }
    // the widest ring is the middle one, and it never reaches full width
    EXPECT_GT(stations[5].halfWidthLeft, stations[4].halfWidthLeft);
    EXPECT_GT(stations[5].halfWidthLeft, stations[6].halfWidthLeft);
    EXPECT_LT(stations[5].halfWidthLeft, halfWidth);
}

// an asymmetric channel carries its asymmetry into the point
TEST(SplineEndTaperTests, AnAsymmetricRunKeepsItsRatioThroughTheTaper){
    constexpr float left = 2.0f;
    constexpr float right = 6.0f;
    std::vector<SplineStation> stations = taperStraightStations(41u, 1.0f, 1.0f);
    for (SplineStation& s : stations){
        s.halfWidthLeft = left;
        s.halfWidthRight = right;
    }

    SplineEndTaperParams params;
    params.taperMetres = 8.0f;
    applyEndTaper(stations, params);

    for (size_t i = 1; i + 1u < stations.size(); i++){
        if (stations[i].halfWidthLeft <= 0.0f){
            continue;
        }
        EXPECT_NEAR(stations[i].halfWidthRight / stations[i].halfWidthLeft, right / left, 1.0e-4f)
            << "station " << i << " lost the channel's asymmetry";
    }
}

// a non-finite station keeps its width instead of taking a NaN one; its neighbours still taper
TEST(SplineEndTaperTests, ANonFiniteStationIsSkippedRatherThanScaledByANaN){
    std::vector<SplineStation> stations = taperStraightStations(41u, 1.0f, 3.0f);
    stations[3].distance = std::numeric_limits<float>::quiet_NaN();

    SplineEndTaperParams params;
    params.taperMetres = 8.0f;
    applyEndTaper(stations, params);

    EXPECT_FLOAT_EQ(stations[3].halfWidthLeft, 3.0f) << "an unplaceable station must keep its width";
    EXPECT_TRUE(std::isfinite(stations[3].halfWidthLeft));
    EXPECT_LT(stations[2].halfWidthLeft, 3.0f);
    EXPECT_LT(stations[4].halfWidthLeft, 3.0f);
}

// The swept taper ends in a point: the pinched end ring collapses onto its station, the band into
// it keeps one real triangle per profile edge, and the mesh stays well-formed (no zero-area
// triangle where the ring met itself).
TEST(SplineEndTaperTests, ATaperedRibbonSweepsToAPointWithoutDegenerateTriangles){
    std::vector<SplineStation> stations = taperStraightStations(31u, 0.5f, 2.0f);
    SplineEndTaperParams params;
    params.taperMetres = 4.0f;
    params.heightDrop = 0.1f;
    applyEndTaper(stations, params);

    SplineStripParams stripParams;
    stripParams.writeFlow = true;
    const SplineStripMesh mesh = buildSplineStrip(flatRibbon(4.0f), stations, stripParams);
    ASSERT_TRUE(mesh.isValid());
    for (uint32_t k = 0; k < mesh.ringVertexCount; k++){
        EXPECT_EQ(mesh.vertices[k].position, stations.front().position) << "slot " << k;
    }
    // two rings that collapse, one triangle each into them, two for the rest
    EXPECT_EQ(mesh.indices.size(), (static_cast<size_t>(mesh.ringCount - 1u) * 2u - 2u) * 3u);
    expectWellFormed(mesh, "tapered ribbon");
}
