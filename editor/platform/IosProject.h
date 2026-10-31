// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A prebuilt iOS player's Xcode project (docs/specs/S30-shipping.md R14): the runtime's template
// project, which links JasperEnginePlayer.xcframework and compiles no engine source, with the
// game's content in Content/, its Info.plist, bundle id, team and icon. On a Mac with Xcode it is
// built: an unsigned simulator .app, or an archive exported to an .ipa (development, ad-hoc or
// app-store); elsewhere the project is the result ("Open on a Mac with Xcode").

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

    // the template's text with the settings in its {{MARKERS}}
    std::string fillIosTemplate(std::string text, const IosProjectSettings& settings);

    bool buildIosProject(const IosProjectSettings& settings, CommandRunner& runner, const std::function<bool()>& cancelled,
                         std::vector<std::filesystem::path>& artifacts, std::vector<std::string>& notes, std::string& error);

}
