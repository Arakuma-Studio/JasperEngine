// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Apple privacy manifest of every iOS and macOS build (mobile-shipping spec 6.3): the engine's
// declarations, the project's additions and the static plugins' manifests, composed into one
// deterministic PrivacyInfo.xcprivacy.

#include "support/TestTempDir.h"

#include "util/ApplePrivacy.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;

namespace apple_privacy_test {

    std::string readFile(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    const ApplePrivacy::AccessedApi* api(const ApplePrivacy::Manifest& m, const std::string& type) {
        for (const ApplePrivacy::AccessedApi& a : m.accessedApis) {
            if (a.type == type) return &a;
        }
        return nullptr;
    }

    // a static framework (its binary an ar archive) or a dynamic one, with a manifest inside
    void writeFramework(const fs::path& framework, bool isStatic, const std::string& manifest) {
        const std::string name = framework.stem().string();
        TestTempDir::write(framework / name, isStatic ? std::string("!<arch>\n") + std::string(64, 'x') : std::string("\xCF\xFA\xED\xFE") + std::string(64, 'x'));
        TestTempDir::write(framework / "PrivacyInfo.xcprivacy", manifest);
    }

}

using namespace apple_privacy_test;

TEST(ApplePrivacy, EngineManifestDeclaresTheEngineApis) {
    const ApplePrivacy::Manifest& engine = ApplePrivacy::engineManifest();
    ASSERT_NE(api(engine, "UserDefaults"), nullptr);
    EXPECT_EQ(api(engine, "UserDefaults")->reasons, std::vector<std::string>{"CA92.1"});
    ASSERT_NE(api(engine, "FileTimestamp"), nullptr);
    EXPECT_EQ(api(engine, "FileTimestamp")->reasons, std::vector<std::string>{"C617.1"});
    ASSERT_NE(api(engine, "SystemBootTime"), nullptr);
    EXPECT_EQ(api(engine, "SystemBootTime")->reasons, std::vector<std::string>{"35F9.1"});
    EXPECT_FALSE(engine.tracking);
    EXPECT_TRUE(engine.collectedData.empty());
    EXPECT_TRUE(ApplePrivacy::validate(engine).empty());

    ApplePrivacy::Manifest back;
    std::vector<std::string> warnings;
    std::string error;
    ASSERT_TRUE(ApplePrivacy::parse(ApplePrivacy::toXml(engine), back, warnings, error)) << error;
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(back, ApplePrivacy::normalized(engine));
}

// red before mobile-shipping: no PrivacyInfo.xcprivacy anywhere in the engine's templates
TEST(ApplePrivacy, CommittedManifestsMatchTheEngine) {
    const fs::path repo = JASPER_TEST_REPO_DIR;
    const std::string expected = ApplePrivacy::toXml(ApplePrivacy::engineManifest());
    for (const char* file : {"engine/workspaces/xcode/ios/PrivacyInfo.xcprivacy", "engine/workspaces/xcode/macos/PrivacyInfo.xcprivacy",
                             "engine/platform/apple/ios/player-template/PrivacyInfo.xcprivacy"}) {
        EXPECT_EQ(readFile(repo / file), expected) << file;
    }
}

TEST(ApplePrivacy, ProjectAndPluginLayersMerge) {
    TestTempDir dir("apple-privacy");
    const std::string adsManifest = ApplePrivacy::toXml([] {
        ApplePrivacy::Manifest m;
        m.tracking = true;
        m.trackingDomains = {"track.example.com"};
        m.collectedData = {{"DeviceID", true, true, {"ThirdPartyAdvertising"}}};
        m.accessedApis = {{"UserDefaults", {"C56D.1"}}};
        return m;
    }());
    TestTempDir::write(dir / "plugins/native/ios/Ads/PrivacyInfo.xcprivacy", adsManifest);
    writeFramework(dir / "plugins/native/ios/sdk/Static.framework", true,
                   ApplePrivacy::toXml(ApplePrivacy::Manifest{false, {}, {}, {{"DiskSpace", {"E174.1"}}}}));
    writeFramework(dir / "plugins/native/ios/sdk/Dynamic.framework", false,
                   ApplePrivacy::toXml(ApplePrivacy::Manifest{false, {}, {}, {{"ActiveKeyboards", {"54BD.1"}}}}));
    TestTempDir::write(dir / "plugins/native/apple/Broken/PrivacyInfo.xcprivacy", "<plist><nope");

    ApplePrivacy::Manifest project;
    project.trackingDomains = {"ads.example.com"};
    project.collectedData = {{"DeviceID", false, false, {"Analytics"}}};

    const ApplePrivacy::Composed ios = ApplePrivacy::compose(dir.path(), project, "ios", true);
    EXPECT_TRUE(ios.manifest.tracking) << "a plugin tracks";
    EXPECT_EQ(ios.manifest.trackingDomains, (std::vector<std::string>{"ads.example.com", "track.example.com"}));
    ASSERT_EQ(ios.manifest.collectedData.size(), 1u);
    EXPECT_TRUE(ios.manifest.collectedData[0].linked);
    EXPECT_TRUE(ios.manifest.collectedData[0].tracking);
    EXPECT_EQ(ios.manifest.collectedData[0].purposes, (std::vector<std::string>{"Analytics", "ThirdPartyAdvertising"}));
    ASSERT_NE(api(ios.manifest, "UserDefaults"), nullptr);
    EXPECT_EQ(api(ios.manifest, "UserDefaults")->reasons, (std::vector<std::string>{"C56D.1", "CA92.1"}));
    EXPECT_NE(api(ios.manifest, "DiskSpace"), nullptr) << "a static framework's manifest is the app's";
    EXPECT_EQ(api(ios.manifest, "ActiveKeyboards"), nullptr) << "a dynamic framework keeps its own";
    EXPECT_EQ(ios.pluginManifests, (std::vector<std::string>{"plugins/native/ios/Ads/PrivacyInfo.xcprivacy",
                                                             "plugins/native/ios/sdk/Static.framework/PrivacyInfo.xcprivacy"}));
    ASSERT_EQ(ios.warnings.size(), 1u);
    EXPECT_NE(ios.warnings[0].find("plugins/native/apple/Broken/PrivacyInfo.xcprivacy does not parse"), std::string::npos) << ios.warnings[0];
    bool fromPlugin = false;
    for (const ApplePrivacy::Source& source : ios.sources) {
        if (source.what == "domain track.example.com") fromPlugin = source.origin == "plugins/native/ios/Ads/PrivacyInfo.xcprivacy";
    }
    EXPECT_TRUE(fromPlugin);

    // iOS's plugins are not macOS's
    const ApplePrivacy::Composed mac = ApplePrivacy::compose(dir.path(), project, "macos");
    EXPECT_FALSE(mac.manifest.tracking);
    EXPECT_EQ(api(mac.manifest, "DiskSpace"), nullptr);
    EXPECT_EQ(mac.manifest.trackingDomains, std::vector<std::string>{"ads.example.com"});
}

// red before the review fix: a static SDK's resource bundle (Foo_Privacy.bundle) was carried
// without its manifest's reasons
TEST(ApplePrivacy, ResourceBundleManifestsMerge) {
    TestTempDir dir("apple-privacy");
    TestTempDir::write(dir / "plugins/native/ios/sdk/Ads_Privacy.bundle/PrivacyInfo.xcprivacy",
                       ApplePrivacy::toXml(ApplePrivacy::Manifest{false, {}, {}, {{"DiskSpace", {"E174.1"}}}}));
    const ApplePrivacy::Composed ios = ApplePrivacy::compose(dir.path(), {}, "ios");
    EXPECT_NE(api(ios.manifest, "DiskSpace"), nullptr) << "the bundle's manifest is the app's";
    EXPECT_EQ(ios.pluginManifests, std::vector<std::string>{"plugins/native/ios/sdk/Ads_Privacy.bundle/PrivacyInfo.xcprivacy"});
    EXPECT_EQ(api(ApplePrivacy::compose(dir.path(), {}, "macos").manifest, "DiskSpace"), nullptr);
}

// red before the review fix: only ios.plist_entries counted
TEST(ApplePrivacy, TrackingUsageFromEntriesOrRawXml) {
    EXPECT_TRUE(ApplePrivacy::declaresTrackingUsage({"NSUserTrackingUsageDescription"}, ""));
    EXPECT_TRUE(ApplePrivacy::declaresTrackingUsage({}, "<key>NSUserTrackingUsageDescription</key>\n<string>Ads</string>"));
    EXPECT_TRUE(ApplePrivacy::declaresTrackingUsage({}, "<key> NSUserTrackingUsageDescription </key><string>Ads</string>"));
    EXPECT_FALSE(ApplePrivacy::declaresTrackingUsage({"NSCameraUsageDescription"}, "<key>NSCameraUsageDescription</key><string>x</string>"));
}

TEST(ApplePrivacy, TrackingWarnings) {
    TestTempDir dir("apple-privacy");
    ApplePrivacy::Manifest project;
    project.tracking = true;
    const ApplePrivacy::Composed ios = ApplePrivacy::compose(dir.path(), project, "ios", false);
    ASSERT_EQ(ios.warnings.size(), 2u);
    EXPECT_EQ(ios.warnings[0].rfind("W-PRIVACY-DOMAINS: ", 0), 0u) << ios.warnings[0];
    EXPECT_EQ(ios.warnings[1].rfind("W-PRIVACY-ATT: ", 0), 0u) << ios.warnings[1];
    EXPECT_EQ(ApplePrivacy::compose(dir.path(), project, "macos", false).warnings.size(), 1u) << "no App Tracking Transparency on macOS";
}

TEST(ApplePrivacy, ValidateRefusesWrongReasonsAndTypes) {
    ApplePrivacy::Manifest m;
    m.accessedApis = {{"FileTimestamp", {"CA92.1"}}};
    EXPECT_NE(ApplePrivacy::validate(m).find("reason \"CA92.1\" is not one of FileTimestamp's"), std::string::npos) << ApplePrivacy::validate(m);
    m.accessedApis = {{"NSPrivacyAccessedAPICategoryFileTimestamp", {"C617.1"}}};
    EXPECT_TRUE(ApplePrivacy::validate(m).empty()) << "Apple's long names too";
    m.collectedData = {{"DeviceId", false, false, {"Analytics"}}};
    EXPECT_NE(ApplePrivacy::validate(m).find("data type \"DeviceId\""), std::string::npos);
    m.collectedData = {{"DeviceID", false, false, {"Ads"}}};
    EXPECT_NE(ApplePrivacy::validate(m).find("purpose \"Ads\""), std::string::npos);
    m.collectedData = {{"DeviceID", false, false, {}}};
    EXPECT_NE(ApplePrivacy::validate(m).find("needs at least one purpose"), std::string::npos);
    m.collectedData = {};
    m.trackingDomains = {"https://ads.example.com"};
    EXPECT_NE(ApplePrivacy::validate(m).find("is not a host name"), std::string::npos);
}

TEST(ApplePrivacy, ToXmlIsDeterministic) {
    ApplePrivacy::Manifest a;
    a.trackingDomains = {"b.example.com", "a.example.com"};
    a.accessedApis = {{"UserDefaults", {"CA92.1"}}, {"DiskSpace", {"E174.1", "85F4.1"}}};
    ApplePrivacy::Manifest b;
    b.trackingDomains = {"a.example.com", "b.example.com"};
    b.accessedApis = {{"DiskSpace", {"85F4.1", "E174.1"}}, {"UserDefaults", {"CA92.1"}}};
    EXPECT_EQ(ApplePrivacy::toXml(a), ApplePrivacy::toXml(b));
    const std::string xml = ApplePrivacy::toXml(a);
    EXPECT_LT(xml.find("NSPrivacyAccessedAPICategoryDiskSpace"), xml.find("NSPrivacyAccessedAPICategoryUserDefaults"));
    EXPECT_EQ(xml.back(), '\n');
}
