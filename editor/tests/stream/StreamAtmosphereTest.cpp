// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The atmosphere's scene data (docs/specs/S12-atmosphere.md AC15): every new field of Fog, Local
// Volumetric Fog, Volumetric Clouds and the Light's volumetric intensity round-trips; a distance
// fog writes exactly the keys it always wrote; an old scene's Fog loads with the new defaults;
// an older editor keeps a height fog's fields as unknown data (S03 preservation, no format
// version change: docs/data-safety.md). The Fog's profile and the Fog Zone (docs/specs/S101-vapor.md
// R39, R40, AC1) are written only when set, without a version change.

#include "Stream.h"
#include "component/FogComponent.h"
#include "component/LightComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/FogZoneComponent.h"
#include "component/VolumetricCloudsComponent.h"
#include "reflect/Reflect.h"
#include "StreamSchema.h"
#include "Project.h"

#include "LogCapture.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <set>
#include <string>

namespace StreamAtmosphereTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::test;

namespace {

    std::set<std::string> streamAtmosphereKeys(const YAML::Node& node){
        std::set<std::string> out;
        for (auto it = node.begin(); it != node.end(); ++it)
            out.insert(it->first.as<std::string>());
        return out;
    }

    FogComponent everyHeightFogField(){
        FogComponent fog;
        fog.type = FogType::HEIGHT;
        fog.colorMode = FogColorMode::SKY;
        fog.attenuationDistance = 123.0f;
        fog.baseHeight = -4.5f;
        fog.maximumHeight = 77.0f;
        fog.startDistance = 12.0f;
        fog.maxFogDistance = 900.0f;
        fog.maxOpacity = 0.75f;
        fog.directionalInscattering = 0.9f;
        fog.anisotropy = -0.3f;
        fog.skyFill = false;
        fog.skyFillAngle = 7.0f;
        fog.volumetric = true;
        fog.volumetricDistance = 48.0f;
        fog.lightShafts = false;
        fog.noiseIntensity = 0.4f;
        fog.noiseScale = 13.0f;
        fog.wind = Vector3(1.0f, 0.0f, -2.0f);
        fog.qualityOverride = AtmosphereQualityOverride::MEDIUM;
        return fog;
    }

}

TEST(StreamAtmosphere, SerializeHeightFogRoundTrips){
    const FogComponent fog = everyHeightFogField();
    const FogComponent back = reflect::decodeValue<FogComponent>(ComponentType::FogComponent, YAML::Load(YAML::Dump(reflect::encodeValue(ComponentType::FogComponent, &fog))));
    EXPECT_EQ(back.type, FogType::HEIGHT);
    EXPECT_EQ(back.colorMode, FogColorMode::SKY);
    EXPECT_FLOAT_EQ(back.attenuationDistance, 123.0f);
    EXPECT_FLOAT_EQ(back.baseHeight, -4.5f);
    EXPECT_FLOAT_EQ(back.maximumHeight, 77.0f);
    EXPECT_FLOAT_EQ(back.startDistance, 12.0f);
    EXPECT_FLOAT_EQ(back.maxFogDistance, 900.0f);
    EXPECT_FLOAT_EQ(back.maxOpacity, 0.75f);
    EXPECT_FLOAT_EQ(back.directionalInscattering, 0.9f);
    EXPECT_FLOAT_EQ(back.anisotropy, -0.3f);
    EXPECT_FALSE(back.skyFill);
    EXPECT_FLOAT_EQ(back.skyFillAngle, 7.0f);
    EXPECT_TRUE(back.volumetric);
    EXPECT_FLOAT_EQ(back.volumetricDistance, 48.0f);
    EXPECT_FALSE(back.lightShafts);
    EXPECT_FLOAT_EQ(back.noiseIntensity, 0.4f);
    EXPECT_FLOAT_EQ(back.noiseScale, 13.0f);
    EXPECT_TRUE(back.wind == Vector3(1.0f, 0.0f, -2.0f));
    EXPECT_EQ(back.qualityOverride, AtmosphereQualityOverride::MEDIUM);
    EXPECT_EQ(YAML::Dump(reflect::encodeValue(ComponentType::FogComponent, &back)), YAML::Dump(reflect::encodeValue(ComponentType::FogComponent, &fog)));
    EXPECT_NO_ENGINE_ERRORS();
}

TEST(StreamAtmosphere, SerializeDistanceFogWritesItsOldKeys){
    FogComponent fog;
    fog.type = FogType::EXPONENTIAL;
    fog.density = 0.05f;
    const std::set<std::string> written = streamAtmosphereKeys(reflect::encodeValue(ComponentType::FogComponent, &fog));
    EXPECT_EQ(written, (std::set<std::string>{"type", "color", "density", "linearStart", "linearEnd"}));
}

TEST(StreamAtmosphere, SerializeOldFogLoadsWithTheDefaults){
    // a Fog as scenes wrote it before the atmosphere
    const YAML::Node old = YAML::Load("{type: linear, color: [0.5, 0.5, 0.5], density: 0.01, linearStart: 10, linearEnd: 60}");
    const FogComponent fog = reflect::decodeValue<FogComponent>(ComponentType::FogComponent, old);
    const FogComponent defaults;
    EXPECT_EQ(fog.type, FogType::LINEAR);
    EXPECT_EQ(fog.colorMode, defaults.colorMode);
    EXPECT_FLOAT_EQ(fog.attenuationDistance, defaults.attenuationDistance);
    EXPECT_FLOAT_EQ(fog.maximumHeight, defaults.maximumHeight);
    EXPECT_EQ(fog.skyFill, defaults.skyFill);
    EXPECT_EQ(fog.volumetric, defaults.volumetric);
    EXPECT_EQ(fog.qualityOverride, defaults.qualityOverride);

    // decoded over a height fog (paste, play-stop restore), the missing keys are the defaults
    const FogComponent height = everyHeightFogField();
    const FogComponent restored = reflect::decodeValue<FogComponent>(ComponentType::FogComponent, old, &height);
    EXPECT_EQ(restored.type, FogType::LINEAR);
    EXPECT_FALSE(restored.volumetric);
    EXPECT_FLOAT_EQ(restored.attenuationDistance, defaults.attenuationDistance);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST(StreamAtmosphere, SerializeLocalVolumetricFogRoundTrips){
    LocalVolumetricFogComponent volume;
    volume.shape = FogVolumeShape::SPHERE;
    volume.size = Vector3(2.0f, 3.0f, 4.0f);
    volume.attenuationDistance = 5.5f;
    volume.albedo = Vector3(0.9f, 0.8f, 0.7f);
    volume.blendDistance = 0.25f;
    volume.falloff = FogVolumeFalloff::EXPONENTIAL;
    volume.noiseIntensity = 0.3f;
    volume.noiseScale = 2.0f;
    volume.scrollSpeed = Vector3(0.0f, 1.0f, 0.0f);
    const LocalVolumetricFogComponent back =
        reflect::decodeValue<LocalVolumetricFogComponent>(ComponentType::LocalVolumetricFogComponent, YAML::Load(YAML::Dump(reflect::encodeValue(ComponentType::LocalVolumetricFogComponent, &volume))));
    EXPECT_EQ(back.shape, FogVolumeShape::SPHERE);
    EXPECT_TRUE(back.size == volume.size);
    EXPECT_FLOAT_EQ(back.attenuationDistance, 5.5f);
    EXPECT_TRUE(back.albedo == volume.albedo);
    EXPECT_FLOAT_EQ(back.blendDistance, 0.25f);
    EXPECT_EQ(back.falloff, FogVolumeFalloff::EXPONENTIAL);
    EXPECT_FLOAT_EQ(back.noiseIntensity, 0.3f);
    EXPECT_FLOAT_EQ(back.noiseScale, 2.0f);
    EXPECT_TRUE(back.scrollSpeed == volume.scrollSpeed);
    EXPECT_EQ(YAML::Dump(reflect::encodeValue(ComponentType::LocalVolumetricFogComponent, &back)), YAML::Dump(reflect::encodeValue(ComponentType::LocalVolumetricFogComponent, &volume)));
}

TEST(StreamAtmosphere, SerializeVolumetricCloudsRoundTrips){
    VolumetricCloudsComponent clouds;
    clouds.preset = CloudsPreset::STORMY;
    clouds.rendering = CloudsRendering::SCREEN;
    clouds.coverage = 0.71f;
    clouds.density = 0.9f;
    clouds.shapeScale = 3.5f;
    clouds.shapeFactor = 0.6f;
    clouds.erosionFactor = 0.4f;
    clouds.erosionScale = 80.0f;
    clouds.bottomAltitude = 900.0f;
    clouds.altitudeRange = 3000.0f;
    clouds.earthCurvature = 0.5f;
    clouds.windDirection = 45.0f;
    clouds.windSpeed = 25.0f;
    clouds.weatherMapScale = 12.0f;
    clouds.sunLightDimmer = 0.8f;
    clouds.ambientLightDimmer = 1.2f;
    clouds.maxDistance = 60.0f;
    clouds.qualityOverride = AtmosphereQualityOverride::LOW;
    const VolumetricCloudsComponent back =
        reflect::decodeValue<VolumetricCloudsComponent>(ComponentType::VolumetricCloudsComponent, YAML::Load(YAML::Dump(reflect::encodeValue(ComponentType::VolumetricCloudsComponent, &clouds))));
    EXPECT_EQ(back.preset, CloudsPreset::STORMY);
    EXPECT_EQ(back.rendering, CloudsRendering::SCREEN);
    EXPECT_FLOAT_EQ(back.coverage, 0.71f);
    EXPECT_FLOAT_EQ(back.bottomAltitude, 900.0f);
    EXPECT_FLOAT_EQ(back.windSpeed, 25.0f);
    EXPECT_FLOAT_EQ(back.maxDistance, 60.0f);
    EXPECT_EQ(back.qualityOverride, AtmosphereQualityOverride::LOW);
    EXPECT_EQ(YAML::Dump(reflect::encodeValue(ComponentType::VolumetricCloudsComponent, &back)), YAML::Dump(reflect::encodeValue(ComponentType::VolumetricCloudsComponent, &clouds)));
}

TEST(StreamAtmosphere, SerializeLightVolumetricIntensity){
    LightComponent light;
    // omitted at its default: old scenes' lights are written as they were
    EXPECT_FALSE(reflect::encodeValue(ComponentType::LightComponent, &light)["volumetricIntensity"].IsDefined());
    light.volumetricIntensity = 2.5f;
    const LightComponent back = reflect::decodeValue<LightComponent>(ComponentType::LightComponent, YAML::Load(YAML::Dump(reflect::encodeValue(ComponentType::LightComponent, &light))));
    EXPECT_FLOAT_EQ(back.volumetricIntensity, 2.5f);
}

// docs/specs/S101-vapor.md R12, R39: the Lit colour mode's fields are written only away from their
// defaults (a scene without Lit is written as it was) and read back when set
TEST(StreamAtmosphere, SerializeLitFieldsOmitIfDefault){
    FogComponent fog = everyHeightFogField();
    const YAML::Node plain = reflect::encodeValue(ComponentType::FogComponent, &fog);
    for (const char* key : {"albedo", "ambientMultiplier", "emissive"})
        EXPECT_FALSE(plain[key].IsDefined()) << key;

    fog.colorMode = FogColorMode::LIT;
    fog.albedo = Vector3(0.05f, 0.4f, 0.1f);
    fog.ambientMultiplier = 3.0f;
    fog.emissive = Vector3(2.5f, 0.0f, 0.75f);
    const YAML::Node lit = reflect::encodeValue(ComponentType::FogComponent, &fog);
    EXPECT_EQ(lit["colorMode"].as<std::string>(), "lit");
    for (const char* key : {"albedo", "ambientMultiplier", "emissive"})
        EXPECT_TRUE(lit[key].IsDefined()) << key;
    const FogComponent back = reflect::decodeValue<FogComponent>(ComponentType::FogComponent, YAML::Load(YAML::Dump(lit)));
    EXPECT_EQ(back.colorMode, FogColorMode::LIT);
    EXPECT_TRUE(back.albedo == fog.albedo);
    EXPECT_FLOAT_EQ(back.ambientMultiplier, 3.0f);
    EXPECT_TRUE(back.emissive == fog.emissive);
    EXPECT_EQ(YAML::Dump(reflect::encodeValue(ComponentType::FogComponent, &back)), YAML::Dump(lit));

    // an old scene's Fog loads with the defaults
    const FogComponent old = reflect::decodeValue<FogComponent>(ComponentType::FogComponent, YAML::Load("{type: height, colorMode: color}"));
    const FogComponent defaults;
    EXPECT_TRUE(old.albedo == defaults.albedo);
    EXPECT_FLOAT_EQ(old.ambientMultiplier, defaults.ambientMultiplier);
    EXPECT_TRUE(old.emissive == defaults.emissive);
    EXPECT_NO_ENGINE_ERRORS();
}

TEST(StreamAtmosphere, SchemaKnowsTheNewKeysAndNames){
    for (const char* key : {"colorMode", "albedo", "ambientMultiplier", "emissive", "attenuationDistance", "volumetric", "qualityOverride", "wind"})
        EXPECT_TRUE(StreamSchema::isKnownFieldKey(ComponentType::FogComponent, key)) << key;
    EXPECT_TRUE(StreamSchema::isKnownFieldKey(ComponentType::LocalVolumetricFogComponent, "blendDistance"));
    EXPECT_TRUE(StreamSchema::isKnownFieldKey(ComponentType::VolumetricCloudsComponent, "weatherMap"));
    EXPECT_TRUE(StreamSchema::isKnownFieldKey(ComponentType::LightComponent, "volumetricIntensity"));
    FogType type;
    EXPECT_TRUE(StreamSchema::fogType().find("height", type));
    EXPECT_EQ(type, FogType::HEIGHT);
    CloudsRendering rendering;
    EXPECT_TRUE(StreamSchema::cloudsRendering().find("sky_map", rendering));
    EXPECT_TRUE(StreamSchema::isKnownRootKey(RootKind::Project, "atmosphereQuality"));
}

TEST(StreamAtmosphere, FogProfileFieldsOmitIfDefault){
    // a Fog without a profile writes what it always wrote
    FogComponent fog;
    EXPECT_FALSE(reflect::encodeValue(ComponentType::FogComponent, &fog)["profile"].IsDefined());
    fog.type = FogType::HEIGHT;
    fog.attenuationDistance = 50.0f;
    EXPECT_FALSE(reflect::encodeValue(ComponentType::FogComponent, &fog)["profile"].IsDefined());

    // linked: the reference round-trips, GUID and path
    const Guid guid = Guid::parse("0123456789abcdef0123456789abcdef");
    fog.profile.set("FogProfiles/night.fogprofile", guid);
    const YAML::Node linked = reflect::encodeValue(ComponentType::FogComponent, &fog);
    ASSERT_TRUE(linked["profile"].IsDefined());
    const FogComponent back = reflect::decodeValue<FogComponent>(ComponentType::FogComponent, YAML::Load(YAML::Dump(linked)));
    EXPECT_EQ(back.profile.path, "FogProfiles/night.fogprofile");
    EXPECT_EQ(back.profile.guid, guid);
    EXPECT_FLOAT_EQ(back.attenuationDistance, 50.0f);
    EXPECT_EQ(YAML::Dump(reflect::encodeValue(ComponentType::FogComponent, &back)), YAML::Dump(linked));

    // an old scene's Fog loads unlinked, its fields as they were (R41)
    const YAML::Node old = YAML::Load("{type: height, color: [0.5, 0.5, 0.5], density: 0.01, linearStart: 10, linearEnd: 60, "
                                      "attenuationDistance: 40, maximumHeight: 6}");
    const FogComponent oldFog = reflect::decodeValue<FogComponent>(ComponentType::FogComponent, old);
    EXPECT_FALSE(oldFog.profile.isSet());
    EXPECT_FLOAT_EQ(oldFog.attenuationDistance, 40.0f);
    EXPECT_FLOAT_EQ(oldFog.maximumHeight, 6.0f);

    // the Fog Zone: key "fogzone", its fields, the profile only when set
    const reflect::ComponentDesc* desc = reflect::findByKey("fogzone");
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->type, ComponentType::FogZoneComponent);
    FogZoneComponent zone;
    EXPECT_EQ(streamAtmosphereKeys(reflect::encodeValue(ComponentType::FogZoneComponent, &zone)),
              (std::set<std::string>{"shape", "size", "blendDistance", "priority", "weight"}));
    zone.shape = FogZoneShape::SPHERE;
    zone.size = Vector3(4.0f, 5.0f, 6.0f);
    zone.blendDistance = 0.0f;
    zone.priority = -3;
    zone.weight = 0.25f;
    zone.profile.set("FogProfiles/cave.fogprofile", guid);
    const YAML::Node zoneNode = reflect::encodeValue(ComponentType::FogZoneComponent, &zone);
    EXPECT_EQ(zoneNode["shape"].as<std::string>(), "sphere");
    const FogZoneComponent zoneBack = reflect::decodeValue<FogZoneComponent>(ComponentType::FogZoneComponent, YAML::Load(YAML::Dump(zoneNode)));
    EXPECT_EQ(zoneBack.shape, FogZoneShape::SPHERE);
    EXPECT_TRUE(zoneBack.size == zone.size);
    EXPECT_FLOAT_EQ(zoneBack.blendDistance, 0.0f);
    EXPECT_EQ(zoneBack.priority, -3);
    EXPECT_FLOAT_EQ(zoneBack.weight, 0.25f);
    EXPECT_EQ(zoneBack.profile.path, "FogProfiles/cave.fogprofile");
    EXPECT_EQ(zoneBack.profile.guid, guid);
    EXPECT_EQ(YAML::Dump(reflect::encodeValue(ComponentType::FogZoneComponent, &zoneBack)), YAML::Dump(zoneNode));
    // the defaults R40 names
    const FogZoneComponent defaults = reflect::decodeValue<FogZoneComponent>(ComponentType::FogZoneComponent, YAML::Load("{}"));
    EXPECT_EQ(defaults.shape, FogZoneShape::BOX);
    EXPECT_TRUE(defaults.size == Vector3(10.0f, 10.0f, 10.0f));
    EXPECT_FLOAT_EQ(defaults.blendDistance, 5.0f);
    EXPECT_EQ(defaults.priority, 0);
    EXPECT_FLOAT_EQ(defaults.weight, 1.0f);
    for (const char* key : {"shape", "size", "blendDistance", "priority", "weight", "profile"})
        EXPECT_TRUE(StreamSchema::isKnownFieldKey(ComponentType::FogZoneComponent, key)) << key;
    EXPECT_TRUE(StreamSchema::isKnownFieldKey(ComponentType::FogComponent, "profile"));
    EXPECT_NO_ENGINE_ERRORS();
}

}  // namespace StreamAtmosphereTest_cpp
