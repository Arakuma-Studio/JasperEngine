// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Scene view's gizmos share their renderers per kind (docs/specs/S98-editor-tooling-gaps.md
// REQ-30, AC24 (a)): a gizmo object per GameObject took a system entity each (light 2, camera 2,
// sound 1, body 1), and the system range is 1..1000, so a large scene logged "No free system
// entity slot" and lost its gizmos. A kind takes a fixed number of system entities now, whatever
// the number of objects.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"
#include "support/PosixCompat.h"
#include "LogCapture.h"

#include "AppSettings.h"
#include "Engine.h"
#include "Project.h"
#include "component/LightComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    Project* gizmoProject() {
        static std::unique_ptr<Project> project = []() {
            const fs::path root = fs::temp_directory_path() / ("jasper-gizmo-entities-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            const fs::path dir = root / "project";
            fs::create_directories(dir / ".jasperengine");
            std::ofstream(dir / "project.yaml") << "name: gizmos\n";
            AppSettings::initialize();
            std::unique_ptr<Project> out(new Project());
            editor::test::useProjectPaths(out.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            if (!out->loadProject(dir, false)) throw std::runtime_error("the gizmo test project did not load");
            return out;
        }();
        editor::test::useProjectPaths(project.get());
        return project.get();
    }

    // a GameObject of the scene with a Transform at `position` (the Scene view's entities)
    Entity addObject(SceneProject* sceneProject, const Vector3& position) {
        Scene* scene = sceneProject->scene;
        const Entity entity = scene->createEntity();
        Transform transform;
        transform.position = position;
        scene->addComponent<Transform>(entity, transform);
        sceneProject->entities.push_back(entity);
        return entity;
    }

}

TEST(SceneRenderGizmos, SystemEntitiesPerKind) {
    Project* project = gizmoProject();
    project->createNewScene("Gizmos", SceneType::SCENE_3D);
    const uint32_t sceneId = project->getSelectedSceneId();
    SceneProject* sceneProject = project->getScene(sceneId);
    ASSERT_TRUE(sceneProject && sceneProject->scene && sceneProject->sceneRender);
    Scene* scene = sceneProject->scene;
    // every body's wire is drawn, not only a selected one's
    sceneProject->displaySettings.showAllBodies = true;

    project->updateSceneRender(sceneId);
    const size_t before = scene->getSystemEntityCount();

    const ::jasperengine::test::LogCapture capture;
    Entity lastLight = NULL_ENTITY;
    for (int i = 0; i < 500; i++) {
        lastLight = addObject(sceneProject, Vector3((float)(i % 25) * 2.0f, 0.0f, (float)(i / 25) * 2.0f));
        LightComponent light;
        light.type = LightType::POINT;
        scene->addComponent<LightComponent>(lastLight, light);

        const Entity body = addObject(sceneProject, Vector3((float)(i % 25) * 2.0f, 5.0f, (float)(i / 25) * 2.0f));
        RigidbodyComponent rigidbody;
        rigidbody.colliders.push_back(Collider3D());
        scene->addComponent<RigidbodyComponent>(body, rigidbody);
    }
    // a selected light shows its shape too
    sceneProject->selectedEntities = {lastLight};
    project->updateSceneRender(sceneId);
    project->updateSceneRender(sceneId);

    const size_t gizmoEntities = scene->getSystemEntityCount() - before;
    // the icons, the light shapes and the body wires: one renderer each
    EXPECT_LE(gizmoEntities, 3u) << "1,000 GameObjects with gizmos took " << gizmoEntities << " system entities";
    EXPECT_EQ(capture.count(D_LOG_ERROR, "No free system entity slot"), 0u);

    sceneProject->selectedEntities.clear();
    sceneProject->isModified = false;
}
