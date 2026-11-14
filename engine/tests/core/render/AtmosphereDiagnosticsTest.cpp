// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The atmosphere's diagnostics (docs/specs/vapor.md R17, AC9): each test builds the state that
// raises one code, expects it, fixes the cause and expects it gone. The view's last frame is an
// AtmosphereInfo built here (what RenderSystem::getAtmosphereInfo would hand over).

#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/FogComponent.h"
#include "component/LightComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/Transform.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/environment/Fog.h"
#include "object/environment/LocalVolumetricFog.h"
#include "render/AtmosphereDiagnostics.h"
#include "subsystem/AtmosphereRenderer.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    class AtmosphereDiagnosticsTest : public ::testing::Test {
    protected:
        Scene scene;
        AtmosphereInfo info;
        bool sceneViewFogDisabled = false;
        std::vector<std::unique_ptr<Fog>> fogs;
        std::vector<std::unique_ptr<LocalVolumetricFog>> volumes;
        std::vector<std::unique_ptr<Light>> lights;
        std::vector<std::unique_ptr<Camera>> cameras;

        void SetUp() override{
            // a frame of the view, at High
            info.frame = 1;
            info.quality = AtmosphereQuality::HIGH;
            info.mainCameraPass = true;
        }

        FogComponent& addFog(FogType type, bool volumetric, const char* name = "Fog"){
            fogs.push_back(std::make_unique<Fog>(&scene));
            scene.setEntityName(fogs.back()->getEntity(), name);
            FogComponent& fog = fogs.back()->getComponent<FogComponent>();
            fog.type = type;
            fog.volumetric = volumetric;
            return fog;
        }

        Entity fogEntity(size_t i = 0) const{
            return fogs[i]->getEntity();
        }

        Entity addVolume(const char* name){
            volumes.push_back(std::make_unique<LocalVolumetricFog>(&scene));
            scene.setEntityName(volumes.back()->getEntity(), name);
            return volumes.back()->getEntity();
        }

        LightComponent& addLight(LightType type, bool shadows, const char* name){
            lights.push_back(std::make_unique<Light>(&scene));
            scene.setEntityName(lights.back()->getEntity(), name);
            LightComponent& light = lights.back()->getComponent<LightComponent>();
            light.type = type;
            light.intensity = 1.0f;
            light.shadows = shadows;
            return light;
        }

        // the main camera at 2 m, looking down -Z
        void addMainCamera(){
            cameras.push_back(std::make_unique<Camera>(&scene));
            cameras.back()->activate();
            cameras.back()->getComponent<CameraComponent>().worldDirection = Vector3(0.0f, 0.0f, 1.0f);
            cameras.back()->getComponent<Transform>().worldPosition = Vector3(0.0f, 2.0f, 0.0f);
        }

        // a volumetric height fog the view ran, fully drawn
        FogComponent& volumetricFog(){
            FogComponent& fog = addFog(FogType::HEIGHT, true);
            info.volumetric = true;
            info.volumetricActive = true;
            return fog;
        }

        AtmosphereReport run(){
            AtmosphereDiagnosticsInput input;
            input.scene = &scene;
            input.info = &info;
            input.sceneViewFogDisabled = sceneViewFogDisabled;
            return AtmosphereDiagnostics::run(input);
        }

        // the diagnostics of a code (about entity, when given)
        std::vector<AtmosphereDiagnostic> find(const std::string& code, Entity entity = NULL_ENTITY){
            std::vector<AtmosphereDiagnostic> found;
            for (const AtmosphereDiagnostic& diagnostic : run().diagnostics){
                if (diagnostic.code == code && (entity == NULL_ENTITY || diagnostic.entity == entity))
                    found.push_back(diagnostic);
            }
            return found;
        }

        bool has(const std::string& code, Entity entity = NULL_ENTITY){
            return !find(code, entity).empty();
        }
    };

    bool mentions(const std::string& text, const std::string& part){
        return text.find(part) != std::string::npos;
    }

}

TEST_F(AtmosphereDiagnosticsTest, FogMissing){
    addVolume("Cave Mist");
    const std::vector<AtmosphereDiagnostic> found = find("fog_missing");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].severity, AtmosphereSeverity::Warning);
    EXPECT_FALSE(found[0].fix.empty());

    volumetricFog();
    EXPECT_FALSE(has("fog_missing"));
}

TEST_F(AtmosphereDiagnosticsTest, FogSecondIgnored){
    addFog(FogType::HEIGHT, false, "Fog");
    addFog(FogType::LINEAR, false, "Other Fog");
    const std::vector<AtmosphereDiagnostic> found = find("fog_second_ignored");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].entity, fogEntity(1));
    EXPECT_TRUE(mentions(found[0].message, "\"Other Fog\"")) << found[0].message;
    EXPECT_TRUE(mentions(found[0].message, "\"Fog\"")) << found[0].message;

    scene.removeComponent<FogComponent>(fogEntity(1));
    EXPECT_FALSE(has("fog_second_ignored"));
}

TEST_F(AtmosphereDiagnosticsTest, VolumetricNeedsHeight){
    FogComponent& fog = addFog(FogType::LINEAR, true);
    ASSERT_TRUE(has("volumetric_needs_height", fogEntity()));
    fog.type = FogType::HEIGHT;
    EXPECT_FALSE(has("volumetric_needs_height"));

    // volumes need the volumetric part on
    fog.volumetric = false;
    addVolume("Cave Mist");
    const std::vector<AtmosphereDiagnostic> found = find("volumetric_needs_height", fogEntity());
    ASSERT_EQ(found.size(), 1u);
    EXPECT_TRUE(mentions(found[0].fix, "Volumetric Fog > Enabled")) << found[0].fix;
    fog.volumetric = true;
    EXPECT_FALSE(has("volumetric_needs_height"));
}

TEST_F(AtmosphereDiagnosticsTest, VolumetricQualityOff){
    volumetricFog();
    info.volumetricActive = false;
    info.quality = AtmosphereQuality::OFF;
    // each source names its own setting
    const std::pair<const char*, const char*> sources[] = {
        {"runtime", "Engine.setAtmosphereQuality"},
        {"game_view_preview", "Game view"},
        {"device_tier", "Device Tiers"},
        {"project", "Project Settings > Quality > Atmosphere Quality"},
    };
    for (const auto& source : sources){
        info.qualitySource = source.first;
        const std::vector<AtmosphereDiagnostic> found = find("volumetric_quality_off", fogEntity());
        ASSERT_EQ(found.size(), 1u) << source.first;
        EXPECT_TRUE(mentions(found[0].message, source.second)) << source.first << ": " << found[0].message;
    }

    info.quality = AtmosphereQuality::LOW;
    info.volumetricActive = true;
    EXPECT_FALSE(has("volumetric_quality_off"));
}

TEST_F(AtmosphereDiagnosticsTest, VolumetricMainCameraOnly){
    volumetricFog();
    // a view that drew through another camera
    info.volumetricActive = false;
    info.mainCameraPass = false;
    EXPECT_TRUE(has("volumetric_main_camera_only", fogEntity()));
    info.volumetricActive = true;
    info.mainCameraPass = true;
    EXPECT_FALSE(has("volumetric_main_camera_only"));

    // a render-to-texture camera gets the analytic fog
    addMainCamera();
    cameras.push_back(std::make_unique<Camera>(&scene));
    const Entity mirror = cameras.back()->getEntity();
    scene.setEntityName(mirror, "Mirror Camera");
    cameras.back()->getComponent<CameraComponent>().renderToTexture = true;
    ASSERT_TRUE(has("volumetric_main_camera_only", mirror));
    cameras.back()->getComponent<CameraComponent>().renderToTexture = false;
    EXPECT_FALSE(has("volumetric_main_camera_only"));
}

TEST_F(AtmosphereDiagnosticsTest, ShaftsOff){
    FogComponent& fog = volumetricFog();
    addLight(LightType::DIRECTIONAL, true, "Sun");
    fog.lightShafts = false;
    const std::vector<AtmosphereDiagnostic> found = find("shafts_off", fogEntity());
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].severity, AtmosphereSeverity::Info);
    fog.lightShafts = true;
    info.lightShafts = true;
    EXPECT_FALSE(has("shafts_off"));
}

TEST_F(AtmosphereDiagnosticsTest, ShaftsSunNoShadows){
    volumetricFog();
    // no sun at all
    ASSERT_TRUE(has("shafts_sun_no_shadows", fogEntity()));
    LightComponent& sun = addLight(LightType::DIRECTIONAL, false, "Sun");
    const std::vector<AtmosphereDiagnostic> found = find("shafts_sun_no_shadows", fogEntity());
    ASSERT_EQ(found.size(), 1u);
    EXPECT_TRUE(mentions(found[0].fix, "\"Sun\"")) << found[0].fix;
    sun.shadows = true;
    info.lightShafts = true;
    EXPECT_FALSE(has("shafts_sun_no_shadows"));
}

TEST_F(AtmosphereDiagnosticsTest, LightNotScattered){
    volumetricFog();
    info.lightShafts = true;
    addLight(LightType::POINT, false, "Lamp");
    const Entity lamp = lights.back()->getEntity();
    AtmosphereFogLight record;
    record.entity = lamp;
    record.scattered = false;
    // every reason the fog-light selection records has a sentence of its own
    std::vector<std::string> messages;
    for (const char* reason : {"intensity", "volumetric_intensity", "distance", "frustum", "tier_cap"}){
        record.reason = reason;
        info.fogLights = {record};
        const std::vector<AtmosphereDiagnostic> found = find("light_not_scattered", lamp);
        ASSERT_EQ(found.size(), 1u) << reason;
        EXPECT_TRUE(mentions(found[0].message, "\"Lamp\"")) << found[0].message;
        for (const std::string& other : messages)
            EXPECT_NE(found[0].message, other) << reason;
        messages.push_back(found[0].message);
    }
    // the cap names the tier's count
    EXPECT_TRUE(mentions(messages.back(), "High")) << messages.back();

    record.scattered = true;
    record.reason.clear();
    info.fogLights = {record};
    EXPECT_FALSE(has("light_not_scattered"));
}

TEST_F(AtmosphereDiagnosticsTest, VolumeCulled){
    volumetricFog();
    info.lightShafts = true;
    const Entity volume = addVolume("Far Mist");
    AtmosphereFogVolume record;
    record.entity = volume;
    record.used = false;
    record.reason = "distance";
    info.fogVolumes = {record};
    const std::vector<AtmosphereDiagnostic> found = find("volume_culled", volume);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_TRUE(mentions(found[0].message, "64 m")) << found[0].message;
    record.reason = "tier_cap";
    info.fogVolumes = {record};
    ASSERT_EQ(find("volume_culled", volume).size(), 1u);
    EXPECT_TRUE(mentions(find("volume_culled", volume)[0].message, "8 volumes")) << find("volume_culled", volume)[0].message;

    record.used = true;
    record.reason.clear();
    info.fogVolumes = {record};
    EXPECT_FALSE(has("volume_culled"));
}

TEST_F(AtmosphereDiagnosticsTest, SceneViewFogDisabled){
    addFog(FogType::LINEAR, false);
    sceneViewFogDisabled = true;
    ASSERT_TRUE(has("scene_view_fog_disabled", fogEntity()));
    sceneViewFogDisabled = false;
    EXPECT_FALSE(has("scene_view_fog_disabled"));
}

TEST_F(AtmosphereDiagnosticsTest, SkyModeNoHorizon){
    FogComponent& fog = addFog(FogType::HEIGHT, false);
    fog.colorMode = FogColorMode::SKY;
    info.horizonSource = "fog_color";
    ASSERT_TRUE(has("sky_mode_no_horizon", fogEntity()));
    info.horizonSource = "cubemap";
    EXPECT_FALSE(has("sky_mode_no_horizon"));
}

TEST_F(AtmosphereDiagnosticsTest, LitNoLight){
    FogComponent& fog = addFog(FogType::HEIGHT, false);
    fog.colorMode = FogColorMode::LIT;
    scene.setGlobalIllumination(0.0f);
    ASSERT_TRUE(has("lit_no_light", fogEntity()));
    // a sun lights it
    addLight(LightType::DIRECTIONAL, false, "Sun");
    EXPECT_FALSE(has("lit_no_light"));
    // and so does the scene's ambient light
    lights.back()->getComponent<LightComponent>().intensity = 0.0f;
    ASSERT_TRUE(has("lit_no_light"));
    scene.setGlobalIllumination(0.5f);
    EXPECT_FALSE(has("lit_no_light"));
}

TEST_F(AtmosphereDiagnosticsTest, FogInvisible){
    addMainCamera();
    FogComponent& fog = addFog(FogType::HEIGHT, false);
    fog.attenuationDistance = 100000.0f;
    const AtmosphereReport report = run();
    ASSERT_TRUE(report.opacity.valid);
    EXPECT_LT(report.opacity.at200, AtmosphereDiagnostics::INVISIBLE_OPACITY);
    ASSERT_TRUE(has("fog_invisible", fogEntity()));
    fog.attenuationDistance = 100.0f;
    EXPECT_FALSE(has("fog_invisible"));
}

TEST_F(AtmosphereDiagnosticsTest, FogOpaque){
    addMainCamera();
    FogComponent& fog = addFog(FogType::HEIGHT, false);
    fog.attenuationDistance = 1.0f;
    const AtmosphereReport report = run();
    ASSERT_TRUE(report.opacity.valid);
    EXPECT_GT(report.opacity.at10, AtmosphereDiagnostics::OPAQUE_OPACITY);
    ASSERT_TRUE(has("fog_opaque", fogEntity()));
    fog.attenuationDistance = 400.0f;
    EXPECT_FALSE(has("fog_opaque"));
}

// a scene without atmosphere has nothing to say, and a view that drew no frame skips the checks
// that need one
TEST_F(AtmosphereDiagnosticsTest, QuietWithoutAtmosphereOrFrame){
    EXPECT_TRUE(run().diagnostics.empty());
    EXPECT_FALSE(run().opacity.valid);
    volumetricFog();
    info = AtmosphereInfo();
    EXPECT_TRUE(run().diagnostics.empty());
}
