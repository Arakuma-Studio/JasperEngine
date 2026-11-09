// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What the S13 post effects turn on (RenderSystem::getPostProcessingTiers): nothing at their
// defaults or neutral values, exactly their own uber bits when enabled (AC9), and dynamic
// resolution keeping the uber pass on at scale 1 (AC10). CPU only.

#include "subsystem/RenderSystem.h"
#include "pool/ShaderPool.h"
#include "device/QualityResolver.h"
#include "device/DeviceSettings.h"

#include <gtest/gtest.h>

using namespace jasperengine;

namespace {

    // the tiers of a default-constructed PostProcessingSettings before S13 (hard-coded, so a
    // new field that changes the default pixels fails here)
    void expectPreS13Defaults(const PostProcessingTiers& tiers){
        EXPECT_FALSE(tiers.tailColor);
        EXPECT_TRUE(tiers.tailGrade);
        EXPECT_FALSE(tiers.uber);
        EXPECT_EQ(tiers.uberFeatures, 0u);
        EXPECT_FLOAT_EQ(tiers.renderScale, 1.0f);
        EXPECT_EQ(tiers.msaaSamples, 1);
        EXPECT_FALSE(tiers.halationChain);
        EXPECT_FALSE(tiers.dynamicResolution);
    }

    uint32_t featuresOf(const PostProcessingSettings& settings){
        return RenderSystem::getPostProcessingTiers(settings).uberFeatures;
    }

}

TEST(PostProcessingTiers, S13Bits){
    // the bits are appended after UBER_UPSCALE in the spec's order, with their short names
    EXPECT_EQ(UBER_VHS, 1u << 13);
    EXPECT_EQ(UBER_HALATION, 1u << 14);
    EXPECT_EQ(UBER_FILM_DAMAGE, 1u << 15);
    EXPECT_EQ(UBER_GRAIN_LENS, 1u << 16);
    EXPECT_EQ(UBER_GRAIN_FILMIC, 1u << 17);
    // (S41's Stretch Fill, 'Psb', is bit 18 after them)
    EXPECT_EQ(UBER_FEATURE_COUNT, 19);
    EXPECT_EQ(ShaderPool::getShaderPropertyCount(ShaderType::BLIT), 19);
    const char* const shortNames[] = {"Vhs", "Hal", "Fdm", "Gln", "Gfm"};
    for (int i = 0; i < 5; i++)
        EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::BLIT, 13 + i, true), shortNames[i]);

    // defaults: the pre-S13 snapshot
    expectPreS13Defaults(RenderSystem::getPostProcessingTiers(PostProcessingSettings()));

    // each new effect at its neutral value: no bit
    {
        PostProcessingSettings s;
        s.vhsEnabled = true;
        s.vhsIntensity = 0.0f;
        expectPreS13Defaults(RenderSystem::getPostProcessingTiers(s));
    }
    {
        PostProcessingSettings s;
        s.halationEnabled = true;
        s.halationIntensity = 0.0f;
        expectPreS13Defaults(RenderSystem::getPostProcessingTiers(s));
    }
    {
        PostProcessingSettings s;
        s.filmDamageEnabled = true;
        s.filmDamageIntensity = 0.0f;
        s.filmGateWeave = 0.0f;
        s.filmGateMask = FilmGateMask::NONE;
        expectPreS13Defaults(RenderSystem::getPostProcessingTiers(s));
    }
    {
        // the grain's type and the film fields do nothing while the grain is off
        PostProcessingSettings s;
        s.filmGrainType = FilmGrainType::FILMIC;
        s.filmGrainSize = 4.0f;
        s.filmGrainColored = true;
        s.filmFrameRate = 12.0f;
        s.lutInputEncoding = LutInputEncoding::ACESCCT;
        s.dynamicResolutionMinScale = 0.5f;
        s.dynamicResolutionTargetFps = 30;
        expectPreS13Defaults(RenderSystem::getPostProcessingTiers(s));
    }

    // each effect alone: exactly its bits
    {
        PostProcessingSettings s;
        s.vhsEnabled = true;
        EXPECT_EQ(featuresOf(s), UBER_VHS);
        // VHS drops FXAA and sharpen (its bleed filters the lines)
        s.fxaaEnabled = true;
        s.sharpenEnabled = true;
        EXPECT_EQ(featuresOf(s), UBER_VHS);
        // and keeps the effects that combine with it
        s.crtEnabled = true;
        s.filmGrainEnabled = true;
        EXPECT_EQ(featuresOf(s), UBER_VHS | UBER_CRT | UBER_GRAIN);
    }
    {
        PostProcessingSettings s;
        s.halationEnabled = true;
        PostProcessingTiers tiers = RenderSystem::getPostProcessingTiers(s);
        EXPECT_EQ(tiers.uberFeatures, UBER_HALATION);
        EXPECT_TRUE(tiers.halationChain);       // its own two passes
        s.bloomEnabled = true;
        tiers = RenderSystem::getPostProcessingTiers(s);
        EXPECT_EQ(tiers.uberFeatures, UBER_HALATION | UBER_BLOOM);
        EXPECT_FALSE(tiers.halationChain);      // bloom's levels
    }
    {
        PostProcessingSettings s;
        s.filmDamageEnabled = true;
        EXPECT_EQ(featuresOf(s), UBER_FILM_DAMAGE);
        // the gate alone is damage too
        s.filmDamageIntensity = 0.0f;
        s.filmGateWeave = 0.0f;
        s.filmGateMask = FilmGateMask::ACADEMY_137;
        EXPECT_EQ(featuresOf(s), UBER_FILM_DAMAGE);
    }
    {
        PostProcessingSettings s;
        s.filmGrainEnabled = true;
        EXPECT_EQ(featuresOf(s), UBER_GRAIN);   // CLASSIC: the pre-S13 grain
        s.filmGrainType = FilmGrainType::LENS;
        EXPECT_EQ(featuresOf(s), UBER_GRAIN | UBER_GRAIN_LENS);
        s.filmGrainType = FilmGrainType::FILMIC;
        EXPECT_EQ(featuresOf(s), UBER_GRAIN | UBER_GRAIN_FILMIC);
    }
}

TEST(PostProcessingTiers, DynamicResolutionKeepsUber){
    PostProcessingSettings s;
    s.dynamicResolutionEnabled = true;
    ASSERT_FLOAT_EQ(s.renderScale, 1.0f);
    const PostProcessingTiers tiers = RenderSystem::getPostProcessingTiers(s);
    EXPECT_TRUE(tiers.uber);
    EXPECT_TRUE(tiers.dynamicResolution);
    EXPECT_EQ(tiers.uberFeatures, UBER_UPSCALE);
    EXPECT_FLOAT_EQ(tiers.renderScale, 1.0f);

    // with an effect the uber pass runs anyway, without the plain upscale bit
    s.vignetteEnabled = true;
    EXPECT_EQ(featuresOf(s), UBER_VIGNETTE);
}

// S51 R8 / AC22: a Low device tier halves the render scale (QualityResolver), which the uber pass
// upscales through a linear filter; a UI camera's image (its text and images) keeps the scale its
// settings give, so an exported UI draws as the editor does
TEST(PostProcessingTiers, AUiImageKeepsItsScaleOnALowTier){
    QualityResolver::reset();
    QualityResolver::setSettings(DeviceSettings());
    QualityResolver::setPlatformGroup(DevicePlatformGroup::Desktop);
    QualityResolver::setTier(DeviceTier::Low);
    const PostProcessingSettings settings;
    const PostProcessingTiers world = RenderSystem::getPostProcessingTiers(settings, nullptr, true);
    EXPECT_FLOAT_EQ(world.renderScale, 0.5f);
    EXPECT_TRUE(world.uber);
    EXPECT_EQ(world.uberFeatures & UBER_UPSCALE, (uint32_t)UBER_UPSCALE);
    const PostProcessingTiers ui = RenderSystem::getPostProcessingTiers(settings, nullptr, false);
    EXPECT_FLOAT_EQ(ui.renderScale, 1.0f);
    EXPECT_FALSE(ui.uber) << "no upscale pass for the UI's image";
    // a scale the settings ask for still applies
    PostProcessingSettings scaled;
    scaled.renderScale = 0.75f;
    EXPECT_FLOAT_EQ(RenderSystem::getPostProcessingTiers(scaled, nullptr, false).renderScale, 0.75f);
    QualityResolver::reset();
}
