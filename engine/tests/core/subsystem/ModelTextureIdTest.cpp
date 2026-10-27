// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The ids of a model's textures, which scenes store: keyed by the model's path in the assets
// directory, and the absolute ones scenes were saved with before migrated on load.

#include "subsystem/MeshSystem.h"

#include "TempDir.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::test;

TEST(ModelTextureId, AnAbsolutePathInsideTheAssetsBecomesRelative){
    TempDir assets;
    const std::string id = assets.str() + "/models/ship.gltf|gltf-image|0|s0";
    EXPECT_EQ(MeshSystem::migrateModelTextureId(id, assets.str()), "models/ship.gltf|gltf-image|0|s0");
}

TEST(ModelTextureId, AProjectMovedSinceFindsTheModelByItsPathTail){
    TempDir assets;
    assets.write("models/ship.gltf", "{}");
    // saved on another machine, in another folder, its assets directory named differently
    EXPECT_EQ(MeshSystem::migrateModelTextureId("/home/someone/old/Game/content/models/ship.gltf|gltf-image|2|s-1", assets.str()),
              "models/ship.gltf|gltf-image|2|s-1");
    EXPECT_EQ(MeshSystem::migrateModelTextureId("C:/Users/someone/Game/assets/models/ship.gltf|gltf-image|0|s0", assets.str()),
              "models/ship.gltf|gltf-image|0|s0");
}

TEST(ModelTextureId, OtherIdsAreKept){
    TempDir assets;
    EXPECT_EQ(MeshSystem::migrateModelTextureId("models/ship.gltf|gltf-image|0|s0", assets.str()), "models/ship.gltf|gltf-image|0|s0");
    EXPECT_EQ(MeshSystem::migrateModelTextureId("camera:12", assets.str()), "camera:12");
    // a model that is not in this project: nothing to point the id at
    EXPECT_EQ(MeshSystem::migrateModelTextureId("/elsewhere/gone.gltf|gltf-image|0|s0", assets.str()),
              "/elsewhere/gone.gltf|gltf-image|0|s0");
    EXPECT_EQ(MeshSystem::migrateModelTextureId("/abs/models/ship.gltf|gltf-image|0|s0", ""), "/abs/models/ship.gltf|gltf-image|0|s0");
}
