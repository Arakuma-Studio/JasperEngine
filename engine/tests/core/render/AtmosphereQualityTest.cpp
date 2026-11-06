// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The atmosphere's quality tiers (docs/specs/S12-atmosphere.md R11), its presets (AC13), the
// project's per-platform defaults and the runtime override (Engine), and the mesh variants a
// height fog adds without touching the distance fogs' (AC1).

#include "render/AtmosphereQuality.h"
#include "render/AtmospherePresets.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/AtmosphereRenderer.h"
#include "pool/ShaderPool.h"
#include "Engine.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace jasperengine;

TEST(AtmosphereTiers, MatchTheSpecTable){
    const AtmosphereTier off = AtmosphereTiers::get(AtmosphereQuality::OFF);
    EXPECT_EQ(off.fogSteps, 0);
    EXPECT_EQ(off.cloudSteps, 0);

    const AtmosphereTier low = AtmosphereTiers::get(AtmosphereQuality::LOW);
    EXPECT_EQ(low.fogDivisor, 4);
    EXPECT_EQ(low.fogSteps, 16);
    EXPECT_EQ(low.fogLocalLights, 0);
    EXPECT_EQ(low.fogVolumes, 2);
    EXPECT_EQ(low.cloudsAuto, CloudsRendering::SKY_MAP);
    EXPECT_EQ(low.skyMapSize, 256);
    EXPECT_EQ(low.skyMapSliceFrames, 8);
    EXPECT_EQ(low.cloudSteps, 32);
    EXPECT_EQ(low.cloudLightSteps, 3);
    EXPECT_FALSE(low.cloudErosion);

    const AtmosphereTier medium = AtmosphereTiers::get(AtmosphereQuality::MEDIUM);
    EXPECT_EQ(medium.fogDivisor, 2);
    EXPECT_EQ(medium.fogSteps, 32);
    EXPECT_EQ(medium.fogVolumes, 4);
    EXPECT_EQ(medium.skyMapSize, 512);
    EXPECT_EQ(medium.skyMapSliceFrames, 4);
    EXPECT_EQ(medium.cloudSteps, 64);
    EXPECT_EQ(medium.cloudLightSteps, 4);
    EXPECT_TRUE(medium.cloudErosion);

    const AtmosphereTier high = AtmosphereTiers::get(AtmosphereQuality::HIGH);
    EXPECT_EQ(high.fogDivisor, 2);
    EXPECT_EQ(high.fogSteps, 64);
    EXPECT_EQ(high.fogLocalLights, 4);
    EXPECT_EQ(high.fogVolumes, 8);
    EXPECT_EQ(high.cloudsAuto, CloudsRendering::SCREEN);
    EXPECT_EQ(high.cloudScreenDivisor, 4);
    EXPECT_EQ(high.cloudSteps, 96);
    EXPECT_EQ(high.cloudLightSteps, 6);

    // the shaders' loop bounds hold every tier
    for (AtmosphereQuality quality : {AtmosphereQuality::LOW, AtmosphereQuality::MEDIUM, AtmosphereQuality::HIGH}){
        const AtmosphereTier tier = AtmosphereTiers::get(quality);
        EXPECT_LE(tier.fogSteps, ATMOSPHERE_MAX_FOG_STEPS);
        EXPECT_LE(tier.fogVolumes, ATMOSPHERE_MAX_FOG_VOLUMES);
        EXPECT_LE(tier.fogLocalLights, ATMOSPHERE_MAX_FOG_LIGHTS);
        EXPECT_LE(tier.cloudSteps, ATMOSPHERE_MAX_CLOUD_STEPS);
        EXPECT_LE(tier.cloudLightSteps, ATMOSPHERE_MAX_CLOUD_LIGHT_STEPS);
    }
}

TEST(AtmosphereTiers, OverridesCapDownwardOnly){
    EXPECT_EQ(AtmosphereTiers::apply(AtmosphereQuality::HIGH, AtmosphereQualityOverride::PROJECT), AtmosphereQuality::HIGH);
    EXPECT_EQ(AtmosphereTiers::apply(AtmosphereQuality::HIGH, AtmosphereQualityOverride::MEDIUM), AtmosphereQuality::MEDIUM);
    EXPECT_EQ(AtmosphereTiers::apply(AtmosphereQuality::HIGH, AtmosphereQualityOverride::LOW), AtmosphereQuality::LOW);
    EXPECT_EQ(AtmosphereTiers::apply(AtmosphereQuality::LOW, AtmosphereQualityOverride::MEDIUM), AtmosphereQuality::LOW);
    EXPECT_EQ(AtmosphereTiers::apply(AtmosphereQuality::OFF, AtmosphereQualityOverride::LOW), AtmosphereQuality::OFF);
}

TEST(AtmosphereTiers, AutomaticCloudsFollowTheTier){
    EXPECT_EQ(AtmosphereTiers::resolveClouds(CloudsRendering::AUTO, AtmosphereQuality::LOW), CloudsRendering::SKY_MAP);
    EXPECT_EQ(AtmosphereTiers::resolveClouds(CloudsRendering::AUTO, AtmosphereQuality::MEDIUM), CloudsRendering::SKY_MAP);
    EXPECT_EQ(AtmosphereTiers::resolveClouds(CloudsRendering::AUTO, AtmosphereQuality::HIGH), CloudsRendering::SCREEN);
    EXPECT_EQ(AtmosphereTiers::resolveClouds(CloudsRendering::SCREEN, AtmosphereQuality::LOW), CloudsRendering::SCREEN);
    EXPECT_EQ(AtmosphereTiers::resolveClouds(CloudsRendering::SKY_MAP, AtmosphereQuality::HIGH), CloudsRendering::SKY_MAP);
}

TEST(AtmosphereTiers, PlatformDefaultsAndRuntimeOverride){
    EXPECT_EQ(Engine::getAtmosphereQualityDefault(Platform::Linux), AtmosphereQuality::HIGH);
    EXPECT_EQ(Engine::getAtmosphereQualityDefault(Platform::Android), AtmosphereQuality::LOW);
    EXPECT_EQ(Engine::getAtmosphereQualityDefault(Platform::Web), AtmosphereQuality::LOW);

    Engine::setAtmosphereQualityDefaults(AtmosphereQuality::MEDIUM, AtmosphereQuality::OFF, AtmosphereQuality::LOW);
    EXPECT_EQ(Engine::getAtmosphereQualityDefault(Platform::Windows), AtmosphereQuality::MEDIUM);
    EXPECT_EQ(Engine::getAtmosphereQualityDefault(Platform::iOS), AtmosphereQuality::OFF);
    EXPECT_FALSE(Engine::isAtmosphereQualityOverridden());

    Engine::setAtmosphereQuality(AtmosphereQuality::OFF);
    EXPECT_TRUE(Engine::isAtmosphereQualityOverridden());
    EXPECT_EQ(Engine::getAtmosphereQuality(), AtmosphereQuality::OFF);
    Engine::resetAtmosphereQuality();
    EXPECT_FALSE(Engine::isAtmosphereQualityOverridden());

    Engine::setAtmosphereQualityDefaults(AtmosphereQuality::HIGH, AtmosphereQuality::LOW, AtmosphereQuality::LOW);
}

TEST(AtmospherePresets, ListFiveFogAndFourCloudLooks){
    const std::vector<AtmospherePreset>& fog = AtmospherePresets::getFogPresets();
    ASSERT_EQ(fog.size(), 5u);
    EXPECT_EQ(fog[0].id, "morning_haze");
    const std::vector<AtmospherePreset>& clouds = AtmospherePresets::getCloudsPresets();
    // Custom, then the four looks, in CloudsPreset order
    ASSERT_EQ(clouds.size(), 5u);
    EXPECT_EQ(clouds[(int)CloudsPreset::CUSTOM].id, "custom");
    EXPECT_EQ(clouds[(int)CloudsPreset::OVERCAST].id, "overcast");
    for (const AtmospherePreset& preset : clouds){
        CloudsPreset value;
        ASSERT_TRUE(AtmospherePresets::findCloudsPreset(preset.id, value)) << preset.id;
        EXPECT_EQ(AtmospherePresets::getCloudsPresetId(value), preset.id);
    }
}

TEST(AtmospherePresets, FogPresetsMakeAHeightFogAndKeepTheVolumetricSettings){
    for (const AtmospherePreset& preset : AtmospherePresets::getFogPresets()){
        FogComponent fog;
        fog.volumetric = true;
        fog.volumetricDistance = 33.0f;
        fog.colorMode = FogColorMode::SKY;
        ASSERT_TRUE(AtmospherePresets::applyFogPreset(preset.id, fog)) << preset.id;
        EXPECT_EQ(fog.type, FogType::HEIGHT) << preset.id;
        EXPECT_TRUE(fog.volumetric);
        EXPECT_FLOAT_EQ(fog.volumetricDistance, 33.0f);
        EXPECT_EQ(fog.colorMode, FogColorMode::SKY);
        EXPECT_GT(fog.attenuationDistance, 0.0f);
        EXPECT_GT(fog.maximumHeight, fog.baseHeight);
    }
    FogComponent fog;
    EXPECT_FALSE(AtmospherePresets::applyFogPreset("no_such_look", fog));
    EXPECT_EQ(fog.type, FogType::LINEAR);
}

TEST(AtmospherePresets, CloudPresetsDiffer){
    VolumetricCloudsComponent sparse;
    VolumetricCloudsComponent overcast;
    AtmospherePresets::applyCloudsPreset(CloudsPreset::SPARSE, sparse);
    AtmospherePresets::applyCloudsPreset(CloudsPreset::OVERCAST, overcast);
    EXPECT_EQ(sparse.preset, CloudsPreset::SPARSE);
    EXPECT_EQ(overcast.preset, CloudsPreset::OVERCAST);
    EXPECT_LT(sparse.coverage, overcast.coverage);

    // Custom sets the field only
    VolumetricCloudsComponent custom = overcast;
    AtmospherePresets::applyCloudsPreset(CloudsPreset::CUSTOM, custom);
    EXPECT_EQ(custom.preset, CloudsPreset::CUSTOM);
    EXPECT_FLOAT_EQ(custom.coverage, overcast.coverage);
}

TEST(Atmosphere, LegacyFogKeysUnchanged){
    // a lit, textured, normal-mapped mesh in a scene with a light and a distance fog
    MeshShaderEnvironment environment;
    environment.lights = true;
    environment.fog = true;
    SubmeshShaderInputs inputs;
    inputs.texCoord1 = true;
    inputs.normal = true;
    inputs.tangent = true;
    inputs.baseColorTexture = true;
    inputs.normalTexture = true;
    const SubmeshShaderVariant distance = RenderSystem::getSubmeshShaderVariant(environment, inputs);
    // the fog's bit; a height fog adds no bit (the fogged variants evaluate it: u_fs_heightFog)
    EXPECT_NE(distance.properties & (uint64_t(1) << 12), 0u);
    // S10's 'Shq' is bit 33 (bit 5 is upstream's 'Sto')
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::MESH, 33), "Shq");

    // the atmosphere's environment fields at their defaults change nothing
    MeshShaderEnvironment same = environment;
    same.atmosphereDepth = false;
    EXPECT_EQ(RenderSystem::getSubmeshShaderVariant(same, inputs).properties, distance.properties);

    // the atmosphere's depth pre-pass asks every mesh for a depth program
    MeshShaderEnvironment depth = environment;
    inputs.castShadows = false;
    EXPECT_FALSE(RenderSystem::getSubmeshShaderVariant(depth, inputs).depthPass);
    depth.atmosphereDepth = true;
    EXPECT_TRUE(RenderSystem::getSubmeshShaderVariant(depth, inputs).depthPass);
}

TEST(Atmosphere, ExportsOnlyThePassesASceneUses){
    EXPECT_TRUE(AtmosphereRenderer::passShaderTypes(false, false, false).empty());
    const std::vector<ShaderType> heightOnly = AtmosphereRenderer::passShaderTypes(true, false, false);
    ASSERT_EQ(heightOnly.size(), 1u);
    EXPECT_EQ(heightOnly[0], ShaderType::SKY_COMPOSITE);
    const std::vector<ShaderType> volumetric = AtmosphereRenderer::passShaderTypes(true, true, false);
    EXPECT_NE(std::find(volumetric.begin(), volumetric.end(), ShaderType::VOLUMETRIC_FOG), volumetric.end());
    EXPECT_EQ(std::find(volumetric.begin(), volumetric.end(), ShaderType::VOLUMETRIC_CLOUDS), volumetric.end());
    const std::vector<ShaderType> clouds = AtmosphereRenderer::passShaderTypes(false, false, true);
    EXPECT_NE(std::find(clouds.begin(), clouds.end(), ShaderType::VOLUMETRIC_CLOUDS), clouds.end());
    EXPECT_EQ(std::find(clouds.begin(), clouds.end(), ShaderType::VOLUMETRIC_FOG), clouds.end());
}
