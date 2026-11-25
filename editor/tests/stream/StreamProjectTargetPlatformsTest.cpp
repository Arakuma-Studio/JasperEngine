// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// project.yaml's export.targetPlatforms (Project Settings > Player > Publishing Settings > Target
// Platforms, docs/superpowers/specs/2026-11-12-target-platforms-design.md 5.1): all six when a project
// saved before it loads, written only when not all six, in BuildPlatform order; unknown names are
// dropped and an empty list targets all six, each with a load warning.

#include "Out.h"
#include "Project.h"
#include "Stream.h"
#include "Workspace.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {
    uint8_t streamProjectTargetPlatformsTargetBit(BuildPlatform platform) {
        return static_cast<uint8_t>(1u << static_cast<unsigned>(platform));
    }

    std::vector<std::string> encodedTargets(Project& project) {
        const YAML::Node encoded = YAML::Load(YAML::Dump(Stream::encodeProject(&project)));
        std::vector<std::string> names;
        if (encoded["export"] && encoded["export"]["targetPlatforms"]) {
            for (const YAML::Node& name : encoded["export"]["targetPlatforms"]) names.push_back(name.as<std::string>());
        }
        return names;
    }

    // the warnings the editor logged since `cursor` that contain `text`
    size_t warningsSince(uint64_t cursor, const std::string& text) {
        size_t count = 0;
        for (const editor::LogEntry& entry : Out::getLogsSince(cursor)) {
            if (entry.type == editor::LogType::Warning && entry.message.find(text) != std::string::npos) count++;
        }
        return count;
    }

    void streamProjectTargetPlatformsEnsureLogging() {
        if (!Out::memorySinkId()) Out::startLogging();
    }
}

TEST(StreamProjectTargetPlatforms, MissingKeyTargetsAll) {
    streamProjectTargetPlatformsEnsureLogging();
    Project project;
    project.setTargetPlatforms(streamProjectTargetPlatformsTargetBit(BuildPlatform::Android));
    // a project.yaml from before Target Platforms
    uint64_t cursor = Out::lastLogId();
    Stream::decodeProject(&project, YAML::Load("vsync: true\nexport:\n  method: prebuilt\n"), Workspace());
    EXPECT_EQ(project.getTargetPlatforms(), Project::allTargetPlatforms);
    EXPECT_EQ(warningsSince(cursor, "Target Platforms"), 0u) << "a missing key is no mistake";
    for (BuildPlatform platform : {BuildPlatform::Windows, BuildPlatform::Linux, BuildPlatform::MacOS, BuildPlatform::Android,
                                   BuildPlatform::IOS, BuildPlatform::Web}) {
        EXPECT_TRUE(project.isTargetPlatform(platform)) << buildPlatformName(platform);
    }
    EXPECT_TRUE(encodedTargets(project).empty()) << "all six are not written";
    const YAML::Node encoded = YAML::Load(YAML::Dump(Stream::encodeProject(&project)));
    EXPECT_FALSE(encoded["export"] && encoded["export"]["targetPlatforms"]);

    // without an export map at all, too (every project.yaml that never set an export option)
    project.setTargetPlatforms(streamProjectTargetPlatformsTargetBit(BuildPlatform::IOS));
    cursor = Out::lastLogId();
    Stream::decodeProject(&project, YAML::Load("vsync: true\n"), Workspace());
    EXPECT_EQ(project.getTargetPlatforms(), Project::allTargetPlatforms);
    EXPECT_EQ(warningsSince(cursor, "Target Platforms"), 0u) << "no export map is no mistake either";
}

TEST(StreamProjectTargetPlatforms, RoundTripsCanonical) {
    Project project;
    Stream::decodeProject(&project, YAML::Load("export:\n  targetPlatforms: [ios, android]\n"), Workspace());
    EXPECT_EQ(project.getTargetPlatforms(), streamProjectTargetPlatformsTargetBit(BuildPlatform::Android) | streamProjectTargetPlatformsTargetBit(BuildPlatform::IOS));
    EXPECT_TRUE(project.isTargetPlatform(BuildPlatform::Android));
    EXPECT_FALSE(project.isTargetPlatform(BuildPlatform::Linux));
    EXPECT_EQ(encodedTargets(project), (std::vector<std::string>{"android", "ios"})) << "BuildPlatform order";

    Project loaded;
    Stream::decodeProject(&loaded, YAML::Load(YAML::Dump(Stream::encodeProject(&project))), Workspace());
    EXPECT_EQ(loaded.getTargetPlatforms(), project.getTargetPlatforms());

    // parseBuildPlatform's other names of macOS
    Stream::decodeProject(&loaded, YAML::Load("export:\n  targetPlatforms: [osx, web]\n"), Workspace());
    EXPECT_EQ(encodedTargets(loaded), (std::vector<std::string>{"macos", "web"}));
}

TEST(StreamProjectTargetPlatforms, UnknownAndEmptyFallBack) {
    streamProjectTargetPlatformsEnsureLogging();
    Project project;
    uint64_t cursor = Out::lastLogId();
    Stream::decodeProject(&project, YAML::Load("export:\n  targetPlatforms: [android, playstation]\n"), Workspace());
    EXPECT_EQ(project.getTargetPlatforms(), streamProjectTargetPlatformsTargetBit(BuildPlatform::Android)) << "the unknown name is dropped";
    EXPECT_EQ(warningsSince(cursor, "playstation"), 1u) << "a load warning names it";

    cursor = Out::lastLogId();
    Stream::decodeProject(&project, YAML::Load("export:\n  targetPlatforms: []\n"), Workspace());
    EXPECT_EQ(project.getTargetPlatforms(), Project::allTargetPlatforms) << "an empty list targets all six";
    EXPECT_EQ(warningsSince(cursor, "targetPlatforms"), 1u);

    cursor = Out::lastLogId();
    Stream::decodeProject(&project, YAML::Load("export:\n  targetPlatforms: [playstation]\n"), Workspace());
    EXPECT_EQ(project.getTargetPlatforms(), Project::allTargetPlatforms) << "empty once the unknown name is dropped";
    EXPECT_EQ(warningsSince(cursor, "playstation"), 1u);
    EXPECT_EQ(warningsSince(cursor, "all six"), 1u);

    cursor = Out::lastLogId();
    Stream::decodeProject(&project, YAML::Load("export:\n  targetPlatforms: android\n"), Workspace());
    EXPECT_EQ(project.getTargetPlatforms(), Project::allTargetPlatforms) << "not a sequence";
    EXPECT_EQ(warningsSince(cursor, "targetPlatforms"), 1u);
}

TEST(ProjectTargetPlatforms, DefaultBuildPlatformIsTheHostOrTheFirstTargetInTheListOrder) {
    Project project;
    EXPECT_EQ(project.defaultBuildPlatform(), hostBuildPlatform()) << "every platform is a target";
    project.setTargetPlatforms(streamProjectTargetPlatformsTargetBit(BuildPlatform::IOS) | streamProjectTargetPlatformsTargetBit(BuildPlatform::Android));
    EXPECT_EQ(project.defaultBuildPlatform(), BuildPlatform::Android);
    project.setTargetPlatforms(streamProjectTargetPlatformsTargetBit(BuildPlatform::Web) | streamProjectTargetPlatformsTargetBit(BuildPlatform::IOS));
    EXPECT_EQ(project.defaultBuildPlatform(), BuildPlatform::IOS);
    // Build Settings lists macOS before Linux
    if (hostBuildPlatform() == BuildPlatform::Windows) {
        project.setTargetPlatforms(streamProjectTargetPlatformsTargetBit(BuildPlatform::Linux) | streamProjectTargetPlatformsTargetBit(BuildPlatform::MacOS));
        EXPECT_EQ(project.defaultBuildPlatform(), BuildPlatform::MacOS);
    } else {
        project.setTargetPlatforms(streamProjectTargetPlatformsTargetBit(BuildPlatform::Linux) | streamProjectTargetPlatformsTargetBit(BuildPlatform::MacOS) | streamProjectTargetPlatformsTargetBit(BuildPlatform::Windows));
        EXPECT_EQ(project.defaultBuildPlatform(), hostBuildPlatform());
        project.setTargetPlatforms(streamProjectTargetPlatformsTargetBit(BuildPlatform::Linux) | streamProjectTargetPlatformsTargetBit(BuildPlatform::MacOS) | streamProjectTargetPlatformsTargetBit(BuildPlatform::Web));
        project.setTargetPlatforms(project.getTargetPlatforms() & ~streamProjectTargetPlatformsTargetBit(hostBuildPlatform()));
        EXPECT_EQ(project.defaultBuildPlatform(), hostBuildPlatform() == BuildPlatform::Linux ? BuildPlatform::MacOS : BuildPlatform::Linux);
    }
    project.setTargetPlatforms(0);
    EXPECT_EQ(project.getTargetPlatforms(), Project::allTargetPlatforms) << "a project always has a target";
}
