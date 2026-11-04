// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T5 AC23, AC34: the tree delta's undo: small in memory, one stroke one step, Esc cancels, and a convert
// is one step with its host.

#include "Project.h"
#include "Scene.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/TreeEditCmd.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "subsystem/FoliageSystem.h"
#include "support/EditorTestSupport.h"
#include "terrain/FoliageTypes.h"
#include "terrain/TreeStore.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::terrain;

namespace {

    struct TreeScene {
        Project project;
        uint32_t sceneId = NULL_PROJECT_SCENE;
        Entity entity = NULL_ENTITY;
        FoliageSystem* foliage = nullptr;
        std::shared_ptr<TreeStore> store;

        TreeScene() {
            test::useProjectPaths(&project);
            sceneId = project.createNewScene("Trees", SceneType::SCENE_3D);
            Scene* scene = project.getScene(sceneId)->scene;
            entity = scene->createEntity();
            scene->addComponent<Transform>(entity);
            scene->addComponent<TerrainComponent>(entity);
            TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
            terrain.terrainSize = 128.0f;
            TerrainTreePrototype prototype;
            prototype.id = 1;
            prototype.meshPath = AssetRef("t5-no-such-tree.obj");
            terrain.treePrototypes.push_back(prototype);
            foliage = scene->getSystem<FoliageSystem>().get();
            foliage->update(0.0);
            store = foliage->getOrCreateTreeStore(entity);
        }

        ~TreeScene() { test::closeProject(project); }

        // what a brush stamp makes
        TreeDelta stamp(int count, float at) {
            std::vector<TreeRecord> records;
            for (int i = 0; i < count; i++) {
                TreeRecord record{};
                record.u = at + static_cast<float>(i) * 0.001f;
                record.v = 0.5f;
                record.scale = 1.0f;
                record.rotation[3] = 32767;
                record.prototypeId = 1;
                record.heightRatio = 128;
                records.push_back(record);
            }
            TreeDelta delta;
            store->apply({}, records, {}, &delta);
            return delta;
        }
    };

}

TEST(TreeEditCmdTests, DeltaMemoryIsSmall) {
    TreeScene scene;
    // a few thousand records in the store; a stamp of 8 keeps 8
    scene.stamp(5000, 0.2f);
    TreeEditCmd command(&scene.project, scene.sceneId, scene.entity, "Paint Trees", scene.stamp(8, 0.7f));
    EXPECT_LE(command.memoryBytes(), sizeof(TreeEditCmd) + 64 + 8 * (sizeof(TreeRecord) + 16)) << "only the delta, not the store";
}

TEST(TreeEditCmdTests, UndoAndRedoRestoreTheBytes) {
    TreeScene scene;
    scene.stamp(3, 0.2f);
    std::vector<uint8_t> before;
    scene.store->encode(before, nullptr);
    TreeEditCmd command(&scene.project, scene.sceneId, scene.entity, "Paint Trees", scene.stamp(8, 0.7f));
    std::vector<uint8_t> after;
    scene.store->encode(after, nullptr);
    ASSERT_NE(before, after);
    command.undo();
    std::vector<uint8_t> undone;
    scene.store->encode(undone, nullptr);
    EXPECT_EQ(undone, before);
    ASSERT_TRUE(command.execute());
    std::vector<uint8_t> redone;
    scene.store->encode(redone, nullptr);
    EXPECT_EQ(redone, after);
}

TEST(TreeEditCmdTests, StrokeMergesAndEscCancels) {
    TreeScene scene;
    const size_t start = scene.store->size();
    TreeEditCmd first(&scene.project, scene.sceneId, scene.entity, "Paint Trees", scene.stamp(4, 0.2f), 11);
    TreeEditCmd second(&scene.project, scene.sceneId, scene.entity, "Paint Trees", scene.stamp(4, 0.3f), 11);
    TreeEditCmd otherStroke(&scene.project, scene.sceneId, scene.entity, "Paint Trees", scene.stamp(4, 0.4f), 12);

    EXPECT_TRUE(second.mergeWith(&first)) << "one stroke is one step";
    EXPECT_FALSE(otherStroke.mergeWith(&second));

    // Esc takes the whole stroke back
    otherStroke.undo();
    second.undo();
    EXPECT_EQ(scene.store->size(), start);
}

TEST(TreeEditCmdTests, ConvertHostIsOneUndoStep) {
    TreeScene scene;
    int hostRuns = 0;
    int hostUndos = 0;
    TreeEditCmd command(&scene.project, scene.sceneId, scene.entity, "Convert to Trees", scene.stamp(6, 0.5f));
    command.alongside = [&](bool undone) {
        (undone ? hostUndos : hostRuns)++;
    };
    const size_t after = scene.store->size();
    command.undo();
    EXPECT_EQ(hostUndos, 1);
    EXPECT_EQ(scene.store->size(), after - 6);
    ASSERT_TRUE(command.execute());
    EXPECT_EQ(hostRuns, 1);
    EXPECT_EQ(scene.store->size(), after);
}
