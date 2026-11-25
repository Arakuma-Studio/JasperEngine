// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The project's own CMake build reads what a game reads (S98 REQ-2, AC4): a scene save cooks the
// scene and the build's prefabs into <internal>/cooked, beside the game.json naming them, and no
// generated C++ per scene or prefab is written; the generated scene_*/prefab_* an older editor
// left in <internal>/generated go when the project opens.

#include "asset/FileOpsTestProject.h"

#include <gtest/gtest.h>

#include "player/PlayerConfig.h"
#include "serialization/CookedScene.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    class ProjectCookCacheTest : public FileOpsProjectTest {
    protected:
        void prepare() override {
            dir.write(".jasperengine/generated/scene_Main_Scene_1.cpp", "void create_scene_Main_Scene_1(Scene*) {}\n");
            dir.write(".jasperengine/generated/prefab_prefabs_Bullet.h", "#pragma once\n");
            dir.write(".jasperengine/generated/prefab_prefabs_Bullet.cpp", "bool create_prefab_prefabs_Bullet() { return true; }\n");
            dir.write(".jasperengine/generated/main.cpp", "// this editor's\n");
        }

        std::vector<std::string> generatedSceneSources() const {
            std::vector<std::string> names;
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(abs(".jasperengine/generated"), ec)) {
                const std::string name = entry.path().filename().string();
                if (name.rfind("scene_", 0) == 0 || name.rfind("prefab_", 0) == 0) names.push_back(name);
            }
            return names;
        }
    };

}

TEST_F(ProjectCookCacheTest, WritesCookedSceneOnSave) {
    EXPECT_EQ(generatedSceneSources(), std::vector<std::string>{}) << "an older editor's scene and prefab sources go when the project opens";
    EXPECT_TRUE(exists(".jasperengine/generated/main.cpp")) << "the rest of the cache stays";

    SceneProject* scene = nullptr;
    for (SceneProject& candidate : project->getScenes()) {
        if (candidate.filepath == "Main Scene.scene") scene = &candidate;
    }
    ASSERT_NE(scene, nullptr);
    if (!scene->scene) project->loadScene(scene->filepath, false, false, true);
    ASSERT_NE(scene->scene, nullptr);
    ASSERT_TRUE(project->saveSceneToPath(scene->id, scene->filepath));

    // the scene, cooked as a build cooks it, loads
    const fs::path cooked = abs(".jasperengine/cooked/scenes/" + std::to_string(scene->id) + ".jscn");
    ASSERT_TRUE(fs::exists(cooked)) << cooked;
    std::ifstream file(cooked, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Scene loaded;
    std::string error;
    ASSERT_TRUE(CookedSceneLoader::loadScene(&loaded, std::span<const uint8_t>(bytes.data(), bytes.size()), &error)) << error;
    ASSERT_FALSE(scene->entities.empty());
    for (Entity entity : scene->entities) {
        ASSERT_TRUE(loaded.isEntityCreated(entity)) << scene->scene->getEntityName(entity);
        EXPECT_EQ(loaded.getEntityName(entity), scene->scene->getEntityName(entity));
    }
    EXPECT_EQ(generatedSceneSources(), std::vector<std::string>{}) << "no generated C++ per scene or prefab";

    // the build's prefabs, and the game.json naming both
    player::PlayerConfig game;
    ASSERT_TRUE(player::PlayerConfig::parse(text(".jasperengine/cooked/game.json"), game, error)) << error;
    const player::GameScene* listed = game.findScene(scene->id);
    ASSERT_NE(listed, nullptr);
    EXPECT_TRUE(fs::exists(abs(".jasperengine/cooked/" + listed->file)));
    ASSERT_FALSE(game.prefabs.empty());
    for (const player::GamePrefab& prefab : game.prefabs) EXPECT_TRUE(fs::exists(abs(".jasperengine/cooked/" + prefab.file))) << prefab.name;
}
