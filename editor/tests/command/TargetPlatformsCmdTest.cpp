// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Target Platforms (Project Settings > Player and the Build Settings window's Target column) is
// edited as one project undo step: ProjectSettingsValuesCmd on player.target_platforms, which
// saves project.yaml after each direction.

#include <gtest/gtest.h>

#include "Project.h"
#include "command/type/ProjectSettingsValuesCmd.h"

#include "TempDir.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {
    std::string targetPlatformsReadText(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // export.targetPlatforms as project.yaml holds it ("" when the key is absent)
    std::string savedTargets(const std::filesystem::path& root) {
        const YAML::Node yaml = YAML::Load(targetPlatformsReadText(root / "project.yaml"));
        if (!yaml["export"] || !yaml["export"]["targetPlatforms"]) return "";
        std::string names;
        for (const YAML::Node& name : yaml["export"]["targetPlatforms"]) names += (names.empty() ? "" : ",") + name.as<std::string>();
        return names;
    }

    uint8_t targetPlatformsCmdTargetBit(BuildPlatform platform) {
        return static_cast<uint8_t>(1u << static_cast<unsigned>(platform));
    }
}

TEST(TargetPlatformsCmd, UndoRedo) {
    jasperengine::test::TempDir dir;
    const std::filesystem::path root = dir.path();
    std::filesystem::create_directories(root / "assets");
    std::ofstream(root / "project.yaml") << "name: targets\nassetsDir: assets\n";

    Project project;
    project.setSkipProjectLock(true);
    ASSERT_TRUE(project.loadProject(root, false));
    ASSERT_EQ(project.getTargetPlatforms(), Project::allTargetPlatforms);

    auto* command = new ProjectSettingsValuesCmd(&project, {{"player.target_platforms", editor::mcp::Json::array({"ios", "android"})}},
                                                 "Set Target Platforms");
    command->setNoMerge();
    // the history owns the command from here (and frees one that fails)
    const bool added = project.getProjectCommandHistory()->addCommand(command);
    ASSERT_TRUE(added) << "player.target_platforms refused";
    EXPECT_EQ(project.getTargetPlatforms(), targetPlatformsCmdTargetBit(BuildPlatform::Android) | targetPlatformsCmdTargetBit(BuildPlatform::IOS));
    EXPECT_EQ(savedTargets(root), "android,ios") << "saved at once, in BuildPlatform order";

    ASSERT_TRUE(project.getProjectCommandHistory()->undo());
    EXPECT_EQ(project.getTargetPlatforms(), Project::allTargetPlatforms);
    EXPECT_EQ(savedTargets(root), "") << "all six again: the key is gone";

    ASSERT_TRUE(project.getProjectCommandHistory()->redo());
    EXPECT_EQ(project.getTargetPlatforms(), targetPlatformsCmdTargetBit(BuildPlatform::Android) | targetPlatformsCmdTargetBit(BuildPlatform::IOS));
    EXPECT_EQ(savedTargets(root), "android,ios");

    // the last target cannot go: refused, nothing changes
    ProjectSettingsValuesCmd empty(&project, {{"player.target_platforms", editor::mcp::Json::array()}}, "Set Target Platforms");
    EXPECT_FALSE(empty.execute());
    EXPECT_NE(empty.failure().find("non-empty list of windows, linux, macos, android, ios, web"), std::string::npos) << empty.failure();
    EXPECT_EQ(project.getTargetPlatforms(), targetPlatformsCmdTargetBit(BuildPlatform::Android) | targetPlatformsCmdTargetBit(BuildPlatform::IOS));
}
