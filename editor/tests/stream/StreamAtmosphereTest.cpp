// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The atmosphere's scene data (docs/specs/S12-atmosphere.md AC15): every new field of Fog, Local
// Volumetric Fog, Volumetric Clouds and the Light's volumetric intensity round-trips; a distance
// fog writes exactly the keys it always wrote; an old scene's Fog loads with the new defaults;
// an older editor keeps a height fog's fields as unknown data (S03 preservation, no format
// version change: docs/data-safety.md).

#include "Stream.h"
#include "component/FogComponent.h"
#include "component/LightComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/VolumetricCloudsComponent.h"
#include "reflect/Reflect.h"
#include "StreamSchema.h"
#include "Project.h"

#include "LogCapture.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <set>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::test;

namespace {

    std::set<std::string> keys(const YAML::Node& node){
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
    const std::set<std::string> written = keys(reflect::encodeValue(ComponentType::FogComponent, &fog));
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

TEST(StreamAtmosphere, SchemaKnowsTheNewKeysAndNames){
    for (const char* key : {"colorMode", "attenuationDistance", "volumetric", "qualityOverride", "wind"})
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
