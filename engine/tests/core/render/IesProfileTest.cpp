// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The LM-63 reader of IES Profiles (render/IesProfile.h, spec S99 R36, AC8): both versions, each
// symmetry of the horizontal angles, the grid's normalisation, and the files it refuses. Every
// file here is synthetic, written for the test.

#include <gtest/gtest.h>

#include "render/IesProfile.h"

#include <cmath>
#include <string>

namespace IesProfileTest_cpp {

using namespace jasperengine;

namespace {
    // 2002, rotationally symmetric, downward only: 200 * cos(theta) candela with a multiplier 2
    const char* kRotational2002 =
        "IESNA:LM-63-2002\n"
        "[TEST] synthetic\n"
        "[MANUFAC] Jasper Test Lights\n"
        "[LUMINAIRE] Cosine Downlight\n"
        "[LAMP] LED 10W\n"
        "TILT=NONE\n"
        "1 1000 2.0 4 1 1 2 0.1 0.1 0.0\n"
        "1.0 1.0 10\n"
        "0 30 60 90\n"
        "0\n"
        "100 86.6025 50 0\n";

    // 1995, quadrant symmetric: the 0, 45 and 90 degree planes differ
    const char* kQuadrant1995 =
        "IESNA:LM-63-1995\n"
        "[MANUFAC] Jasper Test Lights\n"
        "[LUMCAT] QUAD-1\n"
        "[LAMPCAT] HALO\n"
        "TILT=NONE\n"
        "2, 500, 1, 3, 3, 1, 2, 0, 0, 0\n"
        "1 1 50\n"
        "0 45 90\n"
        "0 45 90\n"
        "100 80 0\n"
        "100 60 0\n"
        "100 40 0\n";

    std::string bilateral(){
        return "IESNA:LM-63-2002\nTILT=NONE\n1 -1 1 2 3 1 2 0 0 0\n1 1 0\n0 90\n0 90 180\n10 10\n20 20\n40 40\n";
    }

    // no symmetry, the planes every 30 degrees up to 330 (360 is 0 again)
    std::string full(){
        std::string s = "IESNA:LM-63-2002\nTILT=NONE\n1 -1 1 2 12 1 2 0 0 0\n1 1 0\n0 180\n";
        for (int i = 0; i < 12; i++) s += std::to_string(i * 30) + " ";
        s += "\n";
        for (int i = 0; i < 12; i++) s += std::to_string(10 + i * 10) + " " + std::to_string(10 + i * 10) + "\n";
        return s;
    }
}

TEST(IesProfile, Reads2002RotationalAndNormalisesToItsPeak){
    IesProfile profile;
    std::string error;
    ASSERT_TRUE(IesProfile::parse(kRotational2002, profile, error)) << error;
    EXPECT_EQ(profile.format, "LM-63-2002");
    EXPECT_EQ(profile.manufacturer, "Jasper Test Lights");
    EXPECT_EQ(profile.luminaire, "Cosine Downlight");
    EXPECT_EQ(profile.lamp, "LED 10W");
    EXPECT_EQ(profile.symmetry, IesProfile::Symmetry::ROTATIONAL);
    EXPECT_FLOAT_EQ(profile.peakCandela, 200.0f);
    EXPECT_FLOAT_EQ(profile.lumens, 1000.0f);
    ASSERT_EQ((int)profile.grid.size(), IesProfile::GRID_ROWS * IesProfile::GRID_COLUMNS);
    EXPECT_NEAR(profile.sample(0.0f, 0.0f), 1.0f, 1e-5f);
    EXPECT_NEAR(profile.sample(60.0f, 123.0f), 0.5f, 0.01f);
    EXPECT_NEAR(profile.sample(30.0f, 270.0f), 0.866f, 0.01f);
    // upward of a downlight: nothing
    EXPECT_EQ(profile.sample(120.0f, 0.0f), 0.0f);
    float peak = 0.0f;
    for (float v : profile.grid) peak = std::max(peak, v);
    EXPECT_FLOAT_EQ(peak, 1.0f);
}

TEST(IesProfile, Reads1995QuadrantSymmetry){
    IesProfile profile;
    std::string error;
    ASSERT_TRUE(IesProfile::parse(kQuadrant1995, profile, error)) << error;
    EXPECT_EQ(profile.format, "LM-63-1995");
    EXPECT_EQ(profile.luminaire, "QUAD-1");
    EXPECT_EQ(profile.lamp, "HALO");
    EXPECT_EQ(profile.symmetry, IesProfile::Symmetry::QUADRANT);
    EXPECT_FLOAT_EQ(profile.lumens, 1000.0f);
    // each quadrant mirrors the first
    EXPECT_NEAR(profile.sampleSource(45.0f, 0.0f), 0.8f, 1e-5f);
    EXPECT_NEAR(profile.sampleSource(45.0f, 90.0f), 0.4f, 1e-5f);
    EXPECT_NEAR(profile.sampleSource(45.0f, 135.0f), profile.sampleSource(45.0f, 45.0f), 1e-5f);
    EXPECT_NEAR(profile.sampleSource(45.0f, 180.0f), 0.8f, 1e-5f);
    EXPECT_NEAR(profile.sampleSource(45.0f, 270.0f), 0.4f, 1e-5f);
    EXPECT_NEAR(profile.sampleSource(45.0f, 315.0f), profile.sampleSource(45.0f, 45.0f), 1e-5f);
    // the grid's rows fall on 0, 90, 180, 270 degrees (every 8th of 32 intervals)
    EXPECT_NEAR(profile.sample(45.0f, 90.0f), 0.4f, 0.01f);
    EXPECT_NEAR(profile.sample(45.0f, 180.0f), 0.8f, 0.01f);
}

TEST(IesProfile, ReadsBilateralAndFullSymmetries){
    IesProfile profile;
    std::string error;
    ASSERT_TRUE(IesProfile::parse(bilateral(), profile, error)) << error;
    EXPECT_EQ(profile.symmetry, IesProfile::Symmetry::BILATERAL);
    EXPECT_NEAR(profile.sampleSource(10.0f, 300.0f), profile.sampleSource(10.0f, 60.0f), 1e-5f);
    EXPECT_NEAR(profile.sampleSource(10.0f, 180.0f), 1.0f, 1e-5f);
    // absolute photometry (-1 lumens a lamp): the flux comes from the candela
    EXPECT_GT(profile.lumens, 0.0f);

    ASSERT_TRUE(IesProfile::parse(full(), profile, error)) << error;
    EXPECT_EQ(profile.symmetry, IesProfile::Symmetry::FULL);
    EXPECT_NEAR(profile.sampleSource(90.0f, 330.0f), 1.0f, 1e-5f);
    // between 330 and 360 (= 0): halfway from the last plane back to the first
    EXPECT_NEAR(profile.sampleSource(90.0f, 345.0f), (1.0f + 10.0f / 120.0f) * 0.5f, 1e-4f);

    const std::string bilateral90 = "IESNA:LM-63-2002\nTILT=NONE\n1 -1 1 2 3 1 2 0 0 0\n1 1 0\n0 90\n90 180 270\n10 10\n20 20\n40 40\n";
    ASSERT_TRUE(IesProfile::parse(bilateral90, profile, error)) << error;
    EXPECT_EQ(profile.symmetry, IesProfile::Symmetry::BILATERAL_90);
    // mirrored across the 90-270 plane: 0 reads 180, 45 reads 135
    EXPECT_NEAR(profile.sampleSource(10.0f, 0.0f), profile.sampleSource(10.0f, 180.0f), 1e-5f);
    EXPECT_NEAR(profile.sampleSource(10.0f, 45.0f), profile.sampleSource(10.0f, 135.0f), 1e-5f);
}

TEST(IesProfile, IntegratesAbsolutePhotometry){
    // an isotropic 100 cd source over the whole sphere: 400 pi lumens
    const std::string isotropic = "IESNA:LM-63-2002\nTILT=NONE\n1 -1 1 2 1 1 2 0 0 0\n1 1 0\n0 180\n0\n100 100\n";
    IesProfile profile;
    std::string error;
    ASSERT_TRUE(IesProfile::parse(isotropic, profile, error)) << error;
    EXPECT_NEAR(profile.lumens, 400.0f * 3.14159265f, 400.0f * 3.14159265f * 0.01f);
}

TEST(IesProfile, RefusesTiltAndOtherPhotometry){
    IesProfile profile;
    std::string error;
    EXPECT_FALSE(IesProfile::parse("IESNA:LM-63-2002\nTILT=INCLUDE\n1\n0\n1\n1\n", profile, error));
    EXPECT_NE(error.find("TILT=INCLUDE"), std::string::npos) << error;

    EXPECT_FALSE(IesProfile::parse("IESNA:LM-63-1995\nTILT=lamp.tlt\n", profile, error));
    EXPECT_NE(error.find("tilt file"), std::string::npos) << error;
    EXPECT_NE(error.find("lamp.tlt"), std::string::npos) << error;

    EXPECT_FALSE(IesProfile::parse("IESNA:LM-63-2002\nTILT=NONE\n1 -1 1 2 1 2 2 0 0 0\n1 1 0\n0 90\n0\n1 1\n", profile, error));
    EXPECT_NE(error.find("type B"), std::string::npos) << error;

    EXPECT_FALSE(IesProfile::parse("IESNA:LM-63-2002\nTILT=NONE\n1 -1 1 4 1 1 2 0 0 0\n1 1 0\n0 30 60 90\n0\n1 1\n", profile, error));
    EXPECT_NE(error.find("expected"), std::string::npos) << error;

    EXPECT_FALSE(IesProfile::parse("not a photometric file\n", profile, error));
    EXPECT_NE(error.find("TILT"), std::string::npos) << error;
}

}  // namespace IesProfileTest_cpp
