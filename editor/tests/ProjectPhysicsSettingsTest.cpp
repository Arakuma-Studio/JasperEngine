// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// project.yaml keeps the 3D physics switch (`physics3D`, a bool) and the 3D world capacity
// (`physics3DWorld`, a map) under different keys: a project that turns 3D physics off and changes
// the capacity reads both back (they once shared `physics3D`, one overwriting the other).

#include <gtest/gtest.h>

#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "command/CommandHistory.h"
#include "player/PlayerConfig.h"
#include "registry/PhysicsLayers.h"
#include "settings/SettingsRegistry.h"

#include "yaml-cpp/yaml.h"

#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(ProjectPhysicsSettings, SwitchAndWorldCapacityRoundTripTogether) {
    Project project;
    project.setPhysics3DEnabled(!Project::defaultPhysics3DEnabled);
    Physics3DWorldSettings world;
    world.maxBodies = 1234;
    world.maxBodyPairs = 2345;
    world.maxContactConstraints = 3456;
    project.setPhysics3DWorldSettings(world);

    const YAML::Node node = Stream::encodeProject(&project);
    ASSERT_TRUE(node["physics3D"]);
    EXPECT_TRUE(node["physics3D"].IsScalar());
    ASSERT_TRUE(node["physics3DWorld"]);
    EXPECT_TRUE(node["physics3DWorld"].IsMap());
    const std::string yaml = YAML::Dump(node);

    Project loaded;
    Stream::decodeProject(&loaded, YAML::Load(yaml), Workspace());
    EXPECT_EQ(loaded.isPhysics3DEnabled(), !Project::defaultPhysics3DEnabled) << yaml;
    EXPECT_EQ(loaded.getPhysics3DWorldSettings().maxBodies, 1234u) << yaml;
    EXPECT_EQ(loaded.getPhysics3DWorldSettings().maxBodyPairs, 2345u);
    EXPECT_EQ(loaded.getPhysics3DWorldSettings().maxContactConstraints, 3456u);
}

TEST(ProjectPhysicsSettings, DefaultsWriteNeitherKey) {
    Project project;
    const YAML::Node node = Stream::encodeProject(&project);
    EXPECT_FALSE(node["physics3D"]);
    EXPECT_FALSE(node["physics3DWorld"]);
}

// spatial-queries AC16: Queries Hit Triggers, 3D and 2D: one undo step per toggle, project.yaml's
// physicsQueries only when one is off, an older project.yaml reads true, the engine follows (Play
// starts from the project's values: applyPhysicsQuerySettings, which Play start and Stop call)
TEST(ProjectPhysicsSettings, QueriesHitTriggersRoundTrip) {
    Project project;
    EXPECT_FALSE(Stream::encodeProject(&project)["physicsQueries"]) << "on (the default) writes nothing";
    SettingsContext ctx;
    ctx.projectPtr = &project;
    std::string error;
    const SettingRow* row3D = SettingsRegistry::get().find("physics.queries_hit_triggers");
    const SettingRow* row2D = SettingsRegistry::get().find("physics2d.queries_hit_triggers");
    ASSERT_NE(row3D, nullptr);
    ASSERT_NE(row2D, nullptr);
    CommandHistory* history = project.getProjectCommandHistory();
    ASSERT_FALSE(history->canUndo());

    ASSERT_TRUE(row3D->set(ctx, mcp::Json(false), true, error)) << error;
    EXPECT_FALSE(project.getPhysicsQuerySettings().hitTriggers3D);
    EXPECT_FALSE(Physics::getQueriesHitTriggers()) << "the engine follows the project";
    EXPECT_TRUE(Physics2D::getQueriesHitTriggers());
    ASSERT_TRUE(row2D->set(ctx, mcp::Json(false), true, error)) << error;
    EXPECT_FALSE(Physics2D::getQueriesHitTriggers());
    EXPECT_EQ(row3D->get(ctx), mcp::Json(false));

    const YAML::Node node = Stream::encodeProject(&project);
    ASSERT_TRUE(node["physicsQueries"]);
    EXPECT_FALSE(node["physicsQueries"]["hitTriggers3D"].as<bool>());
    EXPECT_FALSE(node["physicsQueries"]["hitTriggers2D"].as<bool>());
    const std::string yaml = YAML::Dump(node);

    // one undo step each
    ASSERT_TRUE(history->undo());
    EXPECT_TRUE(project.getPhysicsQuerySettings().hitTriggers2D);
    EXPECT_FALSE(project.getPhysicsQuerySettings().hitTriggers3D);
    EXPECT_TRUE(Physics2D::getQueriesHitTriggers());
    ASSERT_TRUE(history->undo());
    EXPECT_TRUE(project.getPhysicsQuerySettings().hitTriggers3D);
    EXPECT_TRUE(Physics::getQueriesHitTriggers());
    EXPECT_FALSE(history->canUndo());

    Project loaded;
    Stream::decodeProject(&loaded, YAML::Load(yaml), Workspace());
    EXPECT_FALSE(loaded.getPhysicsQuerySettings().hitTriggers3D) << yaml;
    EXPECT_FALSE(loaded.getPhysicsQuerySettings().hitTriggers2D);

    // a script turned it on in Play: Stop (and the next Play start) puts the project's back
    Physics::setQueriesHitTriggers(true);
    loaded.applyPhysicsQuerySettings();
    EXPECT_FALSE(Physics::getQueriesHitTriggers());

    // an older project.yaml without the key: on
    Project old;
    Stream::decodeProject(&old, YAML::Load("name: Old\n"), Workspace());
    EXPECT_EQ(old.getPhysicsQuerySettings(), PhysicsQuerySettings());
    EXPECT_TRUE(Physics::getQueriesHitTriggers());
    EXPECT_TRUE(Physics2D::getQueriesHitTriggers());
}

// AC17 (R33): game.json carries them only when off (every build reads it, player::applySettings);
// a game.json without them reads true
TEST(ProjectPhysicsSettings, GameJsonCarriesQueriesHitTriggers) {
    player::PlayerConfig game;
    std::string error;
    const auto defaults = game.toJson();
    EXPECT_FALSE(defaults.contains("queries_hit_triggers_3d"));
    EXPECT_FALSE(defaults.contains("queries_hit_triggers_2d"));
    player::PlayerConfig read;
    read.queriesHitTriggers3D = false;
    ASSERT_TRUE(player::PlayerConfig::fromJson(defaults, read, error)) << error;
    EXPECT_TRUE(read.queriesHitTriggers3D);
    EXPECT_TRUE(read.queriesHitTriggers2D);

    game.queriesHitTriggers3D = false;
    const auto off = game.toJson();
    EXPECT_EQ(off.value("queries_hit_triggers_3d", true), false);
    EXPECT_FALSE(off.contains("queries_hit_triggers_2d"));
    ASSERT_TRUE(player::PlayerConfig::fromJson(off, read, error)) << error;
    EXPECT_FALSE(read.queriesHitTriggers3D);
    EXPECT_TRUE(read.queriesHitTriggers2D);
}
