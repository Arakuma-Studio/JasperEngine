// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A closed scene's shader keys computed from its file (docs/specs/S64-shader-lists.md). These
// binaries start the engine without a graphics device, so a read that asked for one would not get
// through: that is R2's "without GPU work". The consumers, an export among them, are compared in
// tools/mcp/tests/test_shader_lists.py.

#include <gtest/gtest.h>

#include "Project.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderBuildTypes.h"

#include "support/EditorTestSupport.h"
#include "support/TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    void writeText(const fs::path& path, const std::string& text) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

    // A scene as a file holds it, with no saved shader keys
    std::string sceneText(int id, const std::string& name, const std::string& marker = {}) {
        return "id: " + std::to_string(id) + "\nname: " + name + "\nsceneType: scene_2d\nscene:\n  backgroundColor: [0.1, 0.2, 0.3, 1]\n"
               "entities:\n  - type: Entity\n    entity: 1001\n    name: Marker\n    components:\n      transform:\n"
               "        position: [0, 0, 0]\n        scale: [1, 1, 1]\n        rotation: [1, 0, 0, 0]\n" + marker;
    }

    class ProjectShaderList : public ::testing::Test {
    protected:
        jasperengine::test::TestTempDir dir{"shader-list"};
        Project project;

        static void SetUpTestSuite() {
            editor::test::ensureEngine();
        }

        void SetUp() override {
            fs::create_directories(dir.path() / "assets");
            fs::create_directories(dir.path() / "scripts");
            writeText(dir.path() / "Open.scene", sceneText(11, "Open"));
            writeText(dir.path() / "Closed.scene", sceneText(12, "Closed"));
            writeText(dir.path() / "project.yaml",
                      "name: Lists\nassetsDir: assets\nluaDir: scripts\nscenes:\n  - filepath: Open.scene\n  - filepath: Closed.scene\n");
            project.setSkipProjectLock(true);
            ASSERT_TRUE(project.loadProject(dir.path(), false));
        }

        const SceneProject* closedScene() {
            for (const SceneProject& scene : project.getScenes()) {
                if (scene.name == "Closed") return &scene;
            }
            return nullptr;
        }
    };

}

TEST_F(ProjectShaderList, ReadsClosedScenesWithoutADevice) {
    const SceneProject* closed = closedScene();
    ASSERT_NE(closed, nullptr);
    ASSERT_EQ(closed->scene, nullptr) << "only the first scene opens";
    ASSERT_TRUE(closed->shaderKeys.empty()) << "its file saved none";

    const ShaderListStatus status = project.refreshClosedSceneShaderKeys();
    EXPECT_TRUE(status.complete());
    EXPECT_TRUE(status.unreadableScenes.empty());
    EXPECT_EQ(status.scenesRead, 1u);
    // every scene presents through the blit
    EXPECT_EQ(closed->shaderKeys.count(ShaderPool::getShaderKey(ShaderType::BLIT, 0)), 1u);
    EXPECT_EQ(closed->scene, nullptr) << "and it stays closed";
}

TEST_F(ProjectShaderList, WhatTheKeysAlsoReadCountsAsAChange) {
    ASSERT_EQ(project.refreshClosedSceneShaderKeys().scenesRead, 1u);
    // a script can reach other variants (Project::scriptsMention)
    writeText(dir.path() / "scripts" / "game.lua", "scene.postProcessing = true\n");
    EXPECT_EQ(project.refreshClosedSceneShaderKeys().scenesRead, 1u);
    EXPECT_EQ(project.refreshClosedSceneShaderKeys().scenesRead, 0u);
}

TEST_F(ProjectShaderList, ALimitLeavesTheRestPending) {
    ShaderListLimit none;
    none.maxScenes = 0;
    const ShaderListStatus waiting = project.refreshClosedSceneShaderKeys(none);
    EXPECT_FALSE(waiting.complete());
    ASSERT_EQ(waiting.pendingScenes.size(), 1u);
    EXPECT_EQ(waiting.pendingScenes[0], "Closed");
    EXPECT_EQ(waiting.scenesRead, 0u);
    EXPECT_TRUE(closedScene()->shaderKeys.empty());

    ShaderListLimit tooBig;
    tooBig.maxFileBytes = 1;
    EXPECT_FALSE(project.refreshClosedSceneShaderKeys(tooBig).complete());

    EXPECT_TRUE(project.refreshClosedSceneShaderKeys().complete());
}

TEST_F(ProjectShaderList, AFileThatDoesNotReadIsNamed) {
    ASSERT_TRUE(project.refreshClosedSceneShaderKeys().complete());
    const std::set<ShaderKey> known = closedScene()->shaderKeys;

    for (const std::string& text : {std::string("id: [not yaml\n"), std::string("id: 12\nname: Empty\n")}) {
        writeText(dir.path() / "Closed.scene", text);
        const ShaderListStatus status = project.refreshClosedSceneShaderKeys();
        EXPECT_TRUE(status.complete()) << text;
        ASSERT_EQ(status.unreadableScenes.size(), 1u) << text;
        EXPECT_EQ(status.unreadableScenes[0].path, "Closed.scene");
        EXPECT_EQ(status.unreadableScenes[0].name, "Closed");
        EXPECT_FALSE(status.unreadableScenes[0].error.empty());
        EXPECT_EQ(closedScene()->shaderKeys, known) << "the list keeps what it last knew";
    }

    fs::remove(dir.path() / "Closed.scene");
    const ShaderListStatus gone = project.refreshClosedSceneShaderKeys();
    ASSERT_EQ(gone.unreadableScenes.size(), 1u);
    EXPECT_NE(gone.unreadableScenes[0].error.find("missing"), std::string::npos);

    writeText(dir.path() / "Closed.scene", sceneText(12, "Closed"));
    EXPECT_TRUE(project.refreshClosedSceneShaderKeys().unreadableScenes.empty()) << "and a fixed file reads again";
}

TEST_F(ProjectShaderList, EveryConsumerGetsTheSameList) {
    ASSERT_TRUE(project.refreshClosedSceneShaderKeys().complete());
    std::set<ShaderKey> sceneKeys;
    project.collectBuildShaderKeys(sceneKeys);
    EXPECT_EQ(sceneKeys.count(ShaderPool::getShaderKey(ShaderType::BLIT, 0)), 1u);

    const ShaderOverrides& overrides = project.getShaderOverrides();
    EXPECT_EQ(project.collectStandaloneShaderKeys(), project.finishBuildShaderKeys(sceneKeys, overrides.additions, overrides.exclusions));

    const ShaderKey blit = ShaderPool::normalizeKey(ShaderPool::getShaderKey(ShaderType::BLIT, 0));
    const ShaderKey added = ShaderPool::normalizeKey(ShaderPool::getShaderKey(ShaderType::SKYBOX, 1));
    const std::set<ShaderKey> finished = project.finishBuildShaderKeys(sceneKeys, {added}, {blit});
    EXPECT_EQ(finished.count(added), 1u);
    EXPECT_EQ(finished.count(blit), 0u);
}
