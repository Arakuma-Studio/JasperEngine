// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The atmosphere's diagnostics (docs/specs/S101-vapor.md R17, AC9): each test builds the state that
// raises one code, expects it, fixes the cause and expects it gone. Fog Profiles are set in the
// library (no files). The view's last frame is an
// AtmosphereInfo built here (what RenderSystem::getAtmosphereInfo would hand over).

#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/FogComponent.h"
#include "component/FogZoneComponent.h"
#include "component/LightComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/Transform.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/environment/Fog.h"
#include "object/environment/FogZone.h"
#include "object/environment/LocalVolumetricFog.h"
#include "render/AtmosphereDiagnostics.h"
#include "render/AtmosphereMath.h"
#include "render/FogProfile.h"
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
        std::vector<std::unique_ptr<FogZone>> zones;

        void SetUp() override{
            // the profiles a test sets (FogProfileLibrary is process-wide)
            FogProfileLibrary::clear();
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

        void TearDown() override{
            FogProfileLibrary::clear();
        }

        // a Fog Zone at the origin (its world transform as the resolve reads it)
        FogZoneComponent& addZone(const char* name, const char* profile = nullptr){
            zones.push_back(std::make_unique<FogZone>(&scene));
            scene.setEntityName(zones.back()->getEntity(), name);
            scene.getComponent<Transform>(zones.back()->getEntity()).worldPosition = Vector3(0.0f, 0.0f, 0.0f);
            FogZoneComponent& zone = zones.back()->getComponent<FogZoneComponent>();
            if (profile)
                zone.profile = AssetRef(profile);
            return zone;
        }

        Entity zoneEntity(size_t i = 0) const{
            return zones[i]->getEntity();
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

    // a profile that sets the given fields to `values`' values
    FogProfileData profileOf(std::initializer_list<FogField> fields, const FogComponent& values = FogComponent()){
        FogProfileData profile;
        for (FogField field : fields)
            profile.setFrom(values, field);
        return profile;
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

// Fog Zones blend over the scene's Fog: without one they change nothing
TEST_F(AtmosphereDiagnosticsTest, FogMissingForZones){
    FogProfileLibrary::set("FogProfiles/cave.fogprofile", profileOf({FogField::AttenuationDistance}));
    addZone("Cave", "FogProfiles/cave.fogprofile");
    const std::vector<AtmosphereDiagnostic> found = find("fog_missing");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_TRUE(mentions(found[0].message, "Fog Zones")) << found[0].message;
    EXPECT_FALSE(found[0].fix.empty());

    addFog(FogType::HEIGHT, false);
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

TEST_F(AtmosphereDiagnosticsTest, FogInvisibleNotForACameraAboveAGroundLayer){
    // a dense layer from 1 m thinning out by 7 m (the toxic built-in's shape), the camera 30 m up
    // looking level: its own ray passes over the layer, which still fills the view below
    addMainCamera();
    cameras.back()->getComponent<Transform>().worldPosition = Vector3(0.0f, 30.0f, 0.0f);
    FogComponent& fog = addFog(FogType::HEIGHT, false);
    fog.attenuationDistance = 8.0f;
    fog.baseHeight = 1.0f;
    fog.maximumHeight = 7.0f;
    const AtmosphereReport report = run();
    ASSERT_TRUE(report.opacity.valid);
    EXPECT_LT(report.opacity.at200, AtmosphereDiagnostics::INVISIBLE_OPACITY);
    EXPECT_GT(report.opacity.levelAt200, 0.5f);
    EXPECT_FALSE(has("fog_invisible"));
    // the same layer too thin to see anywhere is reported
    fog.attenuationDistance = 100000.0f;
    EXPECT_TRUE(has("fog_invisible", fogEntity()));
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

// a profile named but missing renders as an empty one (R6): the Fog keeps its own look, a zone has no effect
TEST_F(AtmosphereDiagnosticsTest, ProfileMissing){
    FogComponent& fog = addFog(FogType::HEIGHT, false);
    fog.profile = AssetRef("FogProfiles/gone.fogprofile");
    std::vector<AtmosphereDiagnostic> found = find("profile_missing", fogEntity());
    ASSERT_EQ(found.size(), 1u);
    EXPECT_TRUE(mentions(found[0].message, "FogProfiles/gone.fogprofile")) << found[0].message;
    EXPECT_TRUE(mentions(found[0].message, "own values")) << found[0].message;
    EXPECT_FALSE(found[0].fix.empty());
    FogProfileLibrary::set("FogProfiles/gone.fogprofile", profileOf({FogField::AttenuationDistance}));
    EXPECT_FALSE(has("profile_missing"));

    addZone("Cave", "FogProfiles/cave.fogprofile");
    found = find("profile_missing", zoneEntity());
    ASSERT_EQ(found.size(), 1u);
    EXPECT_TRUE(mentions(found[0].message, "\"Cave\"")) << found[0].message;
    EXPECT_TRUE(mentions(found[0].message, "FogProfiles/cave.fogprofile")) << found[0].message;
    // a missing profile is not also reported as a zone without effect
    EXPECT_FALSE(has("zone_no_effect"));
    FogProfileLibrary::set("FogProfiles/cave.fogprofile", profileOf({FogField::AttenuationDistance}));
    EXPECT_FALSE(has("profile_missing"));
}

// a profile that sets only fields the fog's type ignores changes nothing
TEST_F(AtmosphereDiagnosticsTest, ProfileNoEffect){
    FogComponent& fog = addFog(FogType::LINEAR, false);
    FogProfileLibrary::set("FogProfiles/night.fogprofile", profileOf({FogField::AttenuationDistance, FogField::BaseHeight}));
    fog.profile = AssetRef("FogProfiles/night.fogprofile");
    std::vector<AtmosphereDiagnostic> found = find("profile_no_effect", fogEntity());
    ASSERT_EQ(found.size(), 1u);
    EXPECT_TRUE(mentions(found[0].message, "Linear")) << found[0].message;
    EXPECT_TRUE(mentions(found[0].message, "FogProfiles/night.fogprofile")) << found[0].message;
    EXPECT_TRUE(mentions(found[0].fix, "Height")) << found[0].fix;
    fog.type = FogType::HEIGHT;
    EXPECT_FALSE(has("profile_no_effect"));

    // a distance fog's fields on a Height fog, through a zone
    FogProfileLibrary::set("FogProfiles/old.fogprofile", profileOf({FogField::Density, FogField::LinearEnd}));
    addZone("Cave", "FogProfiles/old.fogprofile");
    ASSERT_TRUE(has("profile_no_effect", zoneEntity()));
    FogProfileLibrary::set("FogProfiles/old.fogprofile", profileOf({FogField::Density, FogField::Color}));
    EXPECT_FALSE(has("profile_no_effect"));
}

// a zone without a profile, with a profile that sets nothing, or of weight 0 changes nothing
TEST_F(AtmosphereDiagnosticsTest, ZoneNoEffect){
    addFog(FogType::HEIGHT, false);
    FogZoneComponent& zone = addZone("Cave");
    std::vector<AtmosphereDiagnostic> found = find("zone_no_effect", zoneEntity());
    ASSERT_EQ(found.size(), 1u);
    EXPECT_TRUE(mentions(found[0].message, "no profile")) << found[0].message;

    FogProfileLibrary::set("FogProfiles/empty.fogprofile", FogProfileData());
    zone.profile = AssetRef("FogProfiles/empty.fogprofile");
    found = find("zone_no_effect", zoneEntity());
    ASSERT_EQ(found.size(), 1u);
    EXPECT_TRUE(mentions(found[0].message, "sets no field")) << found[0].message;

    FogProfileLibrary::set("FogProfiles/empty.fogprofile", profileOf({FogField::AttenuationDistance}));
    EXPECT_FALSE(has("zone_no_effect"));

    zone.weight = 0.0f;
    found = find("zone_no_effect", zoneEntity());
    ASSERT_EQ(found.size(), 1u);
    EXPECT_TRUE(mentions(found[0].message, "Weight 0")) << found[0].message;
    zone.weight = 1.0f;
    EXPECT_FALSE(has("zone_no_effect"));
}

// the estimate and the look checks read what the main camera sees: the Fog under its profile and zones
TEST_F(AtmosphereDiagnosticsTest, OpacityFollowsProfile){
    addMainCamera();
    FogComponent& fog = addFog(FogType::HEIGHT, false);
    fog.attenuationDistance = 100000.0f;
    ASSERT_TRUE(has("fog_invisible", fogEntity()));
    const float thin = run().opacity.at200;

    FogComponent dense;
    dense.attenuationDistance = 100.0f;
    FogProfileLibrary::set("FogProfiles/dense.fogprofile", profileOf({FogField::AttenuationDistance}, dense));
    fog.profile = AssetRef("FogProfiles/dense.fogprofile");
    AtmosphereReport report = run();
    ASSERT_TRUE(report.opacity.valid);
    EXPECT_GT(report.opacity.at200, thin + 0.5f);
    // the same as the Fog's own field at that value
    FogComponent same = fog;
    same.profile = AssetRef();
    same.attenuationDistance = 100.0f;
    EXPECT_NEAR(report.opacity.at200, AtmosphereMath::fogOpacity(same, Vector3(0.0f, 2.0f, 0.0f), Vector3(0.0f, 0.0f, -1.0f), 200.0f), 1e-5f);
    EXPECT_FALSE(has("fog_invisible"));
    EXPECT_NEAR(AtmosphereDiagnostics::estimateOpacity(scene).at200, report.opacity.at200, 1e-6f);

    // a zone the camera is in thickens it to opaque
    FogComponent opaque;
    opaque.attenuationDistance = 1.0f;
    FogProfileLibrary::set("FogProfiles/wall.fogprofile", profileOf({FogField::AttenuationDistance}, opaque));
    addZone("Wall", "FogProfiles/wall.fogprofile");
    ASSERT_TRUE(has("fog_opaque", fogEntity()));
    zones.back()->getComponent<FogZoneComponent>().weight = 0.0f;
    EXPECT_FALSE(has("fog_opaque"));

    fog.profile = AssetRef();
    EXPECT_TRUE(has("fog_invisible", fogEntity()));
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
