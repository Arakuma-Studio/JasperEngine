// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Which method a build uses (docs/specs/S30-shipping.md R5, AC3): a prebuilt player unless the
// project needs native code, each reason named.

#include "support/TestTempDir.h"

#include "Exporter.h"
#include "Project.h"
#include "players/PlayerRuntimes.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;

namespace build_method_test {

    bool hasReason(const BuildMethodChoice& choice, const std::string& part) {
        return std::any_of(choice.reasons.begin(), choice.reasons.end(),
                           [&part](const std::string& reason) { return reason.find(part) != std::string::npos; });
    }

    SceneProject& addScene(Project& project, uint32_t id) {
        SceneProject scene;
        scene.id = id;
        scene.name = "Scene" + std::to_string(id);
        project.getScenes().push_back(scene);
        return project.getScenes().back();
    }

    ExportConfig linuxConfig() {
        ExportConfig config;
        config.mode = ExportMode::Player;
        config.platform = BuildPlatform::Linux;
        return config;
    }

}

using namespace build_method_test;

TEST(BuildMethod, APlainProjectUsesThePrebuiltPlayer) {
    Project project;
    addScene(project, 1);
    const BuildMethodChoice choice = Exporter::chooseBuildMethod(&project, linuxConfig(), nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Prebuilt);
    EXPECT_TRUE(choice.reasons.empty());
}

TEST(BuildMethod, CppScriptsCompile) {
    TestTempDir dir("build-method");
    TestTempDir::write(dir / "scripts/Player.cpp", "int x;\n");
    TestTempDir::write(dir / "scripts/Player.h", "#pragma once\n");
    Project project;
    addScene(project, 1);
    project.setScriptDirs({dir / "scripts"});
    const BuildMethodChoice choice = Exporter::chooseBuildMethod(&project, linuxConfig(), nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "C++ scripts (")) << (choice.reasons.empty() ? "" : choice.reasons.front());
    EXPECT_TRUE(hasReason(choice, "Player.cpp"));
    EXPECT_FALSE(hasReason(choice, "Player.h")) << "a header alone compiles nothing";
}

TEST(BuildMethod, CapacitiesAboveThePlayersCompile) {
    Project project;
    addScene(project, 1).maxValues.maxBones = 200;
    BuildMethodChoice choice = Exporter::chooseBuildMethod(&project, linuxConfig(), nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "MAX_BONES 200 (the player holds 128)")) << (choice.reasons.empty() ? "" : choice.reasons.front());

    // a player that holds more takes it
    PlayerInfo bigger;
    bigger.capacities["MAX_BONES"] = 256;
    choice = Exporter::chooseBuildMethod(&project, linuxConfig(), &bigger);
    EXPECT_EQ(choice.method, BuildMethod::Prebuilt);
}

TEST(BuildMethod, AdMobAndForcedCompile) {
    Project project;
    addScene(project, 1);
    project.getAndroidProjectSettings().admobEnabled = true;
    ExportConfig android = linuxConfig();
    android.platform = BuildPlatform::Android;
    BuildMethodChoice choice = Exporter::chooseBuildMethod(&project, android, nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "AdMob")) << (choice.reasons.empty() ? "" : choice.reasons.front());
    // AdMob is Android's: a Linux build does not need it
    EXPECT_EQ(Exporter::chooseBuildMethod(&project, linuxConfig(), nullptr).method, BuildMethod::Prebuilt);

    ExportConfig forced = linuxConfig();
    forced.method = BuildMethod::Compile;
    choice = Exporter::chooseBuildMethod(&project, forced, nullptr);
    EXPECT_EQ(choice.method, BuildMethod::Compile);
    EXPECT_TRUE(hasReason(choice, "Compile chosen"));
}

TEST(BuildMethod, PlayerIds) {
    ExportConfig config = linuxConfig();
    EXPECT_EQ(Exporter::playerIdFor(config, false), "linux-x64-glcore-release");
    config.graphicBackend = "vulkan";
    EXPECT_EQ(Exporter::playerIdFor(config, true), "linux-x64-vulkan-development");
    config.platform = BuildPlatform::Web;
    config.graphicBackend = "";
    EXPECT_EQ(Exporter::playerIdFor(config, false), "web-wasm32-gles3-release");
    config.platform = BuildPlatform::MacOS;
    EXPECT_EQ(Exporter::playerIdFor(config, false), "macos-universal-metal-release");
    config.platform = BuildPlatform::Android;
    EXPECT_EQ(Exporter::playerIdFor(config, false), "android-universal-gles3-release");
}
