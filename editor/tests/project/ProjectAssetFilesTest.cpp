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
#include "texture/TextureExport.h"

#include "support/PosixCompat.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
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

    [[maybe_unused]] std::string projectAssetFilesReadBytes(const fs::path& path) {
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

// S94 F50: a game cannot decode TIFF or PSB, so export cooks them; a reference that reads the
// source file itself (an HTML UI image) stops the export, naming the file and the reader
TEST(ProjectTextureSources, CooksTiffAndPsbAndRefusesTheirSourceReaders) {
    AssetFilesProject& h = assetFilesProject();
    ASSERT_TRUE(h.ready);
    const fs::path assets = h.project->getAssetsPath();
    {
        // a 1 x 1 grey baseline TIFF (little endian, one uncompressed strip)
        std::string tiff("II*\0\x08\0\0\0", 8);
        auto u16 = [&tiff](uint16_t v) { tiff += static_cast<char>(v & 0xff); tiff += static_cast<char>(v >> 8); };
        auto u32 = [&u16](uint32_t v) { u16(static_cast<uint16_t>(v & 0xffff)); u16(static_cast<uint16_t>(v >> 16)); };
        const uint16_t entries[][3] = {{256, 3, 1}, {257, 3, 1}, {258, 3, 8}, {259, 3, 1}, {262, 3, 1},
                                       {273, 4, 122}, {277, 3, 1}, {278, 3, 1}, {279, 4, 1}};
        u16(9);
        for (const auto& e : entries) {
            u16(e[0]);
            u16(e[1]);
            u32(1);
            u32(e[2]);
        }
        u32(0);
        tiff += static_cast<char>(200);
        std::ofstream(assets / "photo.tif", std::ios::binary) << tiff;
    }
    // what never read as a texture before TIFF support (a float bake, not a baseline TIFF) and
    // nothing references stays out of the export instead of failing it
    std::ofstream(assets / "bake.tif", std::ios::binary) << "tif";
    std::ofstream(assets / "art.psb", std::ios::binary) << "psb";
    h.project->refreshAssetDatabase(true);
    const std::vector<std::string> sources = projectTextureSources(*h.project, true);
    EXPECT_NE(std::find(sources.begin(), sources.end(), "photo.tif"), sources.end());
    EXPECT_NE(std::find(sources.begin(), sources.end(), "art.psb"), sources.end());
    std::map<std::string, TextureCookInputs> inputs;
    std::string error;
    h.project->getAssetIndex().waitUntilReady();
    ASSERT_TRUE(TextureExport::gather(*h.project, inputs, error)) << error;
    EXPECT_EQ(inputs.count("photo.tif"), 1u);
    EXPECT_EQ(inputs.count("bake.tif"), 0u);
    fs::remove(assets / "bake.tif");

    std::ofstream(assets / "menu.html") << "<html><body><img src=\"photo.tif\"/></body></html>\n";
    h.project->refreshAssetDatabase(true);
    h.project->getAssetIndex().waitUntilReady();
    EXPECT_FALSE(TextureExport::gather(*h.project, inputs, error));
    EXPECT_NE(error.find("photo.tif"), std::string::npos) << error;
    EXPECT_NE(error.find("menu.html"), std::string::npos) << "names the reader: " << error;
    fs::remove(assets / "menu.html");
    h.project->refreshAssetDatabase(true);
    h.project->getAssetIndex().waitUntilReady();
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

TEST(ProjectResolveAsset, AMissOutsideTheProjectImportsNothing) {
    AssetFilesProject& h = assetFilesProject();
    ASSERT_TRUE(h.ready);
    const fs::path outside = h.root / "outside.txt";
    std::ofstream(outside) << "not the project's\n";
    for (const std::string& asked : {std::string("../outside.txt"), std::string("assets/../../outside.txt"), outside.generic_string()}) {
        EXPECT_FALSE(h.project->resolveAsset(asked, "", AssetType::Unknown).found()) << asked;
        EXPECT_FALSE(h.project->importUnseenAsset(asked)) << asked;
    }
    std::error_code ec;
    EXPECT_FALSE(fs::exists(h.root / "outside.txt.meta", ec)) << "no identity is written next to a file outside the project";
}
