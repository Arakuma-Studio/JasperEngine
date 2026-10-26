// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Photometric light units and colour temperature (util/LightPhotometry.h). CPU only.

#include "util/LightPhotometry.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;

TEST(Photometry, Kelvin){
    const Vector3 white = LightPhotometry::kelvinToLinearRGB(6500.0f);
    EXPECT_NEAR(white.x, 1.0f, 0.03f);
    EXPECT_NEAR(white.y, 1.0f, 0.03f);
    EXPECT_NEAR(white.z, 1.0f, 0.03f);

    EXPECT_LT(LightPhotometry::kelvinToLinearRGB(1900.0f).z, 0.25f);

    float previousRatio = -1.0f;
    for (float kelvin = 1000.0f; kelvin <= 20000.0f; kelvin += 250.0f){
        const Vector3 color = LightPhotometry::kelvinToLinearRGB(kelvin);
        EXPECT_NEAR(std::max(color.x, std::max(color.y, color.z)), 1.0f, 1e-6f) << kelvin;
        const float ratio = color.z / color.x;
        // flat at 0 below 1900 K (no blue), rising from there
        EXPECT_GE(ratio, previousRatio) << kelvin;
        if (kelvin > 2000.0f) {
            EXPECT_GT(ratio, previousRatio) << kelvin;
        }
        previousRatio = ratio;
    }
    // out of range clamps
    EXPECT_EQ(LightPhotometry::kelvinToLinearRGB(500.0f), LightPhotometry::kelvinToLinearRGB(1000.0f));
    EXPECT_EQ(LightPhotometry::kelvinToLinearRGB(90000.0f), LightPhotometry::kelvinToLinearRGB(20000.0f));
}

TEST(Photometry, Sun){
    EXPECT_NEAR(LightPhotometry::toEngineIntensity(100000.0f, LightUnit::LUX, 15.0f), 2.543f, 0.01f);
    // engine units ignore the exposure
    EXPECT_EQ(LightPhotometry::toEngineIntensity(3.0f, LightUnit::ENGINE, 2.0f), 3.0f);
    // a lumen is spread over the sphere
    EXPECT_NEAR(LightPhotometry::toEngineIntensity(4.0f * 3.14159265f, LightUnit::LUMEN, 0.0f),
                LightPhotometry::toEngineIntensity(1.0f, LightUnit::CANDELA, 0.0f), 1e-6f);
}

TEST(Photometry, RoundTrip){
    for (float ev100 : {2.0f, 7.0f, 15.0f}){
        for (LightUnit unit : {LightUnit::ENGINE, LightUnit::LUX, LightUnit::CANDELA, LightUnit::LUMEN}){
            for (float value : {0.25f, 1.0f, 800.0f, 100000.0f}){
                const float engine = LightPhotometry::toEngineIntensity(value, unit, ev100);
                const float back = LightPhotometry::fromEngineIntensity(engine, unit, ev100);
                EXPECT_NEAR(back, value, value * 1e-5f) << (int)unit << " at EV100 " << ev100;
            }
        }
    }
}

TEST(Photometry, ResolveOnChange){
    LightComponent light;
    light.color = Vector3(0.5f, 0.25f, 1.0f);
    light.intensity = 2.0f;
    // engine units, no temperature: copied
    EXPECT_TRUE(LightPhotometry::resolveLight(light, 15.0f));
    EXPECT_EQ(light.resolvedColor, light.color);
    EXPECT_EQ(light.resolvedIntensity, 2.0f);
    // nothing changed: nothing done, the EV100 included (engine units ignore it)
    EXPECT_FALSE(LightPhotometry::resolveLight(light, 15.0f));
    EXPECT_FALSE(LightPhotometry::resolveLight(light, 8.0f));

    light.type = LightType::DIRECTIONAL;
    light.intensityUnit = LightUnit::LUX;
    light.intensity = 100000.0f;
    EXPECT_TRUE(LightPhotometry::resolveLight(light, 15.0f));
    EXPECT_NEAR(light.resolvedIntensity, 2.543f, 0.01f);
    EXPECT_FALSE(LightPhotometry::resolveLight(light, 15.0f));
    EXPECT_TRUE(LightPhotometry::resolveLight(light, 14.0f));
    EXPECT_NEAR(light.resolvedIntensity, 2.0f * 2.543f, 0.02f);

    light.useColorTemperature = true;
    light.colorTemperature = 2700.0f;
    EXPECT_TRUE(LightPhotometry::resolveLight(light, 14.0f));
    const Vector3 tungsten = LightPhotometry::kelvinToLinearRGB(2700.0f);
    EXPECT_NEAR(light.resolvedColor.y, light.color.y * tungsten.y, 1e-6f);
}

TEST(Photometry, UnitChangePreservesBrightness){
    LightComponent light;
    light.type = LightType::POINT;
    light.intensity = 3.0f;
    LightPhotometry::resolveLight(light, 12.0f);
    const float before = light.resolvedIntensity;

    for (LightUnit unit : {LightUnit::CANDELA, LightUnit::LUMEN, LightUnit::ENGINE}){
        LightPhotometry::setIntensityUnit(light, unit, true, 12.0f);
        EXPECT_EQ(light.intensityUnit, unit);
        LightPhotometry::resolveLight(light, 12.0f);
        EXPECT_NEAR(light.resolvedIntensity, before, before * 1e-5f) << (int)unit;
    }

    // lux is a directional light's: a point light takes candela, the same number
    LightPhotometry::setIntensityUnit(light, LightUnit::LUX, false, 12.0f);
    EXPECT_EQ(light.intensityUnit, LightUnit::CANDELA);
    EXPECT_EQ(LightPhotometry::validUnit(LightUnit::CANDELA, LightType::DIRECTIONAL), LightUnit::LUX);
    EXPECT_EQ(LightPhotometry::validUnit(LightUnit::LUMEN, LightType::DIRECTIONAL), LightUnit::LUX);
    EXPECT_EQ(LightPhotometry::validUnit(LightUnit::LUX, LightType::SPOT), LightUnit::CANDELA);
}
