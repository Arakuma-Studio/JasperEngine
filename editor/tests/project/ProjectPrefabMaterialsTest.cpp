// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A prefab's submesh linked to a .material file (Material::name, S04 deviation 1) takes the file's
// current values everywhere the prefab is read, as a scene's do when it opens (S98 REQ-29, AC29):
// Prefab Mode, an instance in a scene, the cooked prefab, Play's template. The prefab file keeps
// its embedded copy until the prefab itself is saved.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "Engine.h"
#include "PrefabStage.h"
#include "Project.h"
#include "Stream.h"
#include "TempDir.h"
#include "command/CommandHandle.h"
#include "command/type/CreatePrefabCmd.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "cook/SceneCooker.h"
#include "serialization/CookedScene.h"

#include "yaml-cpp/yaml.h"

#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace jasperengine::editor {

    // Play's prefab templates (a prefab a script instantiates is copied from one)
    struct PrefabMaterialsTestAccess {
        static Vector4 templateColor(Project& project, const fs::path& prefabPath) {
            const Project::PlayPrefabTemplate* found = project.getPlayPrefabTemplate(prefabPath);
            Vector4 color(-1.0f, -1.0f, -1.0f, -1.0f);
            if (found && project.playTemplateProject && project.playTemplateProject->scene) {
                for (Entity entity : found->entities) {
                    if (const MeshComponent* mesh = project.playTemplateProject->scene->findComponent<MeshComponent>(entity)) {
                        color = mesh->submeshes[0].material.baseColorFactor;
                    }
                }
            }
            project.clearPlayPrefabTemplates();
            return color;
        }
    };

}

namespace prefab_materials_test {

    const Vector4 RED(1.0f, 0.0f, 0.0f, 1.0f);
    const Vector4 BLUE(0.0f, 0.0f, 1.0f, 1.0f);
    const char* MATERIAL = "materials/Box.material";
    const char* PREFAB = "prefabs/Box.prefab";

    void writeMaterial(const fs::path& root, const Vector4& color) {
        Material material;
        material.name = MATERIAL;
        material.baseColorFactor = color;
        fs::create_directories(root / "materials");
        std::ofstream(root / MATERIAL, std::ios::binary) << YAML::Dump(Stream::encodeMaterial(material)) << "\n";
    }

    std::string readBytes(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    std::unique_ptr<Project> openProject(const fs::path& root) {
        auto project = std::make_unique<Project>();
        project->setSkipProjectLock(true);
        test::useProjectPaths(project.get());
        EXPECT_TRUE(project->loadProject(root, false));
        return project;
    }

    // the first submesh colour of a GameObject with a mesh among `entities`
    Vector4 meshColor(EntityRegistry* registry, const std::vector<Entity>& entities) {
        for (Entity entity : entities) {
            if (const MeshComponent* mesh = registry->findComponent<MeshComponent>(entity)) {
                return mesh->submeshes[0].material.baseColorFactor;
            }
        }
        return Vector4(-1.0f, -1.0f, -1.0f, -1.0f);
    }

}

using namespace prefab_materials_test;

TEST(ProjectPrefabMaterials, LinkedMaterialWinsOverEmbeddedCopy) {
    test::ensureEngine();
    Engine::pauseGameEvents(true);
    Engine::setAsyncLoading(false);
    jasperengine::test::TempDir dir;
    const fs::path root = dir.path() / "project";
    fs::create_directories(root / ".jasperengine");
    std::ofstream(root / "project.yaml") << "name: prefabmaterials\n";
    writeMaterial(root, RED);

    // a prefab made from a GameObject whose submesh links the red .material: its file embeds red
    {
        std::unique_ptr<Project> project = openProject(root);
        const uint32_t sceneId = project->createNewScene("Boxes", SceneType::SCENE_3D);
        SceneProject* sceneProject = project->getScene(sceneId);
        ASSERT_NE(sceneProject, nullptr);
        Scene* scene = sceneProject->scene;
        const Entity box = scene->createEntity();
        scene->addComponent<Transform>(box);
        sceneProject->entities.push_back(box);
        MeshComponent mesh;
        mesh.numSubmeshes = 1;
        mesh.submeshes[0].material.name = MATERIAL;
        mesh.submeshes[0].material.baseColorFactor = RED;
        scene->addComponent<MeshComponent>(box, mesh);
        CommandHandle::get(sceneId)->addCommandNoMerge(new CreatePrefabCmd(project.get(), sceneId, PREFAB,
            Stream::encodeEntity(box, scene, project.get(), sceneProject)));
        ASSERT_TRUE(project->isPrefabInstanceRoot(sceneId, box));
        std::string error;
        ASSERT_TRUE(project->saveSceneNow(sceneId, "scenes/Boxes.scene", &error)) << error;
        test::closeProject(*project);
    }
    ASSERT_NE(readBytes(root / PREFAB).find("materials/Box.material"), std::string::npos);

    // the .material turns blue while the prefab is closed
    writeMaterial(root, BLUE);
    const std::string prefabBefore = readBytes(root / PREFAB);

    std::unique_ptr<Project> project = openProject(root);
    uint32_t sceneId = project->findSceneByPath(root / "scenes/Boxes.scene");
    ASSERT_NE(sceneId, NULL_PROJECT_SCENE);
    if (!project->getScene(sceneId)->scene) {
        project->loadScene(project->getScene(sceneId)->filepath, true, false, true);
    }
    SceneProject* sceneProject = project->getScene(sceneId);
    ASSERT_NE(sceneProject->scene, nullptr);

    // the instance in the scene
    EXPECT_EQ(meshColor(sceneProject->scene, sceneProject->entities), BLUE) << "the scene's instance";
    // the asset every instance, Play and the build read
    const Prefab* asset = project->getPrefab(fs::path(PREFAB));
    ASSERT_NE(asset, nullptr);
    EXPECT_EQ(meshColor(asset->registry.get(), asset->registryEntities), BLUE) << "the prefab asset";

    // Prefab Mode
    ASSERT_TRUE(project->openPrefabStage(PREFAB));
    PrefabStage* stage = project->getActivePrefabStage();
    ASSERT_NE(stage, nullptr);
    EXPECT_EQ(meshColor(stage->sceneProject.scene, stage->sceneProject.entities), BLUE) << "Prefab Mode";
    project->closePrefabStages(0, nullptr, 2);

    // Play's template
    EXPECT_EQ(PrefabMaterialsTestAccess::templateColor(*project, PREFAB), BLUE) << "Play's template";

    // the cooked prefab a prebuilt player reads
    const auto baked = project->bakePrefabsForExport();
    const auto found = baked.find(fs::path(PREFAB));
    ASSERT_NE(found, baked.end());
    const std::vector<uint8_t> cooked = cook::SceneCooker::cookPrefab(PREFAB, *found->second, project->getPrefabGuid(PREFAB), 1);
    Scene player;
    const Entity playerRoot = player.createEntity();
    std::vector<Entity> members;
    std::string error;
    ASSERT_TRUE(CookedSceneLoader::instantiatePrefab(&player, playerRoot, std::span<const uint8_t>(cooked.data(), cooked.size()), &members, &error))
        << error;
    members.push_back(playerRoot);
    EXPECT_EQ(meshColor(&player, members), BLUE) << "the cooked prefab";

    // nothing wrote the prefab: its embedded copy changes only when it is saved
    ASSERT_TRUE(project->saveSceneNow(sceneId, {}, &error)) << error;
    EXPECT_EQ(readBytes(root / PREFAB), prefabBefore) << "the .prefab file is unchanged";

    test::closeProject(*project);
}
