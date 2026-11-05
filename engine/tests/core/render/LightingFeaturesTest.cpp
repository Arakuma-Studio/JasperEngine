// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S57's small pure parts (docs/specs/S57-forge-lighting.md): the tier defaults of Light Mode, the
// tile grid and contact shadows, Bend's light coordinate and sample groups, the OIT depth weight
// and the Hair shading model's block and variant.

#include "render/ContactShadows.h"
#include "device/Device.h"
#include "render/LightingTiers.h"
#include "render/OrderIndependentTransparency.h"
#include "texture/Material.h"
#include "texture/MaterialPbr.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

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

TEST(ContactShadows, ThicknessShareGrowsWithTheThickness){
    const Matrix4 projection = Matrix4::perspectiveMatrix(1.0f, 1.0f, 0.1f, 100.0f);
    const float thin = ContactShadows::thicknessShare(projection, 5.0f, 0.01f);
    const float thick = ContactShadows::thicknessShare(projection, 5.0f, 0.5f);
    EXPECT_GT(thin, 0.0f);
    EXPECT_GT(thick, thin);
    EXPECT_LE(thick, 1.0f);
}

TEST(OrderIndependentTransparency, DepthWeightFallsWithDistanceAndScale){
    const float nearWeight = OrderIndependentTransparency::depthWeight(1.0f, 1.0f);
    const float farWeight = OrderIndependentTransparency::depthWeight(100.0f, 1.0f);
    EXPECT_GT(nearWeight, farWeight);
    EXPECT_LE(nearWeight, 3e3f);
    EXPECT_GE(OrderIndependentTransparency::depthWeight(1e6f, 1.0f), 1e-2f);
    // a lower scale takes the weights of nearer surfaces (large scenes)
    EXPECT_GT(OrderIndependentTransparency::depthWeight(100.0f, 0.1f), farWeight);
    TransparencyMode mode;
    ASSERT_TRUE(OrderIndependentTransparency::modeFromName("order_independent", mode));
    EXPECT_EQ(mode, TransparencyMode::ORDER_INDEPENDENT);
    EXPECT_STREQ(OrderIndependentTransparency::getModeName(TransparencyMode::SORTED), "sorted");
}

TEST(OrderIndependentTransparency, SupportOverrideAnswers){
    const bool no = false;
    OrderIndependentTransparency::setSupportOverride(&no);
    EXPECT_FALSE(OrderIndependentTransparency::isSupported(1));
    const bool yes = true;
    OrderIndependentTransparency::setSupportOverride(&yes);
    EXPECT_TRUE(OrderIndependentTransparency::isSupported(4));
    OrderIndependentTransparency::setSupportOverride(nullptr);
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
