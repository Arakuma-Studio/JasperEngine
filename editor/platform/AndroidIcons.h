// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// An Android game's launcher icons from Project Settings > Android, the same for a source export's
// Gradle project and a prebuilt package (docs/specs/S98-editor-tooling-gaps.md REQ-19), as Unity's
// Player settings make them from one image: every density (mipmap-mdpi to -xxxhdpi), a round
// icon, and from the adaptive foreground and background their 108 dp layers with the
// mipmap-anydpi-v26 XML. Resampled with stb_image_resize, as the Apple icons are.

#include "Project.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::platform {

    struct AndroidIconDensity {
        const char* folder;      // mipmap-mdpi ...
        int legacyPx;            // 48 dp
        int adaptivePx;          // 108 dp
    };
    const std::vector<AndroidIconDensity>& androidIconDensities();

    struct AndroidIcons {
        // the manifest's android:icon and android:roundIcon ("" when the project sets no icon: the
        // template's or the player's own stays)
        std::string icon;
        std::string roundIcon;
        // what was written, relative to the res folder (generic separators)
        std::vector<std::string> files;
    };

    // Writes the icons named `name` (ic_launcher: <res>/mipmap-*/ic_launcher.png,
    // ic_launcher_round.png, ...) into resDir. The legacy and round icons come from the Launcher
    // Icon, else from the adaptive layers composed. Paths relative to projectPath. Without an
    // adaptive icon, a mipmap-anydpi-v26 XML of that name an earlier export left is removed.
    bool writeAndroidLauncherIcons(const AndroidProjectSettings& settings, const std::filesystem::path& projectPath,
                                   const std::filesystem::path& resDir, const std::string& name, AndroidIcons& icons, std::string& error);

}
