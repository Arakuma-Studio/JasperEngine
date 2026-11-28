// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S95 REQ-12, REQ-13: Project Settings > Time (time.fixed_timestep, time.maximum_delta_time) and
// Input > Simulate Touch From Mouse (input.simulate_touch_from_mouse) as rows, in project.yaml
// (omitted at their defaults, so old projects load as before) and in every build's game.json, which
// player::runGame applies before the first scene.

#include "cook/GameConfig.h"
#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "player/PlayerConfig.h"
#include "settings/SettingsRegistry.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(TimeAndTouchSettings, RowsValidateAndSet){
    Project project;
    SettingsContext ctx;
    ctx.projectPtr = &project;
    std::string error;

    const SettingRow* fixed = SettingsRegistry::get().find("time.fixed_timestep");
    const SettingRow* maximum = SettingsRegistry::get().find("time.maximum_delta_time");
    const SettingRow* touch = SettingsRegistry::get().find("input.simulate_touch_from_mouse");
    ASSERT_NE(fixed, nullptr);
    ASSERT_NE(maximum, nullptr);
    ASSERT_NE(touch, nullptr);

    EXPECT_EQ(fixed->get(ctx), mcp::Json(1.0 / 60.0));
    EXPECT_EQ(maximum->get(ctx), mcp::Json(0.25));
    EXPECT_EQ(touch->get(ctx), mcp::Json(false));

    ASSERT_TRUE(fixed->set(ctx, mcp::Json(0.02), true, error)) << error;
    ASSERT_TRUE(maximum->set(ctx, mcp::Json(0.1), true, error)) << error;
    ASSERT_TRUE(touch->set(ctx, mcp::Json(true), true, error)) << error;
    EXPECT_EQ(project.getFixedTimestep(), 0.02);
    EXPECT_EQ(project.getMaximumDeltaTime(), 0.1);
    EXPECT_TRUE(project.isSimulateTouchFromMouse());

    EXPECT_FALSE(fixed->set(ctx, mcp::Json(0), true, error)) << "a zero step";
    EXPECT_FALSE(fixed->set(ctx, mcp::Json("fast"), true, error));
    EXPECT_FALSE(maximum->set(ctx, mcp::Json(-1), true, error));
    EXPECT_EQ(project.getFixedTimestep(), 0.02) << "a refused value changes nothing";
}

TEST(TimeAndTouchSettings, RoundTripAndOldProjectsKeepTheDefaults){
    Project project;
    const YAML::Node defaults = Stream::encodeProject(&project);
    EXPECT_FALSE(defaults["time"]) << "a default writes nothing: old files stay as they were";
    EXPECT_FALSE(defaults["simulateTouchFromMouse"]);

    project.setFixedTimestep(1.0 / 120.0);
    project.setMaximumDeltaTime(0.1);
    project.setSimulateTouchFromMouse(true);
    const std::string yaml = YAML::Dump(Stream::encodeProject(&project));

    Project loaded;
    Stream::decodeProject(&loaded, YAML::Load(yaml), Workspace());
    EXPECT_EQ(loaded.getFixedTimestep(), 1.0 / 120.0) << yaml;
    EXPECT_EQ(loaded.getMaximumDeltaTime(), 0.1);
    EXPECT_TRUE(loaded.isSimulateTouchFromMouse());

    Project old;
    Stream::decodeProject(&old, YAML::Load("name: Old\n"), Workspace());
    EXPECT_EQ(old.getFixedTimestep(), Project::defaultFixedTimestep);
    EXPECT_EQ(old.getMaximumDeltaTime(), Project::defaultMaximumDeltaTime);
    EXPECT_FALSE(old.isSimulateTouchFromMouse());

    // a hand-edited step the engine refuses: the default
    Project bad;
    Stream::decodeProject(&bad, YAML::Load("name: Bad\ntime:\n  fixedTimestep: 0\n"), Workspace());
    EXPECT_EQ(bad.getFixedTimestep(), Project::defaultFixedTimestep);
}

TEST(TimeAndTouchSettings, ReachTheGames){
    // a prebuilt player reads them from game.json
    player::PlayerConfig game;
    EXPECT_FALSE(game.toJson().contains("time")) << "a game at the defaults keeps its file";
    game.fixedTimestep = 1.0 / 50.0;
    game.maximumDeltaTime = 0.1;
    game.simulateTouchFromMouse = true;
    player::PlayerConfig read;
    std::string error;
    ASSERT_TRUE(player::PlayerConfig::fromJson(game.toJson(), read, error)) << error;
    EXPECT_EQ(read.fixedTimestep, 1.0 / 50.0);
    EXPECT_EQ(read.maximumDeltaTime, 0.1);
    EXPECT_TRUE(read.simulateTouchFromMouse);
    ASSERT_TRUE(player::PlayerConfig::fromJson(player::PlayerConfig().toJson(), read, error)) << error;
    EXPECT_EQ(read.fixedTimestep, 1.0 / 60.0);
    EXPECT_FALSE(read.simulateTouchFromMouse);

    // every build's game.json carries them (S98: a Compile build's runGame reads it too and
    // applies them before the first scene); at the defaults it writes nothing
    Project project;
    editor::cook::GameConfigTarget target;
    target.fallbackName = "game";
    EXPECT_FALSE(editor::cook::makeGameConfig(project, target).toJson().contains("time"));
    project.setFixedTimestep(1.0 / 120.0);
    project.setMaximumDeltaTime(0.1);
    project.setSimulateTouchFromMouse(true);
    const player::PlayerConfig built = editor::cook::makeGameConfig(project, target);
    ASSERT_TRUE(player::PlayerConfig::fromJson(built.toJson(), read, error)) << error;
    EXPECT_EQ(read.fixedTimestep, 1.0 / 120.0) << "every digit of the double";
    EXPECT_EQ(read.maximumDeltaTime, 0.1);
    EXPECT_TRUE(read.simulateTouchFromMouse);
}
