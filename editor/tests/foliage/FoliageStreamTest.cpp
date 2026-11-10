// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T5 AC39, AC44: what a scene file and the generated code keep of foliage, and the values nothing may
// renumber. A scene from before T5 loads with defaults that change nothing, and saves the same version.

#include "Factory.h"
#include "Project.h"
#include "Scene.h"
#include "Stream.h"
#include "asset/AssetTypes.h"
#include "component/TerrainComponent.h"
#include "object/Terrain.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    TerrainComponent decode(const std::string& text) {
        return Stream::decodeTerrainComponent(YAML::Load(text));
    }

}

TEST(StreamTerrainFoliage, OldSceneLoadsWithDefaults) {
    // the keys of a terrain as a scene saved it before T5
    const TerrainComponent terrain = decode(R"(
terrainSize: 256
maxHeight: 40
foliageLayers:
  - meshPath: grass.glb
    density: 2
    drawDistance: 35
    seed: 9
)");
    EXPECT_FLOAT_EQ(terrain.detailDistance, 0.0f) << "an old layer keeps its own distance";
    EXPECT_FLOAT_EQ(terrain.detailDensity, 1.0f);
    EXPECT_TRUE(terrain.treePrototypes.empty());
    EXPECT_TRUE(terrain.treeData.empty());
    ASSERT_EQ(terrain.foliageLayers.size(), 1u);
    const TerrainFoliageLayer& layer = terrain.foliageLayers[0];
    EXPECT_FLOAT_EQ(layer.minDrawDistance, 0.0f);
    EXPECT_FLOAT_EQ(layer.windBend, 0.0f) << "no wind, so the no-wind variant";
    EXPECT_FLOAT_EQ(layer.densityFalloffStart, 1.0f);
    EXPECT_FLOAT_EQ(layer.farDensity, 1.0f);
    EXPECT_EQ(layer.algorithmVersion, 1u);
    EXPECT_FLOAT_EQ(layer.healthyColor.x, 1.0f);
    EXPECT_FLOAT_EQ(layer.dryColor.x, 1.0f);
}

TEST(StreamTerrainFoliage, SaveKeepsTheSceneVersion) {
    // T5's keys are optional: nothing they mean changed, so no version bump and no migration
    TerrainComponent terrain;
    const YAML::Node node = Stream::encodeTerrainComponent(terrain);
    EXPECT_FALSE(node["treePrototypes"].IsDefined() && node["treePrototypes"].size() > 0);
    EXPECT_FALSE(node["version"].IsDefined());
}

TEST(StreamTerrainFoliage, SaveAndLoadRoundTrip) {
    TerrainComponent terrain;
    terrain.detailDistance = 60.0f;
    terrain.detailDensity = 0.5f;
    terrain.treeDistance = 500.0f;
    terrain.billboardStart = 70.0f;
    terrain.fadeLength = 3.0f;
    terrain.maxMeshTrees = 42;
    terrain.windDirection = Vector3(1.0f, 0.0f, 0.0f);
    terrain.windSpeed = 2.0f;
    terrain.windSize = 11.0f;
    terrain.windBending = 0.75f;
    terrain.treeData = AssetRef("terrain/island.jetrees");

    TerrainFoliageLayer layer;
    layer.minDrawDistance = 12.0f;
    layer.minHeightRatio = 0.8f;
    layer.maxHeightRatio = 1.2f;
    layer.healthyColor = Vector4(0.2f, 0.8f, 0.3f, 1.0f);
    layer.dryColor = Vector4(0.7f, 0.6f, 0.2f, 1.0f);
    layer.noiseSpread = 0.1f;
    layer.windBend = 0.6f;
    layer.densityFalloffStart = 0.5f;
    layer.farDensity = 0.25f;
    layer.densityScale = 0.75f;
    layer.algorithmVersion = 1;
    terrain.foliageLayers.push_back(layer);

    TerrainTreePrototype prototype;
    prototype.id = 7;
    prototype.meshPath = AssetRef("trees/pine.glb");
    prototype.lodDistances = {20.0f, 40.0f};
    prototype.bendFactor = 0.25f;
    prototype.windMask = TerrainTreeWindMask::VertexColor;
    prototype.shadowLodMask = 0x83;
    prototype.collision = TerrainTreeCollision::Capsule;
    prototype.collisionRadius = 0.75f;
    prototype.collisionHeight = 6.0f;
    prototype.navObstacle = true;
    prototype.impostorViews = 8;
    prototype.impostorBoundsCenter = Vector3(0.0f, 4.0f, 0.0f);
    prototype.impostorSize = Vector2(5.0f, 9.0f);
    prototype.impostorSourceHash = "abc";
    terrain.treePrototypes.push_back(prototype);

    const YAML::Node first = Stream::encodeTerrainComponent(terrain);
    const TerrainComponent loaded = Stream::decodeTerrainComponent(first);
    EXPECT_EQ(YAML::Dump(Stream::encodeTerrainComponent(loaded)), YAML::Dump(first));
    ASSERT_EQ(loaded.treePrototypes.size(), 1u);
    EXPECT_EQ(loaded.treePrototypes[0].id, 7u);
    EXPECT_EQ(loaded.treePrototypes[0].shadowLodMask, 0x83u);
    EXPECT_EQ(loaded.treePrototypes[0].collision, TerrainTreeCollision::Capsule);
    EXPECT_FLOAT_EQ(loaded.foliageLayers[0].minDrawDistance, 12.0f);
    EXPECT_EQ(loaded.treeData.str(), "terrain/island.jetrees");
}

TEST(StreamTerrainFoliage, FactoryEmitsTheNewFields) {
    TerrainComponent terrain;
    terrain.treeDistance = 321.0f;
    terrain.maxMeshTrees = 77;
    TerrainTreePrototype prototype;
    prototype.id = 3;
    prototype.collisionRadius = 0.75f;
    terrain.treePrototypes.push_back(prototype);
    TerrainFoliageLayer layer;
    layer.windBend = 0.6f;
    terrain.foliageLayers.push_back(layer);
    Scene scene;
    Terrain terrainObject(&scene);
    const Entity entity = terrainObject.getEntity();
    scene.getComponent<TerrainComponent>(entity) = terrain;
    const std::string code = Factory::createTerrainComponent(0, &scene, entity, std::filesystem::temp_directory_path());
    EXPECT_NE(code.find("treeDistance"), std::string::npos) << code;
    EXPECT_NE(code.find("maxMeshTrees"), std::string::npos);
    EXPECT_NE(code.find("treePrototypes"), std::string::npos);
    EXPECT_NE(code.find("collisionRadius"), std::string::npos);
    EXPECT_NE(code.find("windBend"), std::string::npos);
}

TEST(AssetTypes, TreeDataValueIs30) {
    EXPECT_EQ(static_cast<int>(AssetType::TreeData), 30);
}

// R61: the old flag still means Instanced, and Place As is a key of its own
TEST(TerrainSettingsStream, PlaceInstancedKeepsItsMeaning) {
    YAML::Node node;
    node["terrain"]["placeInstanced"] = false;
    TerrainEditorSettings settings = Stream::decodeTerrainEditorSettings(node["terrain"]);
    EXPECT_FALSE(settings.placeInstanced);
    EXPECT_TRUE(settings.placeAsTree) << "an old project's panel opens on the store's brush";

    node["terrain"]["placeInstanced"] = true;
    node["terrain"]["placeAs"] = "gameobject";
    settings = Stream::decodeTerrainEditorSettings(node["terrain"]);
    EXPECT_TRUE(settings.placeInstanced);
    EXPECT_FALSE(settings.placeAsTree);
}
