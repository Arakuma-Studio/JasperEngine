// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Project.h"
#include "Stream.h"
#include "Workspace.h"

#include <gtest/gtest.h>

using namespace jasperengine::editor;

// project.yaml "playerCrash" (Project Settings > Player > Crash Handling)
TEST(StreamProjectCrash, MissingKeyGivesTheDefaults) {
    Project project;
    PlayerCrashSettings changed;
    changed.enabled = false;
    project.setPlayerCrash(changed);
    YAML::Node node = YAML::Load("vsync: true\n");
    Stream::decodeProject(&project, node, Workspace());
    EXPECT_TRUE(project.getPlayerCrash() == PlayerCrashSettings()) << "an old project loads with the defaults";
    EXPECT_FALSE(Stream::encodeProject(&project)["playerCrash"].IsDefined()) << "the defaults are not written";
}

TEST(StreamProjectCrash, RoundTrips) {
    Project project;
    PlayerCrashSettings crash;
    crash.enabled = false;
    crash.includeLogTail = false;
    crash.dumpWithHeap = true;
    crash.hangWatchdog = false;
    crash.hangSeconds = 12;
    crash.webCrashOverlay = false;
    project.setPlayerCrash(crash);
    YAML::Node encoded = YAML::Load(YAML::Dump(Stream::encodeProject(&project)));
    EXPECT_EQ(encoded["playerCrash"]["dumpType"].as<std::string>(), "with_heap");
    Project loaded;
    Stream::decodeProject(&loaded, encoded, Workspace());
    EXPECT_TRUE(loaded.getPlayerCrash() == crash);
}

TEST(StreamProjectCrash, HangSecondsAreClamped) {
    Project project;
    YAML::Node node = YAML::Load("playerCrash:\n  hangSeconds: 900\n  dumpType: huge\n");
    Stream::decodeProject(&project, node, Workspace());
    EXPECT_EQ(project.getPlayerCrash().hangSeconds, PlayerCrashSettings::maxHangSeconds);
    EXPECT_FALSE(project.getPlayerCrash().dumpWithHeap) << "an unknown dump type is Small";
}
