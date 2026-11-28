// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S57's small pure parts (docs/specs/S57-forge-lighting.md): the tier defaults of Light Mode, the
// tile grid, Bend's contact-shadow light coordinate and sample groups, and the Hair shading
// model's block and variant; S99's Gradient ambient as SH (render/AmbientGradient.h).

#include "render/AmbientGradient.h"
#include "render/ContactShadows.h"
#include "device/Device.h"
#include "render/LightingTiers.h"
#include "texture/Material.h"
#include "texture/MaterialPbr.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace LightingFeaturesTest_cpp {

using namespace jasperengine;

TEST(LightingTiers, AutomaticIsTiledFromMediumAndPerObjectOnLow){
    EXPECT_FALSE(LightingTiers::usesTiledLights(LightMode::AUTOMATIC, LightingTier::LOW));
    EXPECT_TRUE(LightingTiers::usesTiledLights(LightMode::AUTOMATIC, LightingTier::MEDIUM));
    EXPECT_TRUE(LightingTiers::usesTiledLights(LightMode::AUTOMATIC, LightingTier::HIGH));
    EXPECT_TRUE(LightingTiers::usesTiledLights(LightMode::TILED, LightingTier::LOW));
    EXPECT_FALSE(LightingTiers::usesTiledLights(LightMode::PER_OBJECT, LightingTier::ULTRA));
    // the platforms' tiers until S56 resolves the device's: desktop High, web Medium, phones Low
    EXPECT_EQ(LightingTiers::defaultTier(4), LightingTier::HIGH);   // Linux
    EXPECT_EQ(LightingTiers::defaultTier(2), LightingTier::MEDIUM); // Web
    EXPECT_EQ(LightingTiers::defaultTier(3), LightingTier::LOW);    // Android
    int columns = 0;
    int rows = 0;
    LightingTiers::tileGrid(LightingTier::MEDIUM, columns, rows);
    EXPECT_EQ(columns, 16);
    EXPECT_EQ(rows, 9);
    LightingTiers::tileGrid(LightingTier::HIGH, columns, rows);
    EXPECT_EQ(columns, 32);
    EXPECT_EQ(rows, 18);
    EXPECT_EQ(LightingTiers::contactShadowDivisor(LightingTier::HIGH), 1);
    EXPECT_EQ(LightingTiers::contactShadowDivisor(LightingTier::MEDIUM), 2);
    EXPECT_EQ(LightingTiers::contactShadowDivisor(LightingTier::LOW), 0);
    LightMode mode;
    ASSERT_TRUE(LightingTiers::modeFromName("per_object", mode));
    EXPECT_EQ(mode, LightMode::PER_OBJECT);
    EXPECT_STREQ(LightingTiers::getModeName(LightMode::TILED), "tiled");
    EXPECT_FALSE(LightingTiers::modeFromName("forward", mode));
}

// S56: the device's tier decides where it applies; setTier still wins
TEST(LightingTiers, FollowTheDeviceTier){
    LightingTiers::resetTier();
    Device::setTier(DeviceTier::Low);
    EXPECT_EQ(LightingTiers::getTier(), LightingTier::LOW);
    EXPECT_FALSE(LightingTiers::usesTiledLights());
    Device::setTier(DeviceTier::Ultra);
    EXPECT_EQ(LightingTiers::getTier(), LightingTier::ULTRA);
    LightingTiers::setTier(LightingTier::MEDIUM);
    EXPECT_EQ(LightingTiers::getTier(), LightingTier::MEDIUM);
    LightingTiers::resetTier();
    Device::resetTier();
}

TEST(ContactShadows, LightCoordinateIsBendsInNdc){
    // a light in front: its NDC, its packed depth, +1
    const Vector4 front = ContactShadows::lightCoordinate(Vector4(1.0f, -2.0f, 3.0f, 4.0f));
    EXPECT_FLOAT_EQ(front.x, 0.25f);
    EXPECT_FLOAT_EQ(front.y, -0.5f);
    EXPECT_FLOAT_EQ(front.z, 0.5f * 0.75f + 0.5f);
    EXPECT_FLOAT_EQ(front.w, 1.0f);
    // behind the camera: -1
    EXPECT_FLOAT_EQ(ContactShadows::lightCoordinate(Vector4(0.0f, 0.0f, 1.0f, -2.0f)).w, -1.0f);
    // parallel to the view: w kept off zero, the coordinate finite
    const Vector4 parallel = ContactShadows::lightCoordinate(Vector4(1.0f, 0.0f, 1.0f, 0.0f));
    EXPECT_TRUE(std::isfinite(parallel.x));
    EXPECT_GT(std::abs(parallel.x), 1000.0f);
}

TEST(ContactShadows, SampleGroupsKeepFourAveragedSamples){
    for (int samples = ContactShadows::MIN_SAMPLES; samples <= ContactShadows::MAX_SAMPLES; samples++){
        int hard = 0;
        int fade = 0;
        ContactShadows::sampleGroups(samples, hard, fade);
        EXPECT_GE(hard, 2);
        EXPECT_GE(samples - hard - fade, 4) << samples;
    }
    int hard = 0;
    int fade = 0;
    ContactShadows::sampleGroups(100, hard, fade);
    EXPECT_EQ(hard, 4);
}

TEST(HairShadingModel, ItsBlockAndVariant){
    Material material;
    MaterialPbrExtensions& pbr = material.pbr.edit();
    EXPECT_EQ(MaterialPbr::getShadingModel(pbr), ShadingModel::STANDARD);
    MaterialPbr::setShadingModel(pbr, ShadingModel::HAIR);
    pbr.hair.primaryColor = Vector3(0.9f, 0.8f, 0.7f);
    pbr.hair.primaryShift = 0.2f;
    pbr.hair.primaryWidth = 0.1f;
    pbr.hair.secondaryWidth = 0.5f;
    pbr.hair.rotation = 1.5707963f;
    EXPECT_TRUE(MaterialPbr::isLobeEnabled(pbr, PbrLobe::HAIR));

    float block[MaterialPbr::HAIR_UNIFORM_FLOATS];
    MaterialPbr::getHairUniformBlock(pbr, block);
    EXPECT_FLOAT_EQ(block[0], 0.9f);
    EXPECT_FLOAT_EQ(block[3], 0.2f);
    EXPECT_FLOAT_EQ(block[8], 2.0f / 0.01f - 2.0f);
    EXPECT_FLOAT_EQ(block[9], 2.0f / 0.25f - 2.0f);
    EXPECT_NEAR(block[10], 0.0f, 1e-5f);
    EXPECT_NEAR(block[11], 1.0f, 1e-5f);

    // a pixel-lit variant takes PBR_HAIR, a vertex-lit one none
    const PbrVariant variant = MaterialPbr::getPbrVariant(material, MaterialLighting::PIXEL, false, PbrTier::FULL);
    EXPECT_NE(std::find(variant.defines.begin(), variant.defines.end(), "PBR_HAIR"), variant.defines.end());
    EXPECT_TRUE(MaterialPbr::getPbrVariant(material, MaterialLighting::VERTEX, false, PbrTier::FULL).empty());
    // its basename code, read back
    std::vector<std::string> defines;
    ASSERT_TRUE(MaterialPbr::parseVariantBasename(MaterialPbr::getVariantBasename({"PBR_HAIR"}), defines));
    EXPECT_EQ(defines, std::vector<std::string>{"PBR_HAIR"});
    // the field table names its values (serialization, MCP, Lua)
    EXPECT_NE(MaterialPbr::findField("hair.primaryWidth"), nullptr);
    EXPECT_NE(MaterialPbr::findLuaField("hairSecondaryShift"), nullptr);
}

// S99 AC7: the Gradient ambient's SH is the gradient environment projected to bands 0 and 1 and
// convolved with the clamped cosine, divided by pi: checked against a numeric projection
TEST(AmbientGradient, ProjectionMatchesANumericIntegral){
    const Vector3 sky(0.9f, 0.6f, 0.3f);
    const Vector3 equator(0.2f, 0.4f, 0.5f);
    const Vector3 ground(0.05f, 0.1f, 0.7f);
    Vector4 sh[3];
    AmbientGradient::project(sky, equator, ground, sh);

    // L00 and L1 (y) of each channel, midpoint rule in (cos theta, phi): uniform in solid angle
    const float y00 = 0.282095f;
    const float y1 = 0.488603f;
    const int steps = 2000;
    double l00[3] = {0.0, 0.0, 0.0};
    double l1y[3] = {0.0, 0.0, 0.0};
    const double dOmega = 4.0 * 3.14159265358979 / steps;
    for (int i = 0; i < steps; i++){
        const float y = -1.0f + (2.0f * (float)i + 1.0f) / (float)steps;
        // the gradient is symmetric about the up axis: phi integrates to 2 pi (and the x, z bands to 0)
        const Vector3 radiance = y >= 0.0f ? equator + (sky - equator) * y : equator + (ground - equator) * (-y);
        for (int c = 0; c < 3; c++){
            l00[c] += radiance[c] * y00 * dOmega;
            l1y[c] += radiance[c] * y1 * y * dOmega;
        }
    }
    const Vector3 normals[] = {Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f),
                               Vector3(0.6f, 0.8f, 0.0f), Vector3(0.0f, -0.6f, 0.8f)};
    for (const Vector3& n : normals){
        for (int c = 0; c < 3; c++){
            // E(n) / pi = L00 Y00 + (2 / 3) L1 Y1(n)
            const double expected = l00[c] * y00 + (2.0 / 3.0) * l1y[c] * y1 * n.y;
            const float shaded = sh[c].x * n.x + sh[c].y * n.y + sh[c].z * n.z + sh[c].w;
            EXPECT_NEAR(shaded, expected, 1e-3) << "channel " << c << " normal " << n.x << "," << n.y << "," << n.z;
        }
    }
    // the flat consumers' average is the constant term
    const Vector3 average = AmbientGradient::average(sky, equator, ground);
    for (int c = 0; c < 3; c++)
        EXPECT_FLOAT_EQ(average[c], sh[c].w);

    // one colour everywhere is the flat ambient: the same light at every facing
    AmbientGradient::project(Vector3(0.3f, 0.3f, 0.3f), Vector3(0.3f, 0.3f, 0.3f), Vector3(0.3f, 0.3f, 0.3f), sh);
    for (int c = 0; c < 3; c++){
        EXPECT_FLOAT_EQ(sh[c].w, 0.3f);
        EXPECT_FLOAT_EQ(sh[c].x, 0.0f);
        EXPECT_NEAR(sh[c].y, 0.0f, 1e-7f);
        EXPECT_FLOAT_EQ(sh[c].z, 0.0f);
    }
}

}  // namespace LightingFeaturesTest_cpp
