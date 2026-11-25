// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A prefab's submesh linked to a .material file (Material::name, S04 deviation 1) takes the file's
// current values everywhere the prefab is read, as a scene's do when it opens (S98 REQ-29, AC29):
// Prefab Mode, an instance in a scene, the cooked prefab, Play's template. The prefab file keeps
// its embedded copy until the prefab itself is saved. A prefab file from before (version 9) keeps
// using the copies it embeds, as it did (StreamMigrations prefab 9 -> 10).

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "Engine.h"
#include "PrefabStage.h"
#include "Project.h"
#include "Stream.h"
#include "StreamMigrations.h"
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
    jasperengine::editor::test::ensureEngine();
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
        std::string error;
        const uint32_t sceneId = project->createSceneNow("Boxes", SceneType::SCENE_3D, {}, false, UnsavedChoice::Discard, &error);
        SceneProject* sceneProject = project->getScene(sceneId);
        ASSERT_NE(sceneProject, nullptr) << error;
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
        ASSERT_TRUE(project->saveSceneNow(sceneId, "scenes/Boxes.scene", &error)) << error;
        jasperengine::editor::test::closeProject(*project);
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
    const std::vector<uint8_t> cooked = jasperengine::editor::cook::SceneCooker::cookPrefab(PREFAB, *found->second, project->getPrefabGuid(PREFAB), 1);
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

    jasperengine::editor::test::closeProject(*project);
}

// A prefab saved before REQ-29 (version 9) whose submesh links a .material: the migration stamps
// followMaterialFiles: false, and the asset, Prefab Mode and the cooked prefab keep the red copy
// the file embeds while the .material is blue, before and after a save. A version 9 prefab without
// a link gets no key.
TEST(ProjectPrefabMaterials, AnOlderPrefabKeepsItsEmbeddedCopies) {
    const fs::path fixturePath = jasperengine::editor::test::fixture("migrations/prefab-v9-linked-material.prefab");
    YAML::Node migrated = YAML::Load(readBytes(fixturePath));
    ASSERT_EQ(migrated["version"].as<int>(), 9);
    const MigrationResult result = StreamMigrations::migrate(DocumentKind::Prefab, migrated);
    EXPECT_EQ(migrated["version"].as<int>(), Prefab::FILE_VERSION);
    ASSERT_TRUE(migrated["followMaterialFiles"]);
    EXPECT_FALSE(migrated["followMaterialFiles"].as<bool>());
    ASSERT_EQ(result.log.lines.size(), 1u);
    EXPECT_NE(result.log.lines[0].find("keeps the material copies"), std::string::npos) << result.log.lines[0];
    YAML::Node unlinked = YAML::Load(readBytes(fixturePath));
    unlinked["members"][0]["components"]["mesh"]["submeshes"][0]["material"].remove("name");
    StreamMigrations::migrate(DocumentKind::Prefab, unlinked);
    EXPECT_FALSE(unlinked["followMaterialFiles"]) << "no link, nothing to keep";

    jasperengine::editor::test::ensureEngine();
    Engine::pauseGameEvents(true);
    Engine::setAsyncLoading(false);
    jasperengine::test::TempDir dir;
    const fs::path root = dir.path() / "project";
    fs::create_directories(root / ".jasperengine");
    fs::create_directories(root / "prefabs");
    std::ofstream(root / "project.yaml") << "name: prefabmaterials\n";
    fs::copy_file(fixturePath, root / PREFAB);
    writeMaterial(root, BLUE);

    auto check = [&](Project& project, const char* when) {
        const Prefab* asset = project.loadPrefabAsset(PREFAB);
        ASSERT_NE(asset, nullptr) << when;
        EXPECT_FALSE(asset->followMaterialFiles) << when;
        EXPECT_EQ(meshColor(asset->registry.get(), asset->registryEntities), RED) << when << ": the prefab asset";
        ASSERT_TRUE(project.openPrefabStage(PREFAB)) << when;
        PrefabStage* stage = project.getActivePrefabStage();
        ASSERT_NE(stage, nullptr) << when;
        EXPECT_EQ(meshColor(stage->sceneProject.scene, stage->sceneProject.entities), RED) << when << ": Prefab Mode";
        project.closePrefabStages(0, nullptr, 2);
        // (closing the stage dropped the asset no scene uses: read again)
        asset = project.loadPrefabAsset(PREFAB);
        ASSERT_NE(asset, nullptr) << when;
        // a change of the .material reaches no copy of it
        project.reapplyLinkedMaterialFiles();
        EXPECT_EQ(meshColor(asset->registry.get(), asset->registryEntities), RED) << when << ": after the files are read again";
        const auto baked = project.bakePrefabsForExport();
        const auto found = baked.find(fs::path(PREFAB));
        ASSERT_NE(found, baked.end()) << when;
        const std::vector<uint8_t> cooked = jasperengine::editor::cook::SceneCooker::cookPrefab(PREFAB, *found->second, project.getPrefabGuid(PREFAB), 1);
        Scene player;
        const Entity playerRoot = player.createEntity();
        std::vector<Entity> members;
        std::string error;
        ASSERT_TRUE(CookedSceneLoader::instantiatePrefab(&player, playerRoot, std::span<const uint8_t>(cooked.data(), cooked.size()), &members, &error))
            << error;
        members.push_back(playerRoot);
        EXPECT_EQ(meshColor(&player, members), RED) << when << ": the cooked prefab";
    };

    {
        std::unique_ptr<Project> project = openProject(root);
        check(*project, "migrated");
        // a save writes the current version and keeps the key
        ASSERT_EQ(project->savePrefabToDisk(PREFAB, DegradedSavePolicy::Refuse), SaveResult::Saved);
        jasperengine::editor::test::closeProject(*project);
    }
    const YAML::Node saved = YAML::Load(readBytes(root / PREFAB));
    EXPECT_EQ(saved["version"].as<int>(), Prefab::FILE_VERSION);
    ASSERT_TRUE(saved["followMaterialFiles"]) << readBytes(root / PREFAB);
    EXPECT_FALSE(saved["followMaterialFiles"].as<bool>());

    std::unique_ptr<Project> project = openProject(root);
    check(*project, "saved");
    jasperengine::editor::test::closeProject(*project);
}
