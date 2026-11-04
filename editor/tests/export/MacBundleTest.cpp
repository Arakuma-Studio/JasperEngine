// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The prebuilt macOS bundle (mobile-shipping spec 6.6): the privacy manifest in
// Contents/Resources and the project's Info.plist entries, before the signature.

#include "support/TestTempDir.h"

#include "Exporter.h"
#include "platform/MacBundle.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;

namespace mac_bundle_test {

    std::string readFile(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

}

using namespace mac_bundle_test;

// red before mobile-shipping: the bundle had neither (the source export's Xcode app had the entries)
TEST(MacBundle, PrivacyManifestAndPlistEntries) {
    TestTempDir dir("mac-bundle");
    const fs::path bundle = dir / "Game.app";
    TestTempDir::write(bundle / "Contents/MacOS/jasperengine-player", "player");
    platform::MacBundleSettings settings;
    settings.bundleName = "Game";
    settings.executable = "Game";
    settings.bundleIdentifier = "com.example.game";
    settings.version = "1.0";
    settings.build = "1";
    settings.privacyManifest = "<plist version=\"1.0\"><dict/></plist>\n";
    NativePlistValue value;
    value.text = "Game Center friends";
    const std::map<std::string, NativePlistValue> entries = {{"NSGKFriendListUsageDescription", value}};
    settings.customizePlist = [&entries](std::string& plist) { Exporter::applyNativePlistEntries(plist, entries, ""); };
    std::vector<BuildWarning> warnings;
    std::string error;
    ASSERT_TRUE(platform::finishMacBundle(bundle, settings, warnings, error)) << error;
    EXPECT_EQ(readFile(bundle / "Contents/Resources/PrivacyInfo.xcprivacy"), settings.privacyManifest);
    const std::string plist = readFile(bundle / "Contents/Info.plist");
    EXPECT_NE(plist.find("<key>NSGKFriendListUsageDescription</key>\n\t<string>Game Center friends</string>"), std::string::npos) << plist;
    EXPECT_NE(plist.find("<key>CFBundleIdentifier</key>"), std::string::npos);
}
