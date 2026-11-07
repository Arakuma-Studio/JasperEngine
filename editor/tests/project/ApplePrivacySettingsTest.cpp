// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Project Settings > Player > iOS / macOS > Privacy Manifest (mobile-shipping spec 6.3, 6.4): the
// project's additions in project.yaml (apple.privacy, short names, nothing when unset) and their
// undoable command.

#include "Project.h"
#include "Stream.h"
#include "Workspace.h"
#include "yaml-cpp/yaml.h"
#include "command/type/ApplePrivacySettingsCmd.h"
#include "command/CommandHistory.h"
#include "command/UndoOrigin.h"
#include "settings/SettingsRegistry.h"
#include "../command/UndoFakeCommand.h"

#include <gtest/gtest.h>

using namespace jasperengine::editor;

namespace apple_privacy_settings_test {

    ApplePrivacy::Manifest additions() {
        ApplePrivacy::Manifest m;
        m.tracking = true;
        m.trackingDomains = {"ads.example.com"};
        m.collectedData = {{"DeviceID", true, true, {"ThirdPartyAdvertising"}}};
        m.accessedApis = {{"DiskSpace", {"E174.1"}}};
        return m;
    }

}

using namespace apple_privacy_settings_test;

TEST(ApplePrivacySettings, RoundTripsThroughProjectYaml) {
    Project project;
    EXPECT_FALSE(Stream::encodeProject(&project)["apple"]) << "unset: not written";
    project.setApplePrivacy(additions());
    const YAML::Node saved = Stream::encodeProject(&project);
    ASSERT_TRUE(saved["apple"]["privacy"]);
    EXPECT_EQ(saved["apple"]["privacy"]["collectedData"][0]["type"].as<std::string>(), "DeviceID") << "short names";
    EXPECT_EQ(saved["apple"]["privacy"]["accessedApis"][0]["reasons"][0].as<std::string>(), "E174.1");

    Project loaded;
    Stream::decodeProject(&loaded, YAML::Load(YAML::Dump(saved)), Workspace());
    EXPECT_EQ(loaded.getApplePrivacy(), ApplePrivacy::normalized(additions()));

    // Apple's long names are read too
    Project longNames;
    Stream::decodeProject(&longNames, YAML::Load("name: p\napple:\n  privacy:\n    collectedData:\n"
                                                 "      - {type: NSPrivacyCollectedDataTypeDeviceID, linked: true, tracking: true, "
                                                 "purposes: [NSPrivacyCollectedDataTypePurposeThirdPartyAdvertising]}\n"), Workspace());
    ASSERT_EQ(longNames.getApplePrivacy().collectedData.size(), 1u);
    EXPECT_EQ(longNames.getApplePrivacy().collectedData[0].type, "DeviceID");
    EXPECT_EQ(longNames.getApplePrivacy().collectedData[0].purposes, std::vector<std::string>{"ThirdPartyAdvertising"});
}

TEST(ApplePrivacySettings, CommandAppliesAndUndoes) {
    Project project;
    ApplePrivacySettingsCmd command(&project, additions());
    ASSERT_TRUE(command.execute());
    EXPECT_EQ(project.getApplePrivacy(), ApplePrivacy::normalized(additions()));
    command.undo();
    EXPECT_TRUE(project.getApplePrivacy().empty());
    EXPECT_EQ(command.describe(), "Change Privacy Manifest");
}

// AC7 (red before the review fix: one undo step per key): the apple.privacy_* keys of one agent
// call are one step; a user's edits stay apart
TEST(ApplePrivacySettings, OneAgentCallIsOneUndoStep) {
    jasperengine::editor::test::UndoStatics statics;
    Project project;
    SettingsContext ctx;
    ctx.projectPtr = &project;
    const SettingsRegistry& registry = SettingsRegistry::get();
    auto set = [&](const char* key, const jasperengine::editor::mcp::Json& value) {
        const SettingRow* row = registry.find(key);
        ASSERT_NE(row, nullptr) << key;
        std::string error;
        ASSERT_TRUE(row->set(ctx, value, true, error)) << key << ": " << error;
    };
    CommandHistory* history = project.getProjectCommandHistory();
    UndoOrigin agent;
    agent.kind = UndoOriginKind::Agent;
    agent.call = 41;
    {
        UndoOriginScope scope(agent);
        set("apple.privacy_tracking", true);
        set("apple.privacy_tracking_domains", jasperengine::editor::mcp::Json::array({"ads.example.com"}));
        set("apple.privacy_accessed_apis", jasperengine::editor::mcp::Json::parse(R"([{"type": "DiskSpace", "reasons": ["E174.1"]}])"));
    }
    EXPECT_EQ(history->entries().size(), 1u);
    ASSERT_TRUE(history->undo());
    EXPECT_TRUE(project.getApplePrivacy().empty()) << "one undo takes the whole call back";

    set("apple.privacy_tracking", true);
    set("apple.privacy_tracking_domains", jasperengine::editor::mcp::Json::array({"ads.example.com"}));
    EXPECT_EQ(history->entries().size(), 2u) << "a user's edits are separate steps";

    // the macOS page lists the same rows in search
    const SettingRow* mac = registry.find("macos.privacy_tracking_domains");
    ASSERT_NE(mac, nullptr);
    EXPECT_EQ(mac->section, "macOS");
    EXPECT_EQ(mac->get(ctx), jasperengine::editor::mcp::Json::array({"ads.example.com"}));
}

