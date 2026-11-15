// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Editor steps in one main-thread task after a delete (an MCP batch, a script): the delete
// leaves holes in the component arrays (ComponentArray), and a component found by entity must
// stay the entity's through the index reads that follow (they once closed the holes, moving it
// under the reference: an undo restored a GameObject as a root, a move recorded the wrong old
// parent). The Control_ cases close the holes first, as the next frame does.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "JasperEngine.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "util/ProjectUtils.h"

#include "support/PosixCompat.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct HolesHeadless {
        std::unique_ptr<Project> project;
    };

    HolesHeadless& holesHeadless() {
        static HolesHeadless h = []() {
            HolesHeadless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-destroy-holes-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            const fs::path dir = root / "project";
            fs::create_directories(dir / ".jasperengine");
            std::ofstream(dir / "project.yaml") << "name: holes\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            // the engine runs already (TestMain, its TestSystem): this project's folders are its paths
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            if (!out.project->loadProject(dir, false)) throw std::runtime_error("the test project did not load");
            return out;
        }();
        // every test of the project (the paths end with each test)
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    Project* project() {
        return holesHeadless().project.get();
    }

    uint32_t newScene() {
        Project* p = project();
        if (SceneProject* current = p->getSelectedScene()) current->isModified = false;
        p->createNewScene("Holes", SceneType::SCENE_3D);
        return p->getSelectedSceneId();
    }

    Entity create(uint32_t sceneId, const std::string& name, Entity parent = NULL_ENTITY) {
        CreateEntityCmd* cmd = parent == NULL_ENTITY
            ? new CreateEntityCmd(project(), sceneId, name, EntityCreationType::GAMEOBJECT)
            : new CreateEntityCmd(project(), sceneId, name, EntityCreationType::GAMEOBJECT, parent);
        cmd->setQuiet(true);
        CommandHandle::get(sceneId)->addCommandNoMerge(cmd);
        return cmd->getEntity();
    }

    // How A goes first: destroyed (its holes left open, as a GameObject.destroy or a script
    // leaves them until the next frame), deleted by a command (an MCP batch of two
    // delete_entity), or destroyed and compacted (the next frame)
    enum class FirstGoes { Destroyed, DeleteCommand, DestroyedAndCompacted };

    void removeFirst(uint32_t sceneId, Scene* scene, Entity a, FirstGoes how) {
        if (how == FirstGoes::DeleteCommand) {
            CommandHandle::get(sceneId)->addCommandNoMerge(new DeleteEntityCmd(project(), sceneId, a));
            return;
        }
        scene->destroyEntity(a);
        ProjectUtils::reconcileTrackedEntities(scene, project()->getScene(sceneId)->entities, project(), sceneId);
        if (how == FirstGoes::DestroyedAndCompacted) scene->compactComponents();
    }

    // A goes first (before the others in the arrays), then C (below P) is deleted and the delete undone
    void deleteAfterDelete(FirstGoes how) {
        const uint32_t sceneId = newScene();
        Scene* scene = project()->getScene(sceneId)->scene;
        const Entity a = create(sceneId, "A");
        const Entity p = create(sceneId, "P");
        const Entity c = create(sceneId, "C", p);
        create(sceneId, "D");
        removeFirst(sceneId, scene, a, how);
        CommandHandle::get(sceneId)->addCommandNoMerge(new DeleteEntityCmd(project(), sceneId, c));
        EXPECT_FALSE(scene->isEntityCreated(c));
        CommandHandle::get(sceneId)->undo();
        ASSERT_TRUE(scene->isEntityCreated(c));
        EXPECT_EQ(scene->getComponent<Transform>(c).parent, p) << "C restored under the wrong parent";
        auto transforms = scene->getComponentArray<Transform>();
        EXPECT_EQ(transforms->getIndex(c), transforms->getIndex(p) + 1) << "C restored out of P's branch";
    }

    // A goes first, then C is moved from below P to after U (below T)
    void moveAfterDelete(FirstGoes how) {
        const uint32_t sceneId = newScene();
        Scene* scene = project()->getScene(sceneId)->scene;
        const Entity a = create(sceneId, "A");
        const Entity p = create(sceneId, "P");
        const Entity c = create(sceneId, "C", p);
        const Entity t = create(sceneId, "T");
        const Entity u = create(sceneId, "U", t);
        removeFirst(sceneId, scene, a, how);
        Entity oldParent = NULL_ENTITY;
        size_t oldIndex = 0;
        bool hasTransform = false;
        std::vector<Entity> list = project()->getScene(sceneId)->entities;
        // C's index counted by a walk (an index read would have closed the holes before the move)
        size_t indexBefore = 0;
        scene->getComponentArray<Transform>()->forEach([&indexBefore, c](Entity entity, Transform&) {
            if (entity == c) return false;
            indexBefore++;
            return true;
        });
        ASSERT_TRUE(ProjectUtils::moveEntityOrderByTarget(scene, list, c, u, InsertionType::AFTER, oldParent, oldIndex, hasTransform));
        EXPECT_TRUE(hasTransform);
        EXPECT_EQ(oldParent, p) << "old parent recorded wrong";
        EXPECT_EQ(oldIndex, indexBefore);
        EXPECT_EQ(scene->getComponent<Transform>(c).parent, t) << "moved under the wrong parent";
    }

}

TEST(EntityDestroyHoles, DeleteAfterDeleteSameFrameKeepsParentForUndo) {
    deleteAfterDelete(FirstGoes::DeleteCommand);
}

TEST(EntityDestroyHoles, DeleteAfterDestroySameFrameKeepsParentForUndo) {
    deleteAfterDelete(FirstGoes::Destroyed);
}

TEST(EntityDestroyHoles, MoveAfterDeleteSameFrameRecordsOldParent) {
    moveAfterDelete(FirstGoes::DeleteCommand);
}

TEST(EntityDestroyHoles, MoveAfterDestroySameFrameRecordsOldParent) {
    moveAfterDelete(FirstGoes::Destroyed);
}

TEST(EntityDestroyHoles, Control_DeleteAfterDeleteCompacted) {
    deleteAfterDelete(FirstGoes::DestroyedAndCompacted);
}

TEST(EntityDestroyHoles, Control_MoveAfterDeleteCompacted) {
    moveAfterDelete(FirstGoes::DestroyedAndCompacted);
}

// The delete commands leave no holes behind them (the next step reads packed arrays)
TEST(EntityDestroyHoles, DeleteCommandLeavesNoHoles) {
    const uint32_t sceneId = newScene();
    Scene* scene = project()->getScene(sceneId)->scene;
    const Entity a = create(sceneId, "A");
    const Entity p = create(sceneId, "P");
    create(sceneId, "C", p);
    create(sceneId, "D");
    CommandHandle::get(sceneId)->addCommandNoMerge(new DeleteEntityCmd(project(), sceneId, std::vector<Entity>{a, p}));
    EXPECT_FALSE(scene->getComponentArray<Transform>()->hasHoles());
    CommandHandle::get(sceneId)->undo();
    EXPECT_FALSE(scene->getComponentArray<Transform>()->hasHoles());
    CommandHandle::get(sceneId)->redo();
    EXPECT_FALSE(scene->getComponentArray<Transform>()->hasHoles());
}
