// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Changing the assets directory (project-window spec 6.9, Doriax 35c8e31da and c3aed7ee9): the
// preflight refuses before anything moves, a model's sidecar and a Vertex Animation's pair go with
// it, folders the moves emptied are removed, a scene that could not be saved stays loaded and is
// listed, and the plan changes nothing. On tools/fixtures/textures (assets dir "assets": a scene
// referencing textures/ao.png and models/panel.gltf, materials/Rock.material three textures).

#include "asset/FileOpsTestProject.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    class AssetRootChangeTest : public FileOpsProjectTest {
    protected:
        const char* fixture() const override { return "textures"; }

        void prepare() override {
            // a texture alone in its folder, referenced by a second material
            fs::create_directories(abs("assets/solo"));
            fs::copy_file(abs("assets/textures/sprite.png"), abs("assets/solo/one.png"));
            dir.write("materials/Solo.material",
                      "name: materials/Solo.material\nbaseColorTexture: {textureType: 2D, source: path, path: solo/one.png}\n");
            // a model in a folder that holds a script too (moved alone), with its humanoid mapping
            fs::create_directories(abs("assets/mixed"));
            fs::copy_file(abs("assets/models/panel.gltf"), abs("assets/mixed/hero.gltf"));
            dir.write("assets/mixed/hero.gltf.humanoid", "{}\n");
            dir.write("assets/mixed/notes.lua", "return {}\n");
            std::string scene = text("Main.scene");
            scene += "  - type: Entity\n    entity: 1003\n    name: Hero\n    components:\n      transform:\n"
                     "        position: [0, 0, 0]\n        rotation: [1, 0, 0, 0]\n        scale: [1, 1, 1]\n"
                     "      model:\n        filename: mixed/hero.gltf\n";
            dir.write("Main.scene", scene);
        }

        static bool listed(const std::vector<fs::path>& paths, const fs::path& path) {
            return std::find(paths.begin(), paths.end(), path) != paths.end();
        }
    };

}

TEST_F(AssetRootChangeTest, DifferingDestinationRefusesAndChangesNothing) {
    dir.write("content/textures/photo.png", "another photo");
    const AssetRootChange change = project->changeAssetRoots("content", project->getLuaDir(), true);
    EXPECT_FALSE(change.ok());
    EXPECT_NE(change.blocker.find("content/textures/photo.png already exists and differs"), std::string::npos) << change.blocker;
    EXPECT_FALSE(change.changed);
    EXPECT_EQ(project->getAssetsDir(), fs::path("assets"));
    EXPECT_TRUE(exists("assets/textures/ao.png")) << "nothing moved";
    EXPECT_TRUE(exists("assets/models/panel.gltf"));
    EXPECT_EQ(text("content/textures/photo.png"), "another photo");
    EXPECT_NE(text("Main.scene").find("textures/ao.png"), std::string::npos);
}

TEST_F(AssetRootChangeTest, IdenticalDestinationIsUsedAndReported) {
    fs::create_directories(abs("content/textures"));
    fs::copy_file(abs("assets/textures/photo.png"), abs("content/textures/photo.png"));
    const AssetRootChange change = project->changeAssetRoots("content", project->getLuaDir(), true);
    ASSERT_TRUE(change.ok()) << change.refusal();
    EXPECT_TRUE(listed(change.alreadyThere, abs("content/textures/photo.png")));
    EXPECT_EQ(project->getAssetsDir(), fs::path("content"));
}

TEST_F(AssetRootChangeTest, UnwritableSceneRefusesBeforeAnyMove) {
    const fs::path scene = abs("Main.scene");
    project->setAssetRootWriteProbe([scene](const fs::path& file) { return file.lexically_normal() != scene.lexically_normal(); });
    const AssetRootChange change = project->changeAssetRoots("content", project->getLuaDir(), true);
    EXPECT_EQ(change.blocker, "Main.scene cannot be written");
    EXPECT_FALSE(exists("content"));
    EXPECT_TRUE(exists("assets/textures/ao.png"));
    EXPECT_EQ(project->getAssetsDir(), fs::path("assets"));
}

TEST_F(AssetRootChangeTest, FailedSaveKeepsTheSceneLoadedAndListsIt) {
    // the project folder as the assets directory: nothing moves, every reference is rewritten
    // ("textures/ao.png" -> "assets/textures/ao.png"). The probe passes, then the scene's path
    // becomes a folder: its save fails
    const fs::path scene = abs("Main.scene");
    project->setAssetRootWriteProbe([scene](const fs::path& file) {
        if (file.lexically_normal() == scene.lexically_normal()) {
            fs::remove(scene);
            fs::create_directories(scene / "blocker");
        }
        return true;
    });
    const AssetRootChange change = project->changeAssetRoots(".", project->getLuaDir(), true);
    ASSERT_TRUE(change.ok()) << change.refusal();
    EXPECT_TRUE(change.changed);
    EXPECT_TRUE(change.moves.empty()) << "everything is inside the project folder already";
    EXPECT_TRUE(listed(change.failedSaves, scene)) << "the scene keeps the new paths in memory only";
    bool loaded = false;
    for (const SceneProject& candidate : project->getScenes()) {
        if (candidate.filepath == fs::path("Main.scene") && candidate.scene) loaded = candidate.isModified;
    }
    EXPECT_TRUE(loaded) << "a scene whose save failed stays loaded and modified";
}

TEST_F(AssetRootChangeTest, EmptiedFoldersAreRemovedButNotRootsOrLinks) {
    const AssetRootChange change = project->changeAssetRoots("content", project->getLuaDir(), true);
    ASSERT_TRUE(change.ok()) << change.refusal();
    EXPECT_TRUE(exists("content/solo/one.png"));
    EXPECT_FALSE(exists("assets/solo")) << "the folder the move emptied is removed";
    EXPECT_TRUE(listed(change.removedFolders, abs("assets/solo")));
    EXPECT_TRUE(exists("assets")) << "the old root stays";
    EXPECT_TRUE(exists("assets/textures/gradient.png")) << "unreferenced files stay where they are";
    EXPECT_NE(text("materials/Solo.material").find("solo/one.png"), std::string::npos);
}

TEST_F(AssetRootChangeTest, HumanoidSidecarMovesWithItsModel) {
    const AssetRootChange change = project->changeAssetRoots("content", project->getLuaDir(), true);
    ASSERT_TRUE(change.ok()) << change.refusal();
    EXPECT_TRUE(exists("content/mixed/hero.gltf"));
    EXPECT_TRUE(exists("content/mixed/hero.gltf.humanoid")) << "the bone mapping goes with its model";
    EXPECT_FALSE(exists("assets/mixed/hero.gltf.humanoid"));
    EXPECT_TRUE(exists("assets/mixed/notes.lua")) << "the folder holds a script: the model moved alone";
}

TEST_F(AssetRootChangeTest, DryRunChangesNothing) {
    const AssetRootChange plan = project->planAssetRootChange("content", project->getLuaDir(), true);
    ASSERT_TRUE(plan.ok()) << plan.refusal();
    EXPECT_FALSE(plan.changed);
    EXPECT_FALSE(exists("content"));
    EXPECT_EQ(project->getAssetsDir(), fs::path("assets"));
    bool movesAo = false;
    for (const AssetRootChange::Move& move : plan.moves) {
        if (move.from == abs("assets/textures/ao.png") && move.to == abs("content/textures/ao.png")) movesAo = true;
    }
    EXPECT_TRUE(movesAo);
    // the layout under the root is kept: "textures/ao.png" stays the stored path, nothing but
    // project.yaml is rewritten
    EXPECT_FALSE(listed(plan.rewritten, abs("Main.scene")));
    EXPECT_TRUE(listed(plan.rewritten, abs("project.yaml")));
    // the project folder as the root: nothing moves, the references are rewritten
    const AssetRootChange up = project->planAssetRootChange(".", project->getLuaDir(), true);
    ASSERT_TRUE(up.ok()) << up.refusal();
    EXPECT_TRUE(up.moves.empty());
    EXPECT_TRUE(listed(up.rewritten, abs("Main.scene")));
    EXPECT_TRUE(listed(up.rewritten, abs("materials/Rock.material")));

    const AssetRootChange done = project->changeAssetRoots("content", project->getLuaDir(), true);
    ASSERT_TRUE(done.ok()) << done.refusal();
    for (const AssetRootChange::Move& move : plan.moves) {
        EXPECT_TRUE(fs::exists(move.to)) << move.to << " was planned";
    }
}

TEST_F(AssetRootChangeTest, LinkedDestinationRefuses) {
    // content/textures leads outside the project: a move there would leave it
    jasperengine::test::TempDir outside;
    fs::create_directories(abs("content"));
    fs::create_directory_symlink(outside.path(), abs("content/textures"));
    const AssetRootChange change = project->planAssetRootChange("content", project->getLuaDir(), true);
    EXPECT_NE(change.blocker.find("through a link"), std::string::npos) << change.blocker;
}

TEST_F(AssetRootChangeTest, NewRootUnderAFileRefuses) {
    dir.write("afile", "x");
    const AssetRootChange change = project->changeAssetRoots("afile/content", project->getLuaDir(), true);
    EXPECT_EQ(change.blocker, "a new directory is a file or lies under one");
    EXPECT_EQ(project->getAssetsDir(), fs::path("assets"));
}

// S68: a linked folder is a folder of the project. A referenced file reached through one moves
// with the link, and the link's target (outside the project here) is left as it is
TEST_F(AssetRootChangeTest, ALinkedFolderMovesAsTheLinkAndItsTargetStays) {
    jasperengine::test::TempDir shared;
    shared.write("x.png", "shared texture");
    fs::create_directory_symlink(shared.path(), abs("assets/lib"));
    dir.write("materials/Lib.material", "name: materials/Lib.material\nbaseColorTexture: {textureType: 2D, source: path, path: lib/x.png}\n");
    const AssetRootChange change = project->changeAssetRoots("content", project->getLuaDir(), true);
    ASSERT_TRUE(change.ok()) << change.refusal();
    EXPECT_TRUE(fs::is_symlink(abs("content/lib"))) << "the link moved";
    EXPECT_FALSE(fs::exists(fs::symlink_status(abs("assets/lib"))));
    EXPECT_EQ(shared.read("x.png"), "shared texture") << "the target is untouched";
    EXPECT_NE(text("materials/Lib.material").find("lib/x.png"), std::string::npos) << "the reference still names it";
}
