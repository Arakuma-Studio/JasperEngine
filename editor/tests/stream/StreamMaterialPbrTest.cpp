// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The materials and lighting values in files (docs/specs/S11-materials-lighting.md R23):
// Material::pbr as a nested map of the lobes away from their defaults, emissiveStrength, the
// light's unit and colour temperature, a terrain layer's hex tiling; all of them written only
// away from their defaults, so a file saved before them is written again byte for byte.

#include "Stream.h"
#include "component/TerrainComponent.h"
#include "reflect/Reflect.h"
#include "util/LightPhotometry.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <chrono>
#include <string>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {
    // through text, as a file round-trips
    YAML::Node reread(const YAML::Node& node){
        return YAML::Load(YAML::Dump(node));
    }
}

TEST(StreamMaterialPbr, DefaultsWriteNothingNew){
    const YAML::Node node = Stream::encodeMaterial(Material());
    EXPECT_FALSE(node["pbr"]);
    EXPECT_FALSE(node["emissiveStrength"]);
    // an allocated value at its defaults is no different
    Material allocated;
    allocated.pbr.edit();
    EXPECT_EQ(YAML::Dump(Stream::encodeMaterial(allocated)), YAML::Dump(node));

    const LightComponent light0;
    const YAML::Node light = reflect::encodeValue(ComponentType::LightComponent, &light0);
    EXPECT_FALSE(light["intensityUnit"]);
    EXPECT_FALSE(light["useColorTemperature"]);
    EXPECT_FALSE(light["colorTemperature"]);

    const YAML::Node layer = Stream::encodeTerrainSurfaceLayer(TerrainSurfaceLayer());
    EXPECT_FALSE(layer["hexTiling"]);
    EXPECT_FALSE(layer["hexRotation"]);
}

TEST(StreamMaterialPbr, OnlyChangedValuesAreWritten){
    Material material;
    MaterialPbrExtensions& pbr = material.pbr.edit();
    pbr.clearcoat.enabled = true;
    pbr.clearcoat.factor = 0.75f;
    pbr.specular.ior = 1.33f;
    material.emissiveStrength = 4.0f;
    const YAML::Node node = Stream::encodeMaterial(material);
    ASSERT_TRUE(node["pbr"]);
    EXPECT_EQ(node["pbr"].size(), 2u);
    EXPECT_EQ(node["pbr"]["clearcoat"].size(), 2u);
    EXPECT_TRUE(node["pbr"]["clearcoat"]["enabled"].as<bool>());
    EXPECT_FLOAT_EQ(node["pbr"]["clearcoat"]["factor"].as<float>(), 0.75f);
    EXPECT_EQ(node["pbr"]["specular"].size(), 1u);
    EXPECT_FLOAT_EQ(node["pbr"]["specular"]["ior"].as<float>(), 1.33f);
    EXPECT_FALSE(node["pbr"]["sheen"]);
    EXPECT_FLOAT_EQ(node["emissiveStrength"].as<float>(), 4.0f);
}

TEST(StreamMaterialPbr, EveryLobeRoundTrips){
    Material material;
    MaterialPbrExtensions& pbr = material.pbr.edit();
    pbr.clearcoat = {true, 0.5f, 0.25f, 0.75f, Texture("textures/cc.png"), Texture("textures/ccn.png"), 1, 0};
    pbr.sheen.enabled = true;
    pbr.sheen.color = Vector3(0.5f, 0.25f, 0.125f);
    pbr.sheen.roughness = 0.6f;
    pbr.anisotropy.enabled = true;
    pbr.anisotropy.strength = 0.4f;
    pbr.anisotropy.rotation = -1.25f;
    pbr.iridescence.enabled = true;
    pbr.iridescence.thicknessNm = Vector2(150.0f, 550.0f);
    pbr.transmission.enabled = true;
    pbr.transmission.factor = 0.9f;
    pbr.volume.enabled = true;
    pbr.volume.thickness = 0.2f;
    pbr.volume.attenuationDistance = 3.0f;
    pbr.volume.attenuationColor = Vector3(0.9f, 0.5f, 0.25f);
    pbr.specular.enabled = true;
    pbr.specular.factor = 0.5f;
    pbr.specular.color = Vector3(1.5f, 1.0f, 0.5f);
    pbr.hex.enabled = true;
    pbr.hex.rotation = 0.5f;
    pbr.hex.contrast = 0.25f;
    material.emissiveStrength = 2.5f;

    const Material read = Stream::decodeMaterial(reread(Stream::encodeMaterial(material, false)));
    EXPECT_TRUE(read.pbr == material.pbr);
    EXPECT_FLOAT_EQ(read.emissiveStrength, 2.5f);
    EXPECT_EQ(read.pbr->clearcoat.map.getPath(0), "textures/cc.png");
    EXPECT_EQ(read.pbr->clearcoat.mapTexCoord, 1);
}

TEST(StreamMaterialPbr, OutOfRangeReadsIntoRange){
    const YAML::Node node = YAML::Load("pbr:\n  clearcoat:\n    enabled: true\n    factor: 3\n    roughness: -1\n  specular:\n    ior: 0.5\n");
    const Material read = Stream::decodeMaterial(node);
    EXPECT_TRUE(read.pbr->clearcoat.enabled);
    EXPECT_FLOAT_EQ(read.pbr->clearcoat.factor, 1.0f);
    EXPECT_FLOAT_EQ(read.pbr->clearcoat.roughness, 0.0f);
    EXPECT_FLOAT_EQ(read.pbr->specular.ior, 1.0f);
}

TEST(StreamMaterialPbr, LightUnitsRoundTrip){
    LightComponent light;
    light.type = LightType::POINT;
    light.intensity = 800.0f;
    light.intensityUnit = LightUnit::LUMEN;
    light.useColorTemperature = true;
    light.colorTemperature = 2700.0f;
    const YAML::Node node = reread(reflect::encodeValue(ComponentType::LightComponent, &light));
    EXPECT_EQ(node["intensityUnit"].as<std::string>(), "lumen");
    const LightComponent read = reflect::decodeValue<LightComponent>(ComponentType::LightComponent, node);
    EXPECT_EQ(read.intensityUnit, LightUnit::LUMEN);
    EXPECT_TRUE(read.useColorTemperature);
    EXPECT_FLOAT_EQ(read.colorTemperature, 2700.0f);
    EXPECT_FLOAT_EQ(read.intensity, 800.0f);
    // a file without them reads the defaults (a light of before keeps its look)
    const LightComponent old = reflect::decodeValue<LightComponent>(ComponentType::LightComponent, YAML::Load("type: point\nintensity: 2\n"));
    EXPECT_EQ(old.intensityUnit, LightUnit::ENGINE);
    EXPECT_FALSE(old.useColorTemperature);
    EXPECT_FLOAT_EQ(old.colorTemperature, 6500.0f);
}

TEST(StreamMaterialPbr, TerrainHexRoundTrips){
    TerrainSurfaceLayer layer;
    layer.hexTiling = true;
    layer.hexRotation = 0.35f;
    const TerrainSurfaceLayer read = Stream::decodeTerrainSurfaceLayer(reread(Stream::encodeTerrainSurfaceLayer(layer)));
    EXPECT_TRUE(read.hexTiling);
    EXPECT_FLOAT_EQ(read.hexRotation, 0.35f);
}

// A map whose file is missing right now (its load failed) is empty() but keeps its path: a save, a
// copy or an undo snapshot writes the reference, and a saved file does not depend on what was loaded
// (S04 R15). A terrain layer's maps and a lobe's map wrote nothing then, and the file lost them.
namespace {
    void failLoadOf(Texture& texture){
        TextureLoadResult result = texture.load();
        for (int i = 0; i < 2000 && result.state == ResourceLoadState::Loading; i++){
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            result = texture.load();
        }
        ASSERT_EQ(result.state, ResourceLoadState::Failed);
        ASSERT_TRUE(texture.empty());
    }
}

TEST(StreamMaterialPbr, ATerrainLayersMapThatFailedToLoadKeepsItsFile){
    const std::string missing = "assets/bf19-does-not-exist/layer.png";
    TerrainSurfaceLayer layer;
    layer.pbr = true;
    forEachTerrainLayerTexture(layer, [&](Texture& texture){ texture.setPath(missing); });
    const std::string before = YAML::Dump(Stream::encodeTerrainSurfaceLayer(layer));

    forEachTerrainLayerTexture(layer, [&](Texture& texture){ failLoadOf(texture); });
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(YAML::Dump(Stream::encodeTerrainSurfaceLayer(layer)), before);

    TerrainSurfaceLayer read = Stream::decodeTerrainSurfaceLayer(reread(Stream::encodeTerrainSurfaceLayer(layer)));
    forEachTerrainLayerTexture(read, [&](Texture& texture){ EXPECT_EQ(texture.getPath(), missing); });
}

TEST(StreamMaterialPbr, ALobesMapThatFailedToLoadKeepsItsFile){
    const std::string missing = "assets/bf19-does-not-exist/coat.png";
    Material material;
    MaterialPbrExtensions& pbr = material.pbr.edit();
    pbr.clearcoat.enabled = true;
    pbr.clearcoat.map.setPath(missing);
    const std::string before = YAML::Dump(Stream::encodeMaterial(material));

    failLoadOf(material.pbr.edit().clearcoat.map);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(YAML::Dump(Stream::encodeMaterial(material)), before);

    const Material read = Stream::decodeMaterial(reread(Stream::encodeMaterial(material)));
    EXPECT_EQ(read.pbr.get().clearcoat.map.getPath(), missing);
}
