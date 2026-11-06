// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Paths in project files use '/' on every platform, and a '\' in a stored path is a separator
// (Unity's AssetDatabase rule): a project saved on Windows opens on Linux and macOS. The paths
// below hold '\' as a Windows picker or an agent gives them.

#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "command/CommandHandle.h"
#include "texture/Material.h"

#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;
namespace fs = std::filesystem;

namespace {

    std::string readText(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

}

TEST(PortablePaths, ProjectYamlStoresForwardSlashes) {
    Project project;
    project.setWindowIcon(fs::path("icons\\app.png"));
    project.setLuaDir(fs::path("scripts\\lua"));
    project.setAssetsDir(fs::path("content\\assets"));
    AndroidProjectSettings& android = project.getAndroidProjectSettings();
    android.applicationName = "Game";
    android.launcherIcon = fs::path("icons\\launcher.png");
    android.adaptiveIconForeground = fs::path("icons\\front.png");
    android.adaptiveIconBackground = fs::path("icons\\back.png");
    android.keystorePath = fs::path("keys\\release.jks");
    android.keystoreAlias = "release";

    const YAML::Node root = Stream::encodeProject(&project);
    EXPECT_EQ(root["windowIcon"].as<std::string>(), "icons/app.png");
    EXPECT_EQ(root["luaDir"].as<std::string>(), "scripts/lua");
    EXPECT_EQ(root["assetsDir"].as<std::string>(), "content/assets");
    EXPECT_EQ(root["android"]["launcherIcon"].as<std::string>(), "icons/launcher.png");
    EXPECT_EQ(root["android"]["adaptiveIconForeground"].as<std::string>(), "icons/front.png");
    EXPECT_EQ(root["android"]["adaptiveIconBackground"].as<std::string>(), "icons/back.png");
    EXPECT_EQ(root["android"]["keystore"]["path"].as<std::string>(), "keys/release.jks");
}

TEST(PortablePaths, BackslashPathsLoad) {
    // project.yaml written by a Windows editor before paths were made portable
    const YAML::Node yaml = YAML::Load(
        "windowIcon: icons\\app.png\n"
        "luaDir: scripts\\lua\n"
        "android:\n"
        "  launcherIcon: icons\\launcher.png\n"
        "  keystore:\n"
        "    path: keys\\release.jks\n"
        "    alias: release\n");
    Project project;
    Stream::decodeProject(&project, yaml, Workspace());
    EXPECT_EQ(project.getWindowIcon().generic_string(), "icons/app.png");
    EXPECT_EQ(project.getLuaDir().generic_string(), "scripts/lua");
    EXPECT_EQ(project.getAndroidProjectSettings().launcherIcon.generic_string(), "icons/launcher.png");
    EXPECT_EQ(project.getAndroidProjectSettings().keystorePath.generic_string(), "keys/release.jks");

    // a texture, a cube map's faces and asset references in a scene or material
    const Texture texture = Stream::decodeTexture(YAML::Load(
        "textureType: 2D\nsource: path\npath: textures\\wood.png\n"));
    EXPECT_EQ(texture.getPath(0), "textures/wood.png");
    const Texture single = Stream::decodeTexture(YAML::Load(
        "textureType: CUBE\ncubeMode: single\nsource: path\npath: sky\\cube.png\n"));
    EXPECT_EQ(single.getPath(0), "sky/cube.png");
    const Texture faces = Stream::decodeTexture(YAML::Load(
        "textureType: CUBE\ncubeMode: faces\nsource: paths\n"
        "paths: [sky\\px.png, sky\\nx.png, sky\\py.png, sky\\ny.png, sky\\pz.png, sky\\nz.png]\n"));
    EXPECT_EQ(faces.getPath(0), "sky/px.png");
    EXPECT_EQ(faces.getPath(5), "sky/nz.png");

    const YAML::Node refs = YAML::Load("script: scripts\\player.lua\nshader: shaders\\water\n");
    EXPECT_EQ(Stream::decodeAssetRef(refs, "script").path, "scripts/player.lua");
    EXPECT_EQ(Stream::decodeShaderRef(refs, "shader").path, "shaders/water");
}

TEST(PortablePaths, EncodersWriteForwardSlashes) {
    YAML::Node node;
    Stream::encodeAssetRef(node, "script", AssetRef("scripts\\player.lua"));
    Stream::encodeShaderRef(node, "shader", ShaderRef("shaders\\water"));
    EXPECT_EQ(node["script"].as<std::string>(), "scripts/player.lua");
    EXPECT_EQ(node["shader"].as<std::string>(), "shaders/water");

    Texture faces;
    for (size_t f = 0; f < 6; f++) faces.setCubePath(f, "sky\\face" + std::to_string(f) + ".png");
    const YAML::Node encoded = Stream::encodeTexture(faces);
    ASSERT_TRUE(encoded["paths"].IsSequence());
    EXPECT_EQ(encoded["paths"][0].as<std::string>(), "sky/face0.png");
    EXPECT_EQ(encoded["paths"][5].as<std::string>(), "sky/face5.png");
}

TEST(PortablePaths, MaterialFileStoresPicksWithSlashes) {
    TestTempDir dir("portable-material");
    Project project;
    project.setSkipProjectLock(true);
    std::string error;
    ASSERT_TRUE(project.createProjectAt(dir.path() / "Game", "Game", &error)) << error;
    const fs::path assets = project.getAssetsPath();
    fs::create_directories(assets / "textures");
    std::ofstream(assets / "textures" / "wood.png", std::ios::binary) << "not decoded here";

    // a texture picked in the Inspector on Windows: textures\wood.png
    Material material;
    material.baseColorTexture = Texture("textures\\wood.png");
    const fs::path file = fs::path("Wood.material");
    ASSERT_EQ(project.writeMaterialFile(file, material), SaveResult::Saved);

    const std::string text = readText(project.getProjectPath() / file);
    EXPECT_NE(text.find("textures/wood.png"), std::string::npos) << text;
    EXPECT_EQ(text.find('\\'), std::string::npos) << text;
    const YAML::Node texture = YAML::Load(text)["baseColorTexture"];
    ASSERT_TRUE(texture.IsMap()) << text;
    EXPECT_EQ(texture["path"].as<std::string>(), "textures/wood.png");
    EXPECT_TRUE(texture["guid"].IsDefined()) << "the reference was resolved to the file's GUID: " << text;

    for (SceneProject& scene : project.getScenes()) CommandHandle::remove(scene.id);
    project.releaseScenes();
}

// A sub-asset's key names a node of the model: a backslash in it is part of the name, only the
// container's path is made portable
TEST(PortablePaths, SubAssetKeysKeepTheirBackslashes) {
    EXPECT_EQ(Stream::portablePath(std::string("models\\hero.glb#mesh:Body\\Skin")), "models/hero.glb#mesh:Body\\Skin");
    EXPECT_EQ(Stream::portablePath(std::string("models\\hero.glb#material#2")), "models/hero.glb#material#2");
    // a '#' that is no sub-asset key is part of the file name
    EXPECT_EQ(Stream::portablePath(std::string("notes\\take#2.txt")), "notes/take#2.txt");

    YAML::Node node;
    Stream::encodeAssetRef(node, "mesh", AssetRef("models\\hero.glb#mesh:Body\\Skin"));
    EXPECT_EQ(node["mesh"].as<std::string>(), "models/hero.glb#mesh:Body\\Skin");
    EXPECT_EQ(Stream::decodeAssetRef(YAML::Load("mesh: models\\hero.glb#mesh:Body\\Skin\n"), "mesh").path,
              "models/hero.glb#mesh:Body\\Skin");
}
