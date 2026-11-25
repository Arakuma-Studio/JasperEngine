// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// project.yaml's dynamicBatching (Project Settings > Graphics > Dynamic Batching, sprite-batching
// spec): on when a project saved before it loads, written only when off, round-trips, applies to
// the engine at once (Engine.dynamicBatching starts from it and goes back to it with the
// application state) and reaches both kinds of export: the generated main.cpp (only when off) and
// the prebuilt player's game.json.

#include "Engine.h"
#include "Generator.h"
#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "player/PlayerConfig.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::player::PlayerConfig;

TEST(StreamProjectDynamicBatching, OldProjectsBatch){
    Project project;
    project.setDynamicBatching(false);
    // a project.yaml from before Dynamic Batching
    Stream::decodeProject(&project, YAML::Load("vsync: true\n"), Workspace());
    EXPECT_TRUE(project.isDynamicBatching());
    EXPECT_TRUE(Engine::isDynamicBatching());
    EXPECT_FALSE(Stream::encodeProject(&project)["dynamicBatching"].IsDefined()) << "the default is not written";
}

TEST(StreamProjectDynamicBatching, RoundTripsAndApplies){
    Project project;
    project.setDynamicBatching(false);
    EXPECT_FALSE(Engine::isDynamicBatching());
    const YAML::Node encoded = YAML::Load(YAML::Dump(Stream::encodeProject(&project)));
    EXPECT_FALSE(encoded["dynamicBatching"].as<bool>());
    Project loaded;
    Stream::decodeProject(&loaded, encoded, Workspace());
    EXPECT_FALSE(loaded.isDynamicBatching());

    // a game's Engine.dynamicBatching = true lasts until the application state is reset (Play stops)
    Engine::setDynamicBatching(true);
    Engine::resetApplicationState();
    EXPECT_FALSE(Engine::isDynamicBatching());
    loaded.setDynamicBatching(true);
    EXPECT_TRUE(Engine::isDynamicBatching());
}

TEST(StreamProjectDynamicBatching, ExportsCarryIt){
    PlayerConfig config;
    config.dynamicBatching = false;
    PlayerConfig read;
    std::string error;
    ASSERT_TRUE(PlayerConfig::fromJson(config.toJson(), read, error)) << error;
    EXPECT_FALSE(read.dynamicBatching);
    PlayerConfig older;
    nlohmann::json json = config.toJson();
    json.erase("dynamicBatching");
    ASSERT_TRUE(PlayerConfig::fromJson(json, older, error)) << error;
    EXPECT_TRUE(older.dynamicBatching) << "a game.json from before it batches";
}
