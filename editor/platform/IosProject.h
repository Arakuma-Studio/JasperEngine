// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A prebuilt iOS player's Xcode project (docs/specs/S30-shipping.md R14): the runtime's template
// project, which links JasperEnginePlayer.xcframework and compiles no engine source, with the
// game's content in Content/, its Info.plist, bundle id, team and icon. On a Mac with Xcode it is
// built: an unsigned simulator .app, or an archive exported to an .ipa (development, ad-hoc or
// app-store); with no team, the device build is compiled and stops before signing, and says so.
// Elsewhere, or on a Mac without Xcode, the project is the result ("Open on a Mac with Xcode").

#include "ShippingTypes.h"
#include "util/CommandRunner.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor::platform {

    struct IosProjectSettings {
        std::string appName;
        std::string displayName;
        std::string bundleIdentifier;
        std::string version;
        std::string build;
        std::string teamId;
        std::string exportMethod = "development";
        bool hideStatusBar = true;
        std::filesystem::path iconPng;
        std::filesystem::path runtimeDir;     // the player runtime's ios/
        std::filesystem::path projectDir;     // the staged project (Content/ is in it already)
        bool simulator = false;
    };

    // The Xcode a build uses: DEVELOPER_DIR when it is set, the active developer directory when that
    // is a full Xcode, else one in /Applications through DEVELOPER_DIR for the build's own tools
    // (many Macs keep xcode-select on the Command Line Tools, which have no xcodebuild; the Mac's
    // setting is left alone). missing says what is wrong when there is none.
    struct XcodeChoice {
        std::filesystem::path developerDir;
        bool throughEnvironment = false;
        std::string missing;
        bool found() const { return missing.empty(); }
    };
    XcodeChoice chooseXcode(const std::string& developerDirVariable, const std::string& activeDeveloperDir,
                            const std::vector<std::filesystem::path>& applications);
    // this computer's: always missing outside macOS
    XcodeChoice findXcode();

    // What xcodebuild's output says stopped it, in words a user can act on ("" when it says none of
    // it): no team, no account or certificate for the team, the licence, the platform not installed
    std::string describeXcodeFailure(const std::string& log, const std::string& teamId);

    // the template's text with the settings in its {{MARKERS}}
    std::string fillIosTemplate(std::string text, const IosProjectSettings& settings);

    bool buildIosProject(const IosProjectSettings& settings, CommandRunner& runner, const std::function<bool()>& cancelled,
                         std::vector<std::filesystem::path>& artifacts, std::vector<std::string>& notes,
                         std::vector<BuildWarning>& warnings, std::string& error);

}
