// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineLayout/Tests/CenterlineSamplingTests.cpp,
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

// The dense draped centerline the layouts walk: the pinned sample step and its budget, the
// local-to-world arc length, the conform-gap hold and the station conversion.

#include "SplineCenterline.h"

#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    // Metres actually covered per sample once the count is fixed: the quantity that must not
    // drift with path length, or the measured drape and the piece count fall with it.
    float centerlineRealizedStep(float worldLength){
        return worldLength / static_cast<float>(centerlineSampleCount(worldLength) - 1u);
    }

    Matrix4 centerlineRotationY(float radians){
        Matrix4 m;
        m.set(0, 0, std::cos(radians));
        m.set(0, 2, -std::sin(radians));
        m.set(2, 0, std::sin(radians));
        m.set(2, 2, std::cos(radians));
        return m;
    }

    SplineCenterSample centerlineGapSample(float z, float y, bool measured, const Vector3& normal = Vector3(0.0f, 1.0f, 0.0f)){
        SplineCenterSample s;
        s.position = Vector3(0.0f, y, -z);
        s.normal = normal;
        s.surfaceValid = measured;
        return s;
    }

}

TEST(SplineCenterlineTests, StepStaysPinnedAcrossEveryLengthInBudget){
    for (const float length : {1.0f, 9.0f, 40.0f, 250.0f, 1023.0f, 1024.0f, 2000.0f, 4000.0f, SPLINE_MAX_CENTERLINE_LENGTH}){
        EXPECT_LE(centerlineRealizedStep(length), SPLINE_CENTERLINE_STEP + 1.0e-4f) << "length " << length;
    }
}

TEST(SplineCenterlineTests, CountFollowsTheStepBelowTheBudget){
    EXPECT_EQ(centerlineSampleCount(9.0f), 20u);  //  9 / 0.5 + 2
    EXPECT_EQ(centerlineSampleCount(40.0f), 82u); // 40 / 0.5 + 2
    EXPECT_EQ(centerlineSampleCount(2000.0f), 4002u);
}

TEST(SplineCenterlineTests, TheBudgetIsTheOnlyCeilingAndItIsExplicit){
    EXPECT_EQ(SPLINE_MAX_CENTERLINE_SAMPLES, static_cast<uint32_t>(SPLINE_MAX_CENTERLINE_LENGTH / SPLINE_CENTERLINE_STEP) + 2u);
    EXPECT_EQ(centerlineSampleCount(SPLINE_MAX_CENTERLINE_LENGTH), SPLINE_MAX_CENTERLINE_SAMPLES);
    EXPECT_EQ(centerlineSampleCount(SPLINE_MAX_CENTERLINE_LENGTH * 2.0f), SPLINE_MAX_CENTERLINE_SAMPLES);
    // past the budget the step does coarsen (the state the caller warns about), never before
    EXPECT_GT(centerlineRealizedStep(SPLINE_MAX_CENTERLINE_LENGTH * 2.0f), SPLINE_CENTERLINE_STEP);
}

TEST(SplineCenterlineTests, DegenerateLengthsDegradeToTheTwoSampleMinimum){
    EXPECT_EQ(centerlineSampleCount(0.0f), 2u);
    EXPECT_EQ(centerlineSampleCount(-5.0f), 2u);
    EXPECT_EQ(centerlineSampleCount(std::numeric_limits<float>::quiet_NaN()), 2u);
    EXPECT_EQ(centerlineSampleCount(std::numeric_limits<float>::infinity()), SPLINE_MAX_CENTERLINE_SAMPLES);
    EXPECT_EQ(centerlineSampleCount(1.0e30f), SPLINE_MAX_CENTERLINE_SAMPLES);
}

TEST(SplineCenterlineTests, ArcLengthIsScaledOutOfEntityLocalSpace){
    // spline arc lengths are entity-local, the drape and its step world metres: a scaled spline
    // entity must not change how finely the drape is sampled in world space
    EXPECT_NEAR(worldCenterlineLength(40.0f, Matrix4()), 40.0f, 1.0e-3f);

    const Matrix4 uniform = Matrix4::scaleMatrix(Vector3(3.0f, 3.0f, 3.0f));
    EXPECT_NEAR(worldCenterlineLength(40.0f, uniform), 120.0f, 1.0e-3f);
    EXPECT_EQ(centerlineSampleCount(worldCenterlineLength(40.0f, uniform)), 242u);

    // non-uniform: the largest axis wins, so the world step is never coarser than asked for
    const Matrix4 stretched = Matrix4::scaleMatrix(Vector3(2.0f, 1.0f, 0.5f));
    EXPECT_NEAR(worldCenterlineLength(40.0f, stretched), 80.0f, 1.0e-3f);

    // rotation and translation carry no scale
    EXPECT_NEAR(worldCenterlineLength(40.0f, centerlineRotationY(0.7f)), 40.0f, 1.0e-3f);
    const Matrix4 translated = Matrix4::translateMatrix(Vector3(500.0f, 12.0f, -7.0f));
    EXPECT_NEAR(worldCenterlineLength(40.0f, translated), 40.0f, 1.0e-3f);
    EXPECT_NEAR(largestAxisScale(translated * stretched), 2.0f, 1.0e-5f);
}

// ---- The shared conform-miss rule (S18 R9) -------------------------------------------------

TEST(SplineCenterlineTests, HoldSurfaceBackfillsALeadingGapFromTheFirstMeasured){
    const Vector3 groundN(0.0f, 0.6f, -0.8f);
    std::vector<SplineCenterSample> line{centerlineGapSample(0.0f, 9.0f, false), centerlineGapSample(1.0f, 9.0f, false),
                                         centerlineGapSample(2.0f, 2.0f, true, groundN), centerlineGapSample(3.0f, 3.0f, true)};
    holdSurfaceAcrossGaps(line);

    for (size_t i = 0; i < 2; i++){
        EXPECT_NEAR(line[i].position.y, 2.0f, 1.0e-6f) << "sample " << i;
        EXPECT_NEAR(line[i].normal.y, groundN.y, 1.0e-6f) << "sample " << i;
        EXPECT_NEAR(line[i].normal.z, groundN.z, 1.0e-6f) << "sample " << i;
    }
    // measured samples, every XZ, and the flags themselves are untouched
    EXPECT_NEAR(line[2].position.y, 2.0f, 1.0e-6f);
    EXPECT_NEAR(line[3].position.y, 3.0f, 1.0e-6f);
    for (size_t i = 0; i < line.size(); i++){
        EXPECT_NEAR(line[i].position.z, -static_cast<float>(i), 1.0e-6f) << "sample " << i;
    }
    EXPECT_FALSE(line[0].surfaceValid);
    EXPECT_FALSE(line[1].surfaceValid);
    EXPECT_TRUE(line[2].surfaceValid);
}

TEST(SplineCenterlineTests, HoldSurfaceInteriorGapHoldsTheRunBeforeIt){
    std::vector<SplineCenterSample> line{centerlineGapSample(0.0f, 1.0f, true), centerlineGapSample(1.0f, 9.0f, false),
                                         centerlineGapSample(2.0f, 9.0f, false), centerlineGapSample(3.0f, 4.0f, true)};
    holdSurfaceAcrossGaps(line);

    EXPECT_NEAR(line[0].position.y, 1.0f, 1.0e-6f);
    EXPECT_NEAR(line[1].position.y, 1.0f, 1.0e-6f); // holds the run before it, and
    EXPECT_NEAR(line[2].position.y, 1.0f, 1.0e-6f); // the hold cascades through the run
    EXPECT_NEAR(line[3].position.y, 4.0f, 1.0e-6f); // the next measurement is its own
}

TEST(SplineCenterlineTests, HoldSurfaceTrailingGapHoldsTheLastMeasured){
    std::vector<SplineCenterSample> line{centerlineGapSample(0.0f, 1.5f, true), centerlineGapSample(1.0f, 9.0f, false),
                                         centerlineGapSample(2.0f, 9.0f, false)};
    holdSurfaceAcrossGaps(line);

    EXPECT_NEAR(line[1].position.y, 1.5f, 1.0e-6f);
    EXPECT_NEAR(line[2].position.y, 1.5f, 1.0e-6f);
}

TEST(SplineCenterlineTests, HoldSurfaceAllMissLeavesTheAuthoredPolylineUntouched){
    std::vector<SplineCenterSample> line{centerlineGapSample(0.0f, 5.0f, false), centerlineGapSample(1.0f, 6.0f, false),
                                         centerlineGapSample(2.0f, 7.0f, false)};
    const std::vector<SplineCenterSample> before = line;
    holdSurfaceAcrossGaps(line);

    for (size_t i = 0; i < line.size(); i++){
        EXPECT_NEAR(line[i].position.y, before[i].position.y, 1.0e-6f) << "sample " << i;
        EXPECT_NEAR(line[i].normal.y, before[i].normal.y, 1.0e-6f) << "sample " << i;
        EXPECT_FALSE(line[i].surfaceValid) << "sample " << i;
    }
}

TEST(SplineCenterlineTests, StationsBecomeMeasuredSamplesOnTheirOwnUp){
    std::vector<SplineStation> stations(3);
    for (size_t i = 0; i < stations.size(); i++){
        stations[i].position = Vector3(1.0f, 2.0f, -static_cast<float>(i));
        stations[i].up = Vector3(0.0f, 0.8f, 0.6f);
    }
    std::vector<SplineCenterSample> center(7);
    centerlineFromStations(stations, center);
    ASSERT_EQ(center.size(), stations.size());
    for (size_t i = 0; i < center.size(); i++){
        EXPECT_EQ(center[i].position, stations[i].position);
        EXPECT_EQ(center[i].normal, stations[i].up);
        EXPECT_TRUE(center[i].surfaceValid);
    }
}
