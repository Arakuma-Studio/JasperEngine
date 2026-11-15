// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A whole-scene camera-texture bind (CameraTextureLink::resolve) skips the editor's own entities
// (the Scene view's grid, gizmos and icons: what made a prefab apply cost the square of its
// instances), never a GameObject of the scene: also one an older file saved with an id in the
// system range (particles-export's Camera is 1000). Here a GameObject decoded at id 1000 holds a
// texture linked to a camera inside a prefab instance: it is bound by the bind of the whole
// scene, and again after the instance is imported again (an apply makes the camera anew).

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "JasperEngine.h"
#include "Project.h"
#include "Stream.h"
#include "command/CommandHandle.h"
#include "command/type/CreatePrefabCmd.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "util/CameraTextureLink.h"

#include "support/PosixCompat.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-camera-link-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            std::ofstream(out.dir / "project.yaml") << "name: cameralink\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            // the engine runs already (TestMain, its TestSystem): this project's folders are its paths
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    Project* project() { return headless().project.get(); }

    const Texture& screenTexture(Scene* scene, Entity screen) {
        return scene->getComponent<MeshComponent>(screen).submeshes[0].material.baseColorTexture;
    }

    Framebuffer* lensFramebuffer(Scene* scene, Entity lens) {
        return scene->getComponent<CameraComponent>(lens).framebuffer;
    }

}

TEST(CameraTextureLink, AGameObjectWithASystemRangeIdIsBoundByAWholeSceneBindAndAfterAnApply) {
    ASSERT_TRUE(headless().ready);
    fs::create_directories(headless().dir / "camera_link");
    project()->createNewScene("Camera Link", SceneType::SCENE_3D);
    SceneProject* sceneProject = project()->getScene(project()->getSelectedSceneId());
    const uint32_t sceneId = sceneProject->id;
    Scene* scene = sceneProject->scene;

    // a prefab instance whose child renders to a texture
    const Entity eye = scene->createEntity();
    scene->setEntityName(eye, "Eye");
    scene->addComponent<Transform>(eye);
    sceneProject->entities.push_back(eye);
    const Entity lens = scene->createEntity();
    scene->setEntityName(lens, "Lens");
    Transform lensTransform;
    lensTransform.parent = eye;
    scene->addComponent<Transform>(lens, lensTransform);
    // (no render target made: the link binds the camera's framebuffer object, drawn or not)
    scene->addComponent<CameraComponent>(lens, CameraComponent());
    sceneProject->entities.push_back(lens);
    CommandHandle::get(sceneId)->addCommandNoMerge(new CreatePrefabCmd(project(), sceneId, fs::path("camera_link") / "Eye.prefab",
        Stream::encodeEntity(eye, scene, project(), sceneProject)));
    ASSERT_TRUE(project()->isPrefabInstanceRoot(sceneId, eye));

    // a GameObject an older file saved at id 1000, its material showing the lens
    const Entity made = scene->createEntity();
    scene->setEntityName(made, "Screen");
    scene->addComponent<Transform>(made);
    MeshComponent mesh;
    mesh.numSubmeshes = 1;
    mesh.submeshes[0].material.baseColorTexture.setId(CameraTextureLink::makeId(lens));
    scene->addComponent<MeshComponent>(made, mesh);
    YAML::Node node = Stream::encodeEntity(made, scene);
    scene->destroyEntity(made);
    const Entity screen = EntityManager::lastSystemEntity();
    ASSERT_FALSE(scene->isEntityCreated(screen));
    node["entity"] = screen;
    const std::vector<Entity> decoded = Stream::decodeEntity(node, scene, &sceneProject->entities, project(), sceneProject);
    ASSERT_FALSE(decoded.empty());
    ASSERT_EQ(decoded.front(), screen);
    ASSERT_TRUE(scene->findComponent<MeshComponent>(screen));
    ASSERT_EQ(CameraTextureLink::parse(screenTexture(scene, screen).getId()), lens);

    CameraTextureLink::resolve(scene);
    EXPECT_EQ(screenTexture(scene, screen).getFramebuffer(), lensFramebuffer(scene, lens)) << "the whole-scene bind binds it";

    // imported again (what an apply does to every instance): the lens is made anew, and the bind
    // that follows binds the screen to it whatever it held before
    Texture& texture = scene->getComponent<MeshComponent>(screen).submeshes[0].material.baseColorTexture;
    const std::string id = texture.getId();
    texture.setFramebuffer(nullptr); // clears the id
    texture.setId(id);
    ASSERT_TRUE(project()->reimportPrefabInstance(sceneProject, eye, YAML::Node()));
    ASSERT_TRUE(scene->isEntityCreated(lens) && scene->findComponent<CameraComponent>(lens));
    EXPECT_EQ(screenTexture(scene, screen).getFramebuffer(), lensFramebuffer(scene, lens)) << "the apply's bind binds it";
    sceneProject->isModified = false;
}
