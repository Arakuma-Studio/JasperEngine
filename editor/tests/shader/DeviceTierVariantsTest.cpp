// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The variants of the Device Tiers (spec S56): a player applies the tier its GPU gets, and a
// thermal step may move it, so the export ships what every reachable tier draws with. The Low
// tier's render scale of 0.5 runs the uber pass's upscale (BLIT 'Scl'); a build without it showed
// nothing on a device detected as Low ("Missing shader: blit_Scl"). The collection leaves the
// editor's own QualityResolver state as it found it.

#include "shader/ShaderVariantCollector.h"

#include "Scene.h"
#include "device/DeviceSettings.h"
#include "device/QualityResolver.h"
#include "object/Primitive.h"
#include "pool/ShaderPool.h"
#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    class DeviceTierVariants : public ::testing::Test {
    protected:
        static void SetUpTestSuite(){
            editor::test::ensureEngine();
        }
        void TearDown() override{
            QualityResolver::reset();
        }
    };

    std::set<ShaderKey> collectTierKeys(Scene& scene){
        ShaderVariantContent content;
        content.registry = &scene;
        content.entities = scene.getEntityList();
        std::set<ShaderKey> keys;
        ShaderVariantCollector::collectScene(&scene, content, {}, keys);
        return keys;
    }

    bool hasUpscale(const std::set<ShaderKey>& keys){
        return keys.count(ShaderPool::getShaderKey(ShaderType::BLIT, UBER_UPSCALE)) > 0;
    }

    // settings whose tiers override nothing (the defaults' Low and Medium cells cleared)
    DeviceSettings noOverrides(){
        DeviceSettings settings;
        for (auto& group : settings.tiers)
            for (DeviceTierOverrides& tier : group)
                tier = DeviceTierOverrides();
        settings.thermalResponse = {false, false, false};
        return settings;
    }

}

TEST_F(DeviceTierVariants, ReachableTiersShipTheirVariants){
    Scene scene;
    Primitive sphere(&scene);
    sphere.createSphere(1.0f, 24, 12);
    scene.setSSAOEnabled(true);

    // no tier can change anything: the scene's own variants, no upscale
    QualityResolver::setSettings(noOverrides());
    const std::set<ShaderKey> plain = collectTierKeys(scene);
    EXPECT_FALSE(hasUpscale(plain));

    // the default tiers: Low's half render scale and Medium's 0.85 need the upscale, and Low's
    // SSAO off adds the meshes' variants without it
    QualityResolver::setSettings(DeviceSettings());
    const std::set<ShaderKey> tiered = collectTierKeys(scene);
    EXPECT_TRUE(hasUpscale(tiered)) << "the Low tier's render scale draws through BLIT 'Scl'";
    EXPECT_GT(tiered.size(), plain.size());
    for (const ShaderKey& key : plain)
        EXPECT_TRUE(tiered.count(key)) << ShaderPool::getShaderStr(ShaderPool::getShaderTypeFromKey(key), ShaderPool::getPropertiesFromKey(key), 0);
}

TEST_F(DeviceTierVariants, ThermalStepsOfTheFamiliesThatRespond){
    Scene scene;
    Primitive sphere(&scene);
    sphere.createSphere(1.0f, 24, 12);

    // no tier lowers the scale, but a Serious thermal step on mobile scales by 0.8
    DeviceSettings settings = noOverrides();
    QualityResolver::setSettings(settings);
    EXPECT_FALSE(hasUpscale(collectTierKeys(scene)));
    settings.thermalResponse[(size_t)DevicePlatformGroup::Mobile] = true;
    QualityResolver::setSettings(settings);
    EXPECT_TRUE(hasUpscale(collectTierKeys(scene))) << "the thermal response's scale needs the upscale too";
}

TEST_F(DeviceTierVariants, TheEditorsStateComesBack){
    Scene scene;
    Primitive sphere(&scene);
    sphere.createSphere(1.0f, 24, 12);
    QualityResolver::setSettings(DeviceSettings());
    QualityResolver::setPlatformGroup(DevicePlatformGroup::Desktop);
    QualityResolver::setTier(std::nullopt);
    collectTierKeys(scene);
    EXPECT_FALSE(QualityResolver::getTier().has_value());
    EXPECT_EQ(QualityResolver::getPlatformGroup(), DevicePlatformGroup::Desktop);
    EXPECT_EQ(QualityResolver::getThermalResponse(), ThermalState::Nominal);
    EXPECT_FLOAT_EQ(QualityResolver::renderScale(1.0f), 1.0f);
}
