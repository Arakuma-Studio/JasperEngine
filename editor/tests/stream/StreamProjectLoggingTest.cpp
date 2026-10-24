// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Project.h"
#include "Stream.h"
#include "Workspace.h"

#include <gtest/gtest.h>

using namespace jasperengine::editor;

// project.yaml "playerLogging" (Project Settings > Player > Logging)
TEST(StreamProjectLogging, MissingKeyGivesTheDefaults) {
    Project project;
    PlayerLoggingSettings changed;
    changed.level = "warn";
    project.setPlayerLogging(changed);
    YAML::Node node = YAML::Load("vsync: true\n");
    Stream::decodeProject(&project, node, Workspace());
    EXPECT_TRUE(project.getPlayerLogging() == PlayerLoggingSettings());
    EXPECT_FALSE(Stream::encodeProject(&project)["playerLogging"].IsDefined()) << "the defaults are not written";
}

TEST(StreamProjectLogging, RoundTrips) {
    Project project;
    PlayerLoggingSettings logging;
    logging.level = "error";
    logging.fileLog = true;
    logging.backtrace = true;
    project.setPlayerLogging(logging);
    YAML::Node encoded = YAML::Load(YAML::Dump(Stream::encodeProject(&project)));
    Project loaded;
    Stream::decodeProject(&loaded, encoded, Workspace());
    EXPECT_TRUE(loaded.getPlayerLogging() == logging);

    YAML::Node odd = YAML::Load("playerLogging:\n  level: loud\n");
    Stream::decodeProject(&loaded, odd, Workspace());
    EXPECT_EQ(loaded.getPlayerLogging().level, "info") << "an unknown level falls back to the default";
}
