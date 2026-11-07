// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A prebuilt macOS player's .app (docs/specs/S30-shipping.md R12): the executable named for the
// game, Info.plist, AppIcon.icns from the project icon (our own PNG-in-ICNS writer, ic07-ic10),
// then a signature: codesign on a Mac, rcodesign elsewhere when on PATH, else unsigned with the
// blocking warning W-MAC-UNSIGNED (publish verify refuses it: an unsigned arm64 binary does not
// launch).

#include "ShippingTypes.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor::platform {

    struct MacBundleSettings {
        std::string bundleName;
        std::string executable;
        std::string bundleIdentifier;
        std::string version;
        std::string build;
        // the players' and the engine's deployment target (engine/CMakeLists.txt): 11.0 here let
        // macOS 11 open a bundle whose binary needs 12
        std::string minimumSystemVersion = "12.0";
        bool highDpi = true;
        std::filesystem::path iconPng;
        // codesign's identity ("-": ad hoc); JASPER_MACOS_SIGN_IDENTITY overrides
        std::string signIdentity = "-";
        // Contents/Resources/PrivacyInfo.xcprivacy ("": the runtime's stays)
        std::string privacyManifest;
        // the project's Info.plist entries (macos.plist_entries), applied to the written plist
        std::function<void(std::string& plist)> customizePlist;
    };

    std::string infoPlist(const MacBundleSettings& settings);
    // the ICNS bytes of a PNG: 128, 256, 512 and 1024 pixel PNG entries
    bool writeIcns(const std::filesystem::path& png, std::vector<uint8_t>& icns, std::string& error);
    bool finishMacBundle(const std::filesystem::path& bundle, const MacBundleSettings& settings, std::vector<BuildWarning>& warnings,
                         std::string& error);

}
