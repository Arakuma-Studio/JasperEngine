// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/Color.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;

TEST(Color, GammaIsTwoPointTwo){
    EXPECT_NEAR(Color::sRGBToLinear(0.5f, 0.5f, 0.5f).x, std::pow(0.5f, 2.2f), 1e-6f);
}

TEST(Color, BlackAndWhiteAreFixedPoints){
    EXPECT_EQ(Color::linearTosRGB(0, 0, 0), Vector3(0, 0, 0));
    EXPECT_EQ(Color::linearTosRGB(1, 1, 1), Vector3(1, 1, 1));
    EXPECT_EQ(Color::sRGBToLinear(1, 1, 1), Vector3(1, 1, 1));
}

TEST(Color, LinearToSRGBIsAGammaCurve){
    const Vector3 srgb = Color::linearTosRGB(0.5f, 0.25f, 0.1f);
    EXPECT_NEAR(srgb.x, std::pow(0.5f, 1.0f / 2.2f), 1e-5f);
    EXPECT_NEAR(srgb.y, std::pow(0.25f, 1.0f / 2.2f), 1e-5f);
    EXPECT_GT(srgb.z, 0.1f);
}

TEST(Color, RoundTripsBetweenSpaces){
    const Vector3 linear(0.2f, 0.4f, 0.8f);
    const Vector3 back = Color::sRGBToLinear(Color::linearTosRGB(linear));
    EXPECT_NEAR(back.x, linear.x, 1e-5f);
    EXPECT_NEAR(back.y, linear.y, 1e-5f);
    EXPECT_NEAR(back.z, linear.z, 1e-5f);
}

TEST(Color, AlphaIsLinearInBothDirections){
    EXPECT_FLOAT_EQ(Color::linearTosRGB(0.5f, 0.5f, 0.5f, 0.3f).w, 0.3f);
    EXPECT_FLOAT_EQ(Color::sRGBToLinear(Vector4(0.5f, 0.5f, 0.5f, 0.7f)).w, 0.7f);
}
