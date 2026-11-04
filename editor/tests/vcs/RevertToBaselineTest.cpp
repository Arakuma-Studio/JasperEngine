// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Revert to Baseline (docs/specs/S29-version-control.md R16, AC12) on tools/fixtures/
// prefab-runtime loaded headless: the saved file is the baseline, the open scene's encode the
// current side. Each revert is one undo step, and its undo gives the encoding it had before.

#include "VcsTestSupport.h"
#include "spritenormals/SpriteNormalsField.h"
#include "component/LightComponent.h"
#include "component/Transform.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/MoveEntityOrderCmd.h"
#include "command/type/PrefabOverrideCmd.h"
#include "command/type/ReflectedValueCmd.h"
#include "command/type/RevertToBaselineCmd.h"
#include "reflect/Reflect.h"
#include "vcs/scene/SceneDiff.h"

#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;
namespace scene = jasperengine::editor::vcs::scene;

namespace {

    std::string overridesText(const Prefab::ObjectOverrides& overrides) {
        std::ostringstream out;
        for (const auto& [type, names] : overrides.properties) {
            out << static_cast<int>(type) << ":";
            for (const std::string& name : names) out << name << ",";
            out << ";";
        }
        out << "state " << static_cast<int>(overrides.state) << " added " << overrides.addedComponents.size() << " removed "
            << overrides.removedComponents.size();
        return out.str();
    }

    class RevertToBaselineTest : public ::testing::Test {
    protected:
        jasperengine::test::TempDir dir;
        std::unique_ptr<Project> project;
        uint32_t sceneId = 0;
        scene::SceneYamlModel baseline;

        void SetUp() override {
            fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            project = std::make_unique<Project>();
            project->setSkipProjectLock(true);
            ASSERT_TRUE(project->loadProject(dir.path(), false));
            for (SceneProject& candidate : project->getScenes()) {
                if (candidate.filepath.filename() == "Main Scene.scene") sceneId = candidate.id;
            }
            ASSERT_NE(sceneId, 0u);
            if (!project->getScene(sceneId)->scene) project->loadScene(project->getScene(sceneId)->filepath, true, false, true);
            ASSERT_NE(project->getScene(sceneId)->scene, nullptr);
            // the baseline: the scene as this editor saves it now (what HEAD would hold)
            std::string error;
            ASSERT_TRUE(scene::SceneYamlModel::fromDocument(project->encodeDocument(sceneId), baseline, &error)) << error;
        }

        void TearDown() override {
            if (project) {
                for (SceneProject& candidate : project->getScenes()) CommandHandle::remove(candidate.id);
                project->releaseScenes();
            }
            project.reset();
        }

        Scene* sceneOf() { return project->getScene(sceneId)->scene; }
        std::string encoding() { return YAML::Dump(project->encodeDocument(sceneId)); }

        scene::SceneDiffResult diff() {
            scene::SceneYamlModel current;
            std::string error;
            EXPECT_TRUE(scene::SceneYamlModel::fromDocument(project->encodeDocument(sceneId), current, &error)) << error;
            return scene::diffScenes(baseline, current);
        }

        Entity named(const std::string& name) {
            for (Entity entity : project->getScene(sceneId)->entities) {
                if (sceneOf()->getEntityName(entity) == name) return entity;
            }
            return NULL_ENTITY;
        }

        bool run(Command* command) { return CommandHandle::get(sceneId)->addCommandNoMerge(command); }

        // reverts one diff row; checks it is one step whose undo restores the encoding
        void revertAndCheckUndo(uint64_t object, const char* component, const char* property) {
            const scene::SceneDiffResult result = diff();
            const scene::ObjectDiff* objectDiff = result.find(object);
            ASSERT_NE(objectDiff, nullptr) << "no diff of " << object;
            const scene::ComponentDiff* componentDiff = component ? objectDiff->component(component) : nullptr;
            if (component) {
                ASSERT_NE(componentDiff, nullptr) << component;
            }
            const scene::PropertyDiff* propertyDiff = nullptr;
            if (property) {
                for (const scene::PropertyDiff& candidate : componentDiff->properties) {
                    if (candidate.path == property) propertyDiff = &candidate;
                }
                ASSERT_NE(propertyDiff, nullptr) << property;
            }
            std::string why;
            RevertToBaselineCmd* command = RevertToBaselineCmd::build(project.get(), sceneId, baseline, *objectDiff, componentDiff, propertyDiff, &why);
            ASSERT_NE(command, nullptr) << why;
            const std::string before = encoding();
            const size_t steps = CommandHandle::get(sceneId)->appliedCount();
            ASSERT_TRUE(run(command));
            EXPECT_EQ(CommandHandle::get(sceneId)->appliedCount(), steps + 1);
            EXPECT_NE(CommandHandle::get(sceneId)->top()->name().find("to Baseline"), std::string::npos);
            EXPECT_EQ(diff().find(object), nullptr) << "still differs after the revert";
            ASSERT_TRUE(CommandHandle::get(sceneId)->undo());
            EXPECT_EQ(encoding(), before);
            ASSERT_TRUE(CommandHandle::get(sceneId)->redo());
            EXPECT_EQ(diff().find(object), nullptr);
        }
    };

    reflect::Value vector3(float x, float y, float z) {
        const reflect::ComponentDesc* desc = reflect::find(ComponentType::Transform);
        const reflect::FieldDesc* field = reflect::findField(*desc, "position");
        const Vector3 value(x, y, z);
        return reflect::Value(field->ops, &value);
    }

}

TEST_F(RevertToBaselineTest, Property) {
    const Entity spawner = named("Spawner");
    ASSERT_NE(spawner, NULL_ENTITY);
    ASSERT_TRUE(run(new ReflectedValueCmd(project.get(), sceneId, {spawner}, ComponentType::Transform, "position", vector3(5, 5, 5))));
    revertAndCheckUndo(spawner, "transform", "position");
    EXPECT_EQ(sceneOf()->getComponent<Transform>(spawner).position, Vector3(10, 0, 0));
}

TEST_F(RevertToBaselineTest, Component) {
    const Entity spawner = named("Spawner");
    ASSERT_TRUE(run(new AddComponentCmd(project.get(), sceneId, spawner, ComponentType::LightComponent)));
    revertAndCheckUndo(spawner, "light", nullptr);
    EXPECT_EQ(sceneOf()->findComponent<LightComponent>(spawner), nullptr);
}

TEST_F(RevertToBaselineTest, RemovedGameObjectComesBackWithItsId) {
    const Entity spawner = named("Spawner");
    ASSERT_TRUE(run(new DeleteEntityCmd(project.get(), sceneId, spawner)));
    ASSERT_FALSE(sceneOf()->isEntityCreated(spawner));
    revertAndCheckUndo(spawner, nullptr, nullptr);
    ASSERT_TRUE(sceneOf()->isEntityCreated(spawner));
    EXPECT_EQ(sceneOf()->getEntityName(spawner), "Spawner");
}

TEST_F(RevertToBaselineTest, AddedGameObjectIsDeleted) {
    auto* create = new CreateEntityCmd(project.get(), sceneId, "Extra", EntityCreationType::GAMEOBJECT);
    ASSERT_TRUE(run(create));
    const Entity extra = named("Extra");
    ASSERT_NE(extra, NULL_ENTITY);
    revertAndCheckUndo(extra, nullptr, nullptr);
    EXPECT_FALSE(sceneOf()->isEntityCreated(extra));
}

TEST_F(RevertToBaselineTest, ReparentedGameObjectGoesBack) {
    const Entity spawner = named("Spawner");
    const Entity game = named("Game");
    ASSERT_TRUE(run(new MoveEntityOrderCmd(project.get(), sceneId, spawner, game, InsertionType::INTO)));
    ASSERT_EQ(sceneOf()->getComponent<Transform>(spawner).parent, game);
    const scene::SceneDiffResult moved = diff();
    ASSERT_NE(moved.find(spawner), nullptr);
    EXPECT_TRUE(moved.find(spawner)->reparented);
    revertAndCheckUndo(spawner, nullptr, nullptr);
    EXPECT_EQ(sceneOf()->getComponent<Transform>(spawner).parent, NULL_ENTITY);
}

TEST_F(RevertToBaselineTest, PrefabMemberPropertyMatchesAManualRevert) {
    // a member of the Enemy instance: its edit is an override; reverting it to the baseline (no
    // override there) leaves the overrides as the Inspector's Revert does
    Entity body = NULL_ENTITY;
    for (Entity entity : project->getScene(sceneId)->entities) {
        PrefabObjectRef ref;
        if (sceneOf()->getEntityName(entity) == "Body" && project->findPrefabObject(sceneId, entity, ref)) body = entity;
    }
    ASSERT_NE(body, NULL_ENTITY);
    const Prefab::ObjectOverrides untouched = project->getPrefabObjectOverrides(sceneId, body);
    ASSERT_TRUE(run(new ReflectedValueCmd(project.get(), sceneId, {body}, ComponentType::Transform, "position", vector3(4, 4, 4))));
    ASSERT_TRUE(project->getPrefabObjectOverrides(sceneId, body).hasProperty(ComponentType::Transform, "position"));
    const scene::SceneDiffResult result = diff();
    const scene::ObjectDiff* member = result.find(body);
    ASSERT_NE(member, nullptr);
    EXPECT_TRUE(member->prefabMember);
    revertAndCheckUndo(body, "transform", "position");
    const std::string viaBaseline = overridesText(project->getPrefabObjectOverrides(sceneId, body));
    const Vector3 positionViaBaseline = sceneOf()->getComponent<Transform>(body).position;
    // the same edit reverted by hand
    ASSERT_TRUE(CommandHandle::get(sceneId)->undo());
    ASSERT_TRUE(run(new PrefabOverrideCmd(project.get(), sceneId, body, PrefabOverrideCmd::Operation::RevertProperty, ComponentType::Transform,
                                          {"position"})));
    EXPECT_EQ(overridesText(project->getPrefabObjectOverrides(sceneId, body)), viaBaseline);
    EXPECT_EQ(sceneOf()->getComponent<Transform>(body).position, positionViaBaseline);
    EXPECT_EQ(viaBaseline, overridesText(untouched));
}
