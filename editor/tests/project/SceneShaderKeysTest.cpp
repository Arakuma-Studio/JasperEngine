// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The shader variant keys a scene save writes (SceneProject::shaderKeys, Project::
// collectSavedShaderKeys) are derived from what the scene holds, never from what has drawn: the
// same scene saves the same list before and after its meshes loaded (a loaded mesh records the
// variants it was loaded with, and a depth variant only while shadows or SSAO ran), so a file
// does not depend on the render history (test_reflection::test_golden_resave saw a depth variant
// come and go). What a scene's file listed stays, and Play adds what its run loaded at Stop.

#include <gtest/gtest.h>

#include "Project.h"
#include "Scene.h"
#include "component/LightComponent.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderBuildTypes.h"

#include "support/EditorTestSupport.h"

#include <memory>
#include <set>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    class SceneShaderKeys : public ::testing::Test {
    protected:
        static void SetUpTestSuite() {
            editor::test::ensureEngine();
        }
    };

    std::string describe(const std::set<ShaderKey>& keys) {
        std::string text;
        for (ShaderKey key : keys) text += ShaderPool::getShaderKeyText(key) + " ";
        return text;
    }

}

TEST_F(SceneShaderKeys, ASaveDoesNotDependOnWhatHasDrawn) {
    Project project;
    auto scene = std::make_unique<Scene>();
    const Entity light = scene->createEntity();
    scene->addComponent<Transform>(light, {});
    LightComponent lightComponent;
    lightComponent.shadows = true;
    scene->addComponent<LightComponent>(light, lightComponent);
    const Entity entity = scene->createEntity();
    scene->addComponent<Transform>(entity, {});
    MeshComponent mesh;
    mesh.numSubmeshes = 1;
    scene->addComponent<MeshComponent>(entity, mesh);

    SceneProject sceneProject;
    sceneProject.id = 1;
    sceneProject.name = "Keys";
    sceneProject.scene = scene.get();
    sceneProject.entities = {light, entity};

    std::set<ShaderKey> beforeDraw;
    project.collectSavedShaderKeys(&sceneProject, beforeDraw);

    // what a draw leaves on the mesh: loaded, with the variants it was loaded with (a depth
    // variant RenderSystem computed while the light's shadows ran)
    MeshComponent& loaded = scene->getComponent<MeshComponent>(entity);
    loaded.loaded = true;
    loaded.submeshes[0].shaderProperties = shaderPropertyBit(4);
    loaded.submeshes[0].depthShaderProperties = shaderPropertyBit(9);
    std::set<ShaderKey> afterDraw;
    project.collectSavedShaderKeys(&sceneProject, afterDraw);

    EXPECT_EQ(afterDraw, beforeDraw) << "before: " << describe(beforeDraw) << "\nafter: " << describe(afterDraw);
    sceneProject.scene = nullptr;
}

TEST_F(SceneShaderKeys, TheFilesKeysStay) {
    Project project;
    auto scene = std::make_unique<Scene>();
    SceneProject sceneProject;
    sceneProject.id = 1;
    sceneProject.scene = scene.get();
    // a key its file listed (or Play added at Stop): still saved
    const ShaderKey recorded = ShaderPool::getShaderKey(ShaderType::MESH, shaderPropertyBit(4) | shaderPropertyBit(7), 0);
    sceneProject.shaderKeys.insert(recorded);
    std::set<ShaderKey> saved;
    project.collectSavedShaderKeys(&sceneProject, saved);
    EXPECT_EQ(saved.count(recorded), 1u) << describe(saved);
    sceneProject.scene = nullptr;
}
