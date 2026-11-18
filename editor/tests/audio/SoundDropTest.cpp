// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// A sound dropped in the scene (editor/audio/SoundDrop.cpp) makes a GameObject with an Audio
// Source, undone with its steps (docs/specs/S97-audio-gaps.md REQ-21): a 2D drop (a 2D scene, the
// Hierarchy) is Spatial Blend 0 on a Transform, a 3D drop Spatial Blend 1 where it lands.

#include "Project.h"
#include "audio/SoundDrop.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "component/AudioSourceComponent.h"
#include "component/Transform.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

namespace sound_drop_test {

    class SoundDropTest : public ::testing::Test {
    protected:
        jasperengine::test::TestTempDir dir{"sound-drop"};
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

        Entity named(const std::string& name){
            for (Entity entity : scene->entities){
                if (scene->scene->getEntityName(entity) == name) return entity;
            }
            return NULL_ENTITY;
        }
    };

}

using namespace sound_drop_test;

TEST_F(SoundDropTest, ATwoDDropIsAGameObjectAtBlendZero){
    std::string error;
    CommandHistory* history = CommandHandle::get(scene->id);
    const size_t before = history->position();
    ASSERT_TRUE(SoundDrop::createSource(project.get(), scene->id, "sounds/flat.wav", false, Vector3(0, 0, 0), error)) << error;
    const Entity entity = named("flat");
    ASSERT_NE(entity, NULL_ENTITY);
    EXPECT_NE(scene->scene->findComponent<Transform>(entity), nullptr) << "a GameObject";
    const AudioSourceComponent& audio = scene->scene->getComponent<AudioSourceComponent>(entity);
    EXPECT_FLOAT_EQ(audio.spatialBlend, 0.0f);
    EXPECT_EQ(audio.filename.path, "sounds/flat.wav");

    while (history->position() > before && history->undo()) {}
    EXPECT_EQ(history->position(), before);
    EXPECT_EQ(named("flat"), NULL_ENTITY) << "undone";
}

TEST_F(SoundDropTest, AThreeDDropIsAtBlendOneWhereItLands){
    std::string error;
    ASSERT_TRUE(SoundDrop::createSource(project.get(), scene->id, "sounds/spatial.wav", true, Vector3(1, 2, 3), error)) << error;
    const Entity entity = named("spatial");
    ASSERT_NE(entity, NULL_ENTITY);
    const Transform* transform = scene->scene->findComponent<Transform>(entity);
    ASSERT_NE(transform, nullptr);
    EXPECT_EQ(transform->position, Vector3(1, 2, 3));
    EXPECT_FLOAT_EQ(scene->scene->getComponent<AudioSourceComponent>(entity).spatialBlend, 1.0f);
}
