// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The names the editor gives new and copied GameObjects (S95 REQ-4, AC-5; ledger S15/S16): as
// Unity, a name is numbered ("Blue 2") only when a sibling under the same parent (or the scene
// root) has it. Two parents may each hold a "Blue". Create, duplicate and prefab import.

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/DuplicateEntityCmd.h"
#include "command/type/ImportPrefabCmd.h"
#include "component/Transform.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace EntityNamesTest_cpp {

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    class EntityNamesTest : public ::testing::Test {
    protected:
        jasperengine::test::TestTempDir dir{"entity-names"};
        std::unique_ptr<Project> project;
        SceneProject* scene = nullptr;

        void SetUp() override{
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            for (SceneProject& candidate : project->getScenes()){
                if (!candidate.filepath.empty() && !candidate.scene){
                    project->loadScene(candidate.filepath, false, false, true);
                }
            }
            for (SceneProject& candidate : project->getScenes()){
                if (candidate.scene && candidate.name == "Main Scene") scene = &candidate;
            }
            ASSERT_NE(scene, nullptr);
            project->setSelectedSceneId(scene->id);
        }

        void TearDown() override{
            if (project){
                for (SceneProject& candidate : project->getScenes()){
                    CommandHandle::remove(candidate.id);
                }
                project->releaseScenes();
            }
            project.reset();
        }

        CommandHistory* history(){ return CommandHandle::get(scene->id); }

        std::string name(Entity entity){ return scene->scene->getEntityName(entity); }

        Entity parentOf(Entity entity){
            const Transform* transform = scene->scene->findComponent<Transform>(entity);
            return transform ? transform->parent : NULL_ENTITY;
        }

        Entity create(const std::string& entityName, Entity parent = NULL_ENTITY){
            auto* cmd = new CreateEntityCmd(project.get(), scene->id, entityName, EntityCreationType::GAMEOBJECT, parent);
            EXPECT_TRUE(history()->addCommandNoMerge(cmd));
            return cmd->getEntity();
        }

        Entity duplicate(Entity source){
            auto* cmd = new DuplicateEntityCmd(project.get(), scene->id, {source});
            EXPECT_TRUE(history()->addCommandNoMerge(cmd));
            // the copy: the created entity beside its source
            for (Entity entity : cmd->getCreatedEntities()){
                if (parentOf(entity) == parentOf(source)) return entity;
            }
            return NULL_ENTITY;
        }

        Entity importEnemy(Entity parent = NULL_ENTITY){
            auto* cmd = new ImportPrefabCmd(project.get(), scene->id, fs::path("prefabs/Enemy.prefab"), parent);
            EXPECT_TRUE(history()->addCommandNoMerge(cmd));
            return cmd->getRootEntity();
        }
    };

}

TEST_F(EntityNamesTest, CreateNumbersAmongSiblingsOnly){
    const Entity a = create("A");
    const Entity b = create("B");
    const Entity blueA = create("Blue", a);
    const Entity blueB = create("Blue", b);
    EXPECT_EQ(name(blueA), "Blue");
    EXPECT_EQ(name(blueB), "Blue") << "a Blue under another parent is not a sibling";
    EXPECT_EQ(parentOf(blueB), b);

    const Entity secondA = create("Blue", a);
    EXPECT_EQ(name(secondA), "Blue 2") << "a sibling has the name";
    EXPECT_EQ(name(create("Blue")), "Blue") << "no root is named Blue";
    EXPECT_EQ(name(create("Blue")), "Blue 2") << "now one is";

    // undo and redo give the same names
    history()->undo();
    history()->undo();
    history()->redo();
    history()->redo();
    int rootBlues = 0;
    for (Entity entity : scene->entities){
        if (parentOf(entity) == NULL_ENTITY && (name(entity) == "Blue" || name(entity) == "Blue 2")) rootBlues++;
    }
    EXPECT_EQ(rootBlues, 2);
}

TEST_F(EntityNamesTest, DuplicateNumbersAmongSiblingsOnly){
    const Entity a = create("A");
    const Entity b = create("B");
    create("Blue", a);
    create("Blue 2", a);
    const Entity blueB = create("Blue", b);

    const Entity copy = duplicate(blueB);
    ASSERT_NE(copy, NULL_ENTITY);
    EXPECT_EQ(parentOf(copy), b);
    EXPECT_EQ(name(copy), "Blue 2") << "A's Blue 2 is not B's child";
    const Entity again = duplicate(blueB);
    EXPECT_EQ(name(again), "Blue 3");
}

TEST_F(EntityNamesTest, PrefabImportNumbersAmongSiblingsOnly){
    // the fixture scene has an Enemy instance at the root
    const Entity holder = create("Holder");
    const Entity atRoot = importEnemy();
    ASSERT_NE(atRoot, NULL_ENTITY);
    EXPECT_EQ(name(atRoot), "Enemy 2") << "the root has an Enemy";
    const Entity below = importEnemy(holder);
    ASSERT_NE(below, NULL_ENTITY);
    EXPECT_EQ(parentOf(below), holder);
    EXPECT_EQ(name(below), "Enemy") << "Holder has no Enemy child";
    EXPECT_EQ(name(importEnemy(holder)), "Enemy 2");
}

}  // namespace EntityNamesTest_cpp
