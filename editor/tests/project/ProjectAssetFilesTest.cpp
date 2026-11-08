// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What the project writes and lists for its asset files, on a headless project: a .material's text
// keeps the identity and the line endings of the file it replaces, even one the asset database
// has not seen yet (BF23), and export's texture list leaves out sources that were never in it.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "AppSettings.h"
#include "Engine.h"
#include "Project.h"
#include "texture/Material.h"
#include "texture/TextureCookQueue.h"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct AssetFilesProject {
        bool ready = false;
        fs::path root;
        std::unique_ptr<Project> project;
    };

    AssetFilesProject& assetFilesProject() {
        static AssetFilesProject h = []() {
            AssetFilesProject out;
            out.root = fs::temp_directory_path() / ("jasper-project-asset-files-test-" + std::to_string(::getpid()));
            fs::remove_all(out.root);
            fs::create_directories(out.root / "config");
            setenv("XDG_CONFIG_HOME", (out.root / "config").c_str(), 1);
            const fs::path dir = out.root / "project";
            fs::create_directories(dir / ".jasperengine");
            fs::create_directories(dir / "assets");
            std::ofstream(dir / "project.yaml") << "name: assetfiles\nassetsDir: assets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(dir, false);
            return out;
        }();
        if (h.project) editor::test::useProjectPaths(h.project.get());
        return h;
    }

    std::string readBytes(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

}

TEST(MaterialFileText, KeepsTheIdentityAndLineEndingsOfTheFileItReplaces) {
    AssetFilesProject& h = assetFilesProject();
    ASSERT_TRUE(h.ready);
    // copied in from outside: the database has not scanned it yet
    const std::string guid = "0123456789abcdef0123456789abcdef";
    const fs::path file = h.project->getAssetsPath() / "copied.material";
    std::ofstream(file, std::ios::binary) << "formatVersion: 1\r\nguid: " << guid << "\r\nmetallicFactor: 0.5\r\n";
    Material material;
    material.metallicFactor = 0.25f;
    const std::string text = h.project->materialFileText(file, material);
    EXPECT_NE(text.find("guid: " + guid), std::string::npos) << text;
    EXPECT_NE(text.find("\r\n"), std::string::npos) << "a file of a Windows checkout keeps its line endings";
    EXPECT_EQ(text.find("\n", text.find("\r\n") + 2), text.find("\r\n", text.find("\r\n") + 2) + 1) << "every line";
}

// an .exr has an identity, but export never cooked them: a project's export stays what it was
TEST(ProjectTextureSources, LeavesExrOutOfExport) {
    AssetFilesProject& h = assetFilesProject();
    ASSERT_TRUE(h.ready);
    const fs::path assets = h.project->getAssetsPath();
    std::ofstream(assets / "listed.png", std::ios::binary) << "png";
    std::ofstream(assets / "source.exr", std::ios::binary) << "exr";
    h.project->refreshAssetDatabase(true);
    ASSERT_FALSE(h.project->getAssetDatabase().getAssetGuid("assets/source.exr").isNull()) << "it has its identity";
    const std::vector<std::string> sources = projectTextureSources(*h.project, true);
    EXPECT_NE(std::find(sources.begin(), sources.end(), "listed.png"), sources.end());
    EXPECT_EQ(std::find(sources.begin(), sources.end(), "source.exr"), sources.end());
}

TEST(ProjectResolveAsset, AHitTouchesNothingAndAMissImportsOnlyThatPath) {
    AssetFilesProject& h = assetFilesProject();
    ASSERT_TRUE(h.ready);
    const fs::path assets = h.project->getAssetsPath();
    std::ofstream(assets / "known.txt") << "known\n";
    h.project->refreshAssetDatabase(true);
    ASSERT_TRUE(h.project->getAssetDatabase().lookupPath("assets/known.txt").found());

    // written since, by another program: no frame has looked
    std::ofstream(assets / "other.txt") << "other\n";
    std::ofstream(assets / "asked.txt") << "asked\n";
    EXPECT_TRUE(h.project->resolveAsset("assets/known.txt", "", AssetType::Unknown).found());
    EXPECT_FALSE(h.project->getAssetDatabase().lookupPath("assets/other.txt").found()) << "a hit walks no disk";

    EXPECT_TRUE(h.project->resolveAsset("assets/asked.txt", "", AssetType::Unknown).found()) << "a file on disk is not missing";
    EXPECT_FALSE(h.project->getAssetDatabase().lookupPath("assets/other.txt").found()) << "a miss imports the path asked for, alone";
    EXPECT_FALSE(h.project->resolveAsset("assets/absent.txt", "", AssetType::Unknown).found());
}
