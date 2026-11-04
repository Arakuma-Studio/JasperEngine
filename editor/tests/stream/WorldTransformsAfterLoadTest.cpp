// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Scene and prefab files hold no world values (scene format 4, docs/specs/S29-version-control.md
// R20), so what a decode gives must have them computed before any frame runs: Lua and MCP read
// them in Edit mode, picking, bounds and bakes use them (editor/util/WorldTransforms.h). Each
// case reads them right after the decode, with no transform pass in between.

#include "support/EditorTestSupport.h"
#include "component/Transform.h"

#include "Project.h"
#include "TempDir.h"
#include "command/CommandHandle.h"
#include "command/type/ImportPrefabCmd.h"
#include "util/Prefab.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace world_transforms_test {

    void expectNear(const Vector3& actual, const Vector3& expected, const std::string& what) {
        EXPECT_NEAR(actual.x, expected.x, 1e-5f) << what;
        EXPECT_NEAR(actual.y, expected.y, 1e-5f) << what;
        EXPECT_NEAR(actual.z, expected.z, 1e-5f) << what;
    }

    Entity named(EntityRegistry& registry, const std::vector<Entity>& entities, const std::string& name) {
        for (Entity entity : entities) {
            if (registry.getEntityName(entity) == name) return entity;
        }
        return NULL_ENTITY;
    }

    // a root at (2, 0, 0) scaled 2, its child one unit up: the child is at (2, 2, 0)
    const char* const kParentAndChild =
        "  - type: Entity\n    entity: 1001\n    name: Base\n    components:\n      transform:\n"
        "        position: [2, 0, 0]\n        rotation: [1, 0, 0, 0]\n        scale: [2, 2, 2]\n"
        "    children:\n      - type: Entity\n        entity: 1002\n        name: Arm\n        components:\n"
        "          transform:\n            position: [0, 1, 0]\n            rotation: [1, 0, 0, 0]\n            scale: [1, 1, 1]\n";

}

using namespace world_transforms_test;

TEST(WorldTransforms, LoadedSceneHasThemBeforeAFrame) {
    const DecodedScene decoded = decodeSceneText(std::string("formatVersion: 4\nid: 7\nname: Loaded\nscene:\n"
                                                             "  backgroundColor: [0, 0, 0, 1]\nsceneType: scene_3d\nentities:\n") +
                                                     kParentAndChild,
                                                 "loaded.scene");
    Scene* scene = decoded.scene->scene;
    ASSERT_NE(scene, nullptr);
    const Entity arm = named(*scene, decoded.scene->entities, "Arm");
    ASSERT_NE(arm, NULL_ENTITY);
    const Transform& transform = scene->getComponent<Transform>(arm);
    expectNear(transform.worldPosition, Vector3(2, 2, 0), "the child's world position");
    expectNear(transform.worldScale, Vector3(2, 2, 2), "the child's world scale");
    expectNear(Vector3(transform.modelMatrix[3][0], transform.modelMatrix[3][1], transform.modelMatrix[3][2]), Vector3(2, 2, 0),
               "the child's model matrix");
    // still due for the render pass (bounds, cameras, lights)
    EXPECT_TRUE(transform.needUpdate);
}

TEST(WorldTransforms, PrefabRegistryHasThem) {
    ensureEngine();
    Prefab prefab;
    prefab.loadRegistry(YAML::Load(std::string("type: Prefab\nversion: 6\nguid: abcd\nnextEntity: 1003\nroot: 1001\nmembers:\n") +
                                   kParentAndChild));
    ASSERT_TRUE(prefab.registry);
    const Entity arm = named(*prefab.registry, prefab.registryEntities, "Arm");
    ASSERT_NE(arm, NULL_ENTITY);
    expectNear(prefab.registry->getComponent<Transform>(arm).worldPosition, Vector3(2, 2, 0), "a registry member's world position");
}

TEST(WorldTransforms, ProjectSceneAndInstantiatedPrefabHaveThem) {
    ensureEngine();
    jasperengine::test::TempDir dir;
    fs::copy(fs::path(JASPER_TEST_REPO_DIR) / "tools/fixtures/prefab-runtime", dir.path(),
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    auto project = std::make_unique<Project>();
    project->setSkipProjectLock(true);
    ASSERT_TRUE(project->loadProject(dir.path(), false));
    uint32_t sceneId = 0;
    for (SceneProject& candidate : project->getScenes()) {
        if (candidate.filepath.filename() == "Main Scene.scene") sceneId = candidate.id;
    }
    ASSERT_NE(sceneId, 0u);
    if (!project->getScene(sceneId)->scene) project->loadScene(project->getScene(sceneId)->filepath, true, false, true);
    SceneProject* sceneProject = project->getScene(sceneId);
    ASSERT_NE(sceneProject->scene, nullptr);
    Scene* scene = sceneProject->scene;

    // a Prefab instance member of the loaded scene: Enemy at (1, 2, 3), its Body one unit up
    // (editor/tests/fixtures/vcs/world/prefab-runtime.yaml)
    Entity body = NULL_ENTITY;
    for (Entity entity : sceneProject->entities) {
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (scene->getEntityName(entity) == "Body" && transform && transform->parent != NULL_ENTITY && scene->getEntityName(transform->parent) == "Enemy") body = entity;
    }
    ASSERT_NE(body, NULL_ENTITY);
    expectNear(scene->getComponent<Transform>(body).worldPosition, Vector3(1, 3, 3), "a loaded instance member");

    // instantiated now, its root placed at (5, 0, 0): its Body is at (5, 1, 0) at once
    auto* import = new ImportPrefabCmd(project.get(), sceneId, fs::path("prefabs/Enemy.prefab"));
    import->setPlacement(Vector3(5, 0, 0), Quaternion(), Vector3(1, 1, 1));
    ASSERT_TRUE(CommandHandle::get(sceneId)->addCommandNoMerge(import));
    const Entity root = import->getRootEntity();
    ASSERT_NE(root, NULL_ENTITY);
    expectNear(scene->getComponent<Transform>(root).worldPosition, Vector3(5, 0, 0), "the new instance's root");
    Entity newBody = NULL_ENTITY;
    for (Entity entity : import->getImportedEntities()) {
        if (scene->getEntityName(entity) == "Body") newBody = entity;
    }
    ASSERT_NE(newBody, NULL_ENTITY);
    expectNear(scene->getComponent<Transform>(newBody).worldPosition, Vector3(5, 1, 0), "the new instance's member");

    for (SceneProject& open : project->getScenes()) CommandHandle::remove(open.id);
    project->releaseScenes();
}
