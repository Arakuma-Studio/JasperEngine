// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Engine.h"
#include "util/Angle.h"

#include <gtest/gtest.h>

using namespace jasperengine;

namespace {
    const float pi = 3.14159265f;

    // Engine::setUseDegrees is global: every test leaves it as it found it
    class AngleTest : public ::testing::Test {
    protected:
        bool degrees = Engine::isUseDegrees();
        void TearDown() override{
            Engine::setUseDegrees(degrees);
        }
    };
}

TEST_F(AngleTest, DegreesAreTheDefaultUnit){
    EXPECT_TRUE(degrees);
}

TEST_F(AngleTest, ConvertsBetweenDegreesAndRadians){
    EXPECT_NEAR(Angle::degToRad(180.0f), pi, 1e-5f);
    EXPECT_NEAR(Angle::radToDeg(pi / 2.0f), 90.0f, 1e-4f);
    EXPECT_NEAR(Angle::radToDeg(Angle::degToRad(37.5f)), 37.5f, 1e-4f);
}

TEST_F(AngleTest, DefaultUnitInDegreesMode){
    Engine::setUseDegrees(true);
    EXPECT_FLOAT_EQ(Angle::degToDefault(45.0f), 45.0f);
    EXPECT_NEAR(Angle::radToDefault(pi), 180.0f, 1e-4f);
    EXPECT_NEAR(Angle::defaultToRad(90.0f), pi / 2.0f, 1e-5f);
    EXPECT_FLOAT_EQ(Angle::defaultToDeg(30.0f), 30.0f);
}

TEST_F(AngleTest, DefaultUnitInRadiansMode){
    Engine::setUseDegrees(false);
    EXPECT_NEAR(Angle::degToDefault(180.0f), pi, 1e-5f);
    EXPECT_FLOAT_EQ(Angle::radToDefault(1.5f), 1.5f);
    EXPECT_FLOAT_EQ(Angle::defaultToRad(1.5f), 1.5f);
    EXPECT_NEAR(Angle::defaultToDeg(pi), 180.0f, 1e-4f);
}
