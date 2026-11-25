// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The prebuilt iOS player's Info.plist (docs/specs/S98-editor-tooling-gaps.md REQ-20, AC18): the
// staged Xcode project lists the orientations of Player > Resolution and Presentation, as a source
// export does, read from the repository's template. Text only: nothing here needs a Mac.

#include "support/TestTempDir.h"

#include "Exporter.h"
#include "platform/IosProject.h"
#include "util/PlatformRules.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;

namespace ios_template_test {

    const fs::path repo = JASPER_TEST_REPO_DIR;
    const fs::path templateDir = repo / "engine" / "platform" / "apple" / "ios" / "player-template";

    // the <string> items of a top-level array key, in order; nothing when the key is absent
    std::vector<std::string> arrayOf(const std::string& plist, const std::string& key) {
        std::vector<std::string> items;
        const size_t at = plist.find("<key>" + key + "</key>");
        if (at == std::string::npos) return items;
        const size_t end = plist.find("</array>", at);
        for (size_t pos = plist.find("<string>", at); pos < end; pos = plist.find("<string>", pos)) {
            pos += 8;
            items.push_back(plist.substr(pos, plist.find("</string>", pos) - pos));
        }
        return items;
    }

    // the boolean value of a top-level key ("" when absent)
    std::string boolOf(const std::string& plist, const std::string& key) {
        const size_t at = plist.find("<key>" + key + "</key>");
        if (at == std::string::npos) return {};
        const size_t value = plist.find('<', at + key.size() + 11);
        return plist.substr(value, plist.find('>', value) - value + 1);
    }

    // a runtime's ios/ with the repository's template and a stand-in framework
    fs::path fakeRuntime(const fs::path& dir) {
        const fs::path runtime = dir / "runtime" / "ios";
        fs::create_directories(runtime / "template");
        fs::copy(templateDir, runtime / "template", fs::copy_options::recursive);
        TestTempDir::write(runtime / "JasperEnginePlayer.xcframework" / "Info.plist", "x");
        return runtime;
    }

    uint8_t bits(std::initializer_list<ScreenOrientation> orientations) {
        uint8_t mask = 0;
        for (ScreenOrientation o : orientations) mask |= PlatformRules::orientationBit(o);
        return mask;
    }

}

using namespace ios_template_test;

// red before REQ-20: the template's Portrait was what every prebuilt iOS build declared
TEST(IosTemplate, OrientationsFromPresentation) {
    TestTempDir dir("ios-template");
    platform::IosProjectSettings settings;
    settings.appName = "Game";
    settings.displayName = "Game";
    settings.bundleIdentifier = "com.example.game";
    settings.presentation.defaultOrientation = ScreenOrientation::AutoRotation;
    settings.presentation.allowedOrientations = bits({ScreenOrientation::LandscapeLeft, ScreenOrientation::LandscapeRight});
    settings.runtimeDir = fakeRuntime(dir.path());
    settings.projectDir = dir / "project";
    std::string error;
    ASSERT_TRUE(platform::stageIosProject(settings, error)) << error;
    const std::string plist = TestTempDir::read(settings.projectDir / "Info.plist");

    // Unity's Landscape Left is UIKit's LandscapeRight
    const std::vector<std::string> landscape = {"UIInterfaceOrientationLandscapeRight", "UIInterfaceOrientationLandscapeLeft"};
    EXPECT_EQ(arrayOf(plist, "UISupportedInterfaceOrientations"), landscape) << plist;
    EXPECT_EQ(arrayOf(plist, "UISupportedInterfaceOrientations~ipad"), landscape) << plist;
    EXPECT_EQ(boolOf(plist, "UIRequiresFullScreen"), "<true/>") << plist;
    // each key once: replaced, not added beside the template's
    EXPECT_EQ(plist.find("<key>UISupportedInterfaceOrientations</key>"), plist.rfind("<key>UISupportedInterfaceOrientations</key>"));
    EXPECT_EQ(plist.find("<key>UIRequiresFullScreen</key>"), plist.rfind("<key>UIRequiresFullScreen</key>"));
    // the markers are still filled
    EXPECT_EQ(plist.find("{{"), std::string::npos) << plist;
}

// all four rotating: iPad multitasking may stay on; a fixed default lists that one alone
TEST(IosTemplate, AllFourAndFixedOrientations) {
    const std::string text = TestTempDir::read(templateDir / "Info.plist");
    platform::IosProjectSettings settings;
    settings.presentation.defaultOrientation = ScreenOrientation::AutoRotation;
    settings.presentation.allowedOrientations = PlatformRules::ALL_ORIENTATIONS;
    std::string plist = platform::fillIosInfoPlist(text, settings);
    EXPECT_EQ(arrayOf(plist, "UISupportedInterfaceOrientations").size(), 4u) << plist;
    EXPECT_EQ(boolOf(plist, "UIRequiresFullScreen"), "<false/>") << plist;

    settings.presentation.defaultOrientation = ScreenOrientation::Portrait;
    plist = platform::fillIosInfoPlist(text, settings);
    EXPECT_EQ(arrayOf(plist, "UISupportedInterfaceOrientations~ipad"), std::vector<std::string>{"UIInterfaceOrientationPortrait"}) << plist;
    EXPECT_EQ(boolOf(plist, "UIRequiresFullScreen"), "<true/>") << plist;
}
