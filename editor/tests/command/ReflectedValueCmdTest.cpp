// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ReflectedValueCmd (docs/specs/S08-reflection.md R11, AC9) on a headless Project
// (tools/fixtures/prefab-runtime, its Enemy prefab given a Layout Group): array insert, remove
// and move on two GameObjects with undo and redo, a prefab override recorded under the field's
// own name when the edit used an alias, the field's update flags, a drag as one undo entry, and
// the S24 name.

#include "UndoFakeCommand.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/PlayChangeReview.h"
#include "command/type/ReflectedValueCmd.h"
#include "animation/ActionPreviewSnapshot.h"
#include "reflect/TypeOps.h"
#include "component/Joint3DComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/LightComponent.h"
#include "util/ProjectUtils.h"

#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

// (a named namespace, tests included: a unity build merges the test sources)
namespace reflected_value_cmd_test {

    constexpr Entity GAME = 1001;
    constexpr Entity SPAWNER = 1002;
    constexpr Entity ENEMY = 1003;   // the root of an Enemy.prefab instance

    class ReflectedValueCmdTest : public ::testing::Test {
    protected:
        UndoStatics statics;
        jasperengine::test::TestTempDir dir{"reflected_value"};
        std::unique_ptr<Project> project;
        SceneProject* scene = nullptr;
        uint32_t sceneId = 0;

        void SetUp() override {
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            // the prefab's root holds a Layout Group, whose useAllWrapSpace older files call fillAvailableSpace
            const fs::path prefab = dir.path() / "prefabs" / "Enemy.prefab";
            std::stringstream text;
            text << std::ifstream(prefab).rdbuf();
            std::string content = text.str();
            const std::string anchor = "    components:\n";
            const size_t at = content.find(anchor);
            ASSERT_NE(at, std::string::npos);
            content.insert(at + anchor.size(), "      layoutgroup:\n        useAllWrapSpace: false\n");
            std::ofstream(prefab, std::ios::binary) << content;

            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            for (SceneProject& candidate : project->getScenes()) {
                if (!candidate.filepath.empty() && !candidate.scene) project->loadScene(candidate.filepath, false, false, true);
            }
            for (SceneProject& candidate : project->getScenes()) {
                if (candidate.scene && candidate.name == "Main Scene") scene = &candidate;
            }
            ASSERT_NE(scene, nullptr);
            sceneId = scene->id;
            project->setSelectedSceneId(sceneId);
            scene->isModified = false;
        }
        void TearDown() override {
            PlayChangeReview::get().discardAll();
            if (project) {
                for (SceneProject& candidate : project->getScenes()) CommandHandle::remove(candidate.id);
                // a Project frees none of its scenes itself (LeakSanitizer: each test leaked its scenes and renders)
                project->releaseScenes();
            }
            project.reset();
        }

        CommandHistory* history() { return CommandHandle::get(sceneId); }

        void add(Entity entity, ComponentType type) {
            ProjectUtils::addEntityComponent(scene->scene, entity, type, scene->entities);
        }

        std::vector<Vector3>& path(Entity entity) { return scene->scene->getComponent<Joint3DComponent>(entity).pathPoints; }

        static reflect::Value vector3(const Vector3& value) { return reflect::Value(&reflect::ops::Vector3, &value); }
        static reflect::Value number(float value) { return reflect::Value(&reflect::ops::Float, &value); }
    };

    TEST_F(ReflectedValueCmdTest, ArrayInsertRemoveMoveOnTwoGameObjects) {
        add(GAME, ComponentType::Joint3DComponent);
        add(SPAWNER, ComponentType::Joint3DComponent);
        const std::vector<Entity> both = {GAME, SPAWNER};
        for (Entity entity : both) scene->scene->getComponent<Joint3DComponent>(entity).needUpdateJoint = false;

        ASSERT_TRUE(history()->addCommand(ReflectedValueCmd::insert(project.get(), sceneId, both, ComponentType::Joint3DComponent, "pathPoints",
                                                                    0, vector3(Vector3(1, 2, 3)))));
        ASSERT_TRUE(history()->addCommand(ReflectedValueCmd::insert(project.get(), sceneId, both, ComponentType::Joint3DComponent, "pathPoints",
                                                                    99, vector3(Vector3(4, 5, 6)))));
        for (Entity entity : both) {
            ASSERT_EQ(path(entity).size(), 2u);
            EXPECT_EQ(path(entity)[1], Vector3(4, 5, 6));
            // the field's update flags reached the component (UpdateFlags_Joint3D)
            EXPECT_TRUE(scene->scene->getComponent<Joint3DComponent>(entity).needUpdateJoint);
        }

        ASSERT_TRUE(history()->addCommand(ReflectedValueCmd::move(project.get(), sceneId, both, ComponentType::Joint3DComponent, "pathPoints", 1, 0)));
        for (Entity entity : both) EXPECT_EQ(path(entity)[0], Vector3(4, 5, 6));
        ASSERT_TRUE(history()->addCommand(ReflectedValueCmd::remove(project.get(), sceneId, both, ComponentType::Joint3DComponent, "pathPoints", 0)));
        for (Entity entity : both) {
            ASSERT_EQ(path(entity).size(), 1u);
            EXPECT_EQ(path(entity)[0], Vector3(1, 2, 3));
        }

        ASSERT_TRUE(history()->undo());   // the remove
        for (Entity entity : both) EXPECT_EQ(path(entity), (std::vector<Vector3>{Vector3(4, 5, 6), Vector3(1, 2, 3)}));
        ASSERT_TRUE(history()->undo());   // the move
        for (Entity entity : both) EXPECT_EQ(path(entity), (std::vector<Vector3>{Vector3(1, 2, 3), Vector3(4, 5, 6)}));
        ASSERT_TRUE(history()->undo());
        ASSERT_TRUE(history()->undo());
        for (Entity entity : both) EXPECT_TRUE(path(entity).empty());

        ASSERT_TRUE(history()->redo());
        ASSERT_TRUE(history()->redo());
        ASSERT_TRUE(history()->redo());
        ASSERT_TRUE(history()->redo());
        for (Entity entity : both) EXPECT_EQ(path(entity), std::vector<Vector3>{Vector3(1, 2, 3)});
    }

    TEST_F(ReflectedValueCmdTest, AliasRecordsTheFieldsOwnOverride) {
        ASSERT_NE(scene->scene->findComponent<LayoutGroupComponent>(ENEMY), nullptr);
        const bool value = true;
        auto* cmd = new ReflectedValueCmd(project.get(), sceneId, {ENEMY}, ComponentType::LayoutGroupComponent, "fillAvailableSpace",
                                          reflect::Value(&reflect::ops::Bool, &value));
        EXPECT_EQ(cmd->getPath(), "useAllWrapSpace");
        ASSERT_TRUE(history()->addCommand(cmd));
        EXPECT_TRUE(scene->scene->getComponent<LayoutGroupComponent>(ENEMY).useAllWrapSpace);

        Prefab::ObjectOverrides overrides;
        ASSERT_TRUE(project->getPrefabEditOverrides(sceneId, ENEMY, overrides));
        const auto it = overrides.properties.find(ComponentType::LayoutGroupComponent);
        ASSERT_NE(it, overrides.properties.end());
        EXPECT_TRUE(it->second.count("useAllWrapSpace"));
        EXPECT_FALSE(it->second.count("fillAvailableSpace"));

        ASSERT_TRUE(history()->undo());
        EXPECT_FALSE(scene->scene->getComponent<LayoutGroupComponent>(ENEMY).useAllWrapSpace);
        ASSERT_TRUE(project->getPrefabEditOverrides(sceneId, ENEMY, overrides));
        const auto after = overrides.properties.find(ComponentType::LayoutGroupComponent);
        EXPECT_TRUE(after == overrides.properties.end() || !after->second.count("useAllWrapSpace"));
    }

    TEST_F(ReflectedValueCmdTest, DragMergesIntoOneEntry) {
        add(GAME, ComponentType::LightComponent);
        const float start = scene->scene->getComponent<LightComponent>(GAME).intensity;
        const size_t before = history()->appliedCount();
        for (float value : {2.0f, 3.0f, 4.0f}) {
            ASSERT_TRUE(history()->addCommand(new ReflectedValueCmd(project.get(), sceneId, {GAME}, ComponentType::LightComponent, "intensity",
                                                                    number(value))));
        }
        EXPECT_EQ(history()->appliedCount(), before + 1);
        EXPECT_FLOAT_EQ(scene->scene->getComponent<LightComponent>(GAME).intensity, 4.0f);
        ASSERT_TRUE(history()->undo());
        EXPECT_FLOAT_EQ(scene->scene->getComponent<LightComponent>(GAME).intensity, start);
    }

    TEST_F(ReflectedValueCmdTest, NamesTheEdit) {
        add(GAME, ComponentType::LightComponent);
        add(SPAWNER, ComponentType::LightComponent);
        ReflectedValueCmd two(project.get(), sceneId, {GAME, SPAWNER}, ComponentType::LightComponent, "intensity", number(2.0f));
        EXPECT_EQ(two.describe(), "Set Light Intensity (2 objects)");
        ReflectedValueCmd one(project.get(), sceneId, {GAME}, ComponentType::LightComponent, "intensity", number(2.0f));
        EXPECT_EQ(one.describe(), "Set Light Intensity");
    }

    // runtime-core 6.6: an array row's insert during an action preview is an edit the preview keeps:
    // ReflectedValueCmd tells the preview's snapshot about its writes (ClipRecordHooks), so the seek's
    // restore does not take the element away, and its undo still removes it
    TEST_F(ReflectedValueCmdTest, ArrayInsertDuringAPreviewSurvivesItsSeek) {
        add(GAME, ComponentType::Joint3DComponent);
        ActionPreviewSnapshot preview;
        preview.add(sceneId, scene->scene, GAME);

        ASSERT_TRUE(history()->addCommand(ReflectedValueCmd::insert(project.get(), sceneId, {GAME}, ComponentType::Joint3DComponent, "pathPoints",
                                                                    0, vector3(Vector3(1, 2, 3)))));
        EXPECT_TRUE(preview.takeChanged()) << "the preview heard the write";
        preview.restore(scene->scene);   // what a seek of the preview does
        ASSERT_EQ(path(GAME).size(), 1u) << "the element added during the preview survives its seek";
        EXPECT_EQ(path(GAME)[0], Vector3(1, 2, 3));

        preview.restore(scene->scene);
        preview.clear();                 // the preview stops
        ASSERT_TRUE(history()->undo());
        EXPECT_TRUE(path(GAME).empty()) << "its undo goes back to the authored value";
    }

}
