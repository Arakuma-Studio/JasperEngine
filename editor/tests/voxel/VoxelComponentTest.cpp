// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R1, R19, R80 (AC1, AC18, AC55): the Voxel Volume in a scene file, and what is never in one

#include "support/EditorTestSupport.h"

#include "asset/AssetType.h"
#include "asset/AssetTypes.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "reflect/Components.h"
#include "subsystem/VoxelSystem.h"
#include "voxel/VoxelGenerator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace voxel_component_test {

    constexpr const char* kHeader = "formatVersion: 1\nid: 4242\nname: Voxels\nscene:\n  backgroundColor: [0, 0, 0, 1]\nsceneType: scene_3d\n"
                                    "mainCamera: 0\nentities:\n";

    std::string withVolume(const std::string& fields) {
        return std::string(kHeader) + "  - type: Entity\n    entity: 1001\n    name: Cave\n    components:\n      transform:\n        position: [0, 0, 0]\n"
               "      voxelvolume:" + fields;
    }

}

using namespace voxel_component_test;

TEST(VoxelComponent, DefaultsAndRoundTrip) {
    // R1's defaults
    const VoxelVolumeComponent defaults;
    EXPECT_TRUE(defaults.enabled);
    EXPECT_EQ(defaults.quality, VoxelQuality::Auto);
    EXPECT_EQ(defaults.lodLevels, 0);
    EXPECT_EQ(defaults.lodRadius, 4);
    EXPECT_EQ(defaults.maxDrawDistance, 0.0f);
    EXPECT_TRUE(defaults.castShadows);
    EXPECT_TRUE(defaults.collision);
    EXPECT_EQ(defaults.collisionRadius, 0);
    EXPECT_TRUE(defaults.rescueBodies);
    EXPECT_EQ(defaults.navigationUpdate, VoxelNavigationUpdate::Automatic);
    EXPECT_EQ(defaults.terrain, NULL_ENTITY);
    EXPECT_EQ(defaults.terrainCut, VoxelTerrainCut::Off);
    EXPECT_EQ(defaults.focus, NULL_ENTITY);
    EXPECT_FALSE(defaults.runtimeGeneration);

    // values at their defaults are not written: the whole component is an empty map
    const reflect::ComponentDesc* desc = reflect::find(ComponentType::VoxelVolumeComponent);
    ASSERT_NE(desc, nullptr);
    EXPECT_STREQ(desc->displayName, "Voxel Volume");
    DecodedScene empty = decodeSceneText(withVolume(" {}\n"), "Voxels.scene");
    const YAML::Node encoded = encodeScene(empty);
    const YAML::Node component = encoded["entities"][0]["components"]["voxelvolume"];
    ASSERT_TRUE(component.IsDefined());
    EXPECT_EQ(component.size(), 0u) << encodeSceneText(empty);

    // every field away from its default reads back and writes byte-identically
    const std::string fields = "\n        volumeData: voxels/cave.voxelvolume\n"
                               "        materialSlots:\n"
                               "          - name: Rock\n"
                               "            layer:\n"
                               "              roughnessFactor: 0.25\n"
                               "            linkedTerrainLayer: 2\n"
                               "        quality: medium\n        lodLevels: 3\n        lodRadius: 6\n        maxDrawDistance: 120\n"
                               "        castShadows: false\n        collision: false\n        collisionRadius: 2\n        rescueBodies: false\n"
                               "        navigationUpdate: off\n        terrainCut: automatic\n        overlapDepth: 0.5\n        cutTolerance: 0.75\n"
                               "        runtimeGeneration: true\n";
    DecodedScene decoded = decodeSceneText(withVolume(fields), "Voxels.scene");
    Scene* scene = decoded.scene->scene;
    ASSERT_FALSE(decoded.scene->entities.empty());
    const VoxelVolumeComponent* volume = scene->findComponent<VoxelVolumeComponent>(decoded.scene->entities.front());
    ASSERT_NE(volume, nullptr);
    EXPECT_EQ(volume->volumeData.path, "voxels/cave.voxelvolume");
    ASSERT_EQ(volume->materialSlots.size(), 1u);
    EXPECT_EQ(volume->materialSlots[0].name, "Rock");
    EXPECT_FLOAT_EQ(volume->materialSlots[0].layer.roughnessFactor, 0.25f);
    EXPECT_EQ(volume->materialSlots[0].linkedTerrainLayer, 2);
    EXPECT_EQ(volume->quality, VoxelQuality::Medium);
    EXPECT_EQ(volume->lodLevels, 3);
    EXPECT_EQ(volume->lodRadius, 6);
    EXPECT_FLOAT_EQ(volume->maxDrawDistance, 120.0f);
    EXPECT_FALSE(volume->castShadows);
    EXPECT_FALSE(volume->collision);
    EXPECT_EQ(volume->collisionRadius, 2);
    EXPECT_FALSE(volume->rescueBodies);
    EXPECT_EQ(volume->navigationUpdate, VoxelNavigationUpdate::Off);
    EXPECT_EQ(volume->terrainCut, VoxelTerrainCut::Automatic);
    EXPECT_FLOAT_EQ(volume->overlapDepth, 0.5f);
    EXPECT_FLOAT_EQ(volume->cutTolerance, 0.75f);
    EXPECT_TRUE(volume->runtimeGeneration);

    const std::string once = encodeSceneText(decoded);
    DecodedScene again = decodeSceneText(once, "Voxels.scene");
    EXPECT_EQ(encodeSceneText(again), once);
}

// R1's values hold whatever a hand-edited file says
TEST(VoxelComponent, DecodeKeepsTheValuesInRange) {
    DecodedScene decoded = decodeSceneText(withVolume("\n        lodLevels: 9\n        lodRadius: 5\n        collisionRadius: -3\n"), "Voxels.scene");
    const VoxelVolumeComponent* volume = decoded.scene->scene->findComponent<VoxelVolumeComponent>(decoded.scene->entities.front());
    ASSERT_NE(volume, nullptr);
    EXPECT_EQ(volume->lodLevels, 4);
    EXPECT_EQ(volume->lodRadius, 6);
    EXPECT_EQ(volume->collisionRadius, 0);
}

// AC18: chunks are the engine's: not in the hierarchy, not in the saved scene
TEST(VoxelSystem, ChunksNotSavedNotInHierarchy) {
    DecodedScene decoded = decodeSceneText(withVolume(" {volumeData: 'mem://chunks.voxelvolume', collision: false, navigationUpdate: off}\n"), "Voxels.scene");
    Scene* scene = decoded.scene->scene;
    voxel::VolumeDesc desc;
    desc.dimensions = {64, 64, 64};
    desc.levels = 2;
    voxel::GeneratorDesc generator;
    generator.kind = "solid_ground";
    generator.height = 32.0;
    std::shared_ptr<VoxelAsset> asset = VoxelAssets::create("mem://chunks.voxelvolume", desc, generator, 1);
    const std::string before = encodeSceneText(decoded);
    const std::vector<Entity> hierarchy = decoded.scene->entities;

    std::shared_ptr<VoxelSystem> system = scene->getSystem<VoxelSystem>();
    ASSERT_NE(system, nullptr);
    const Entity owner = hierarchy.front();
    for (int frame = 0; frame < 8 && !system->isVolumeReady(owner); frame++) {
        system->update(1.0 / 60.0);
        system->finishPendingWork();
    }
    const std::vector<Entity> chunks = system->getGeneratedEntities(owner);
    ASSERT_FALSE(chunks.empty()) << "the red control: no chunk was made, so nothing here was tested";
    for (Entity chunk : chunks) {
        EXPECT_TRUE(scene->isEntityCreated(chunk));
        EXPECT_NE(scene->findComponent<MeshComponent>(chunk), nullptr);
        EXPECT_EQ(std::find(decoded.scene->entities.begin(), decoded.scene->entities.end(), chunk), decoded.scene->entities.end());
    }
    EXPECT_EQ(decoded.scene->entities, hierarchy);
    EXPECT_EQ(encodeSceneText(decoded), before);
}

TEST(AssetTypes, VoxelVolumeValueIs31) {
    // a file format (assetmap.bin): the number and the name stay, whichever stream lands first
    EXPECT_EQ(static_cast<int>(AssetType::VoxelVolume), 31);
    EXPECT_STREQ(assetTypeName(AssetType::VoxelVolume), "voxel_volume");
    EXPECT_EQ(assetTypeFromName("voxel_volume"), AssetType::VoxelVolume);
    // no other number takes the name (the ones before it belong to other streams, D22)
    for (int value = 0; value < 64; value++) {
        if (value != 31) EXPECT_STRNE(assetTypeName(static_cast<AssetType>(value)), "voxel_volume") << value;
    }
    const AssetTypeInfo* info = AssetTypes::find("assets/caves/a.voxelvolume", "assets");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->type, AssetType::VoxelVolume);
    EXPECT_EQ(info->identity, AssetIdentity::Sidecar);
    EXPECT_EQ(info->base, AssetPathBase::Assets);
}
