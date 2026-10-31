// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A prebuilt Windows player's icon and version resources (docs/specs/S30-shipping.md R12),
// patched with BeginUpdateResourceW / UpdateResourceW on a Windows host. Other hosts leave the
// engine's (the build warns W-ICON-NONWINDOWS); the window icon comes from content at runtime.

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::platform {

    struct WindowsVersionInfo {
        std::string productName;
        std::string companyName;
        std::string fileVersion;      // "1.2.3.4" (missing parts are 0)
        std::string productVersion;
    };

    bool canPatchExecutableResources();
    bool patchExecutableResources(const std::filesystem::path& exe, const std::filesystem::path& iconPng, const WindowsVersionInfo& info,
                                  std::string& error);

    // the VS_VERSIONINFO resource bytes (also what the tests check on every host)
    std::vector<uint8_t> buildVersionResource(const WindowsVersionInfo& info);
    // a GRPICONDIR and its RT_ICON images (PNG) at 16, 32, 48 and 256 from a PNG
    bool buildIconResources(const std::filesystem::path& png, std::vector<uint8_t>& group, std::vector<std::vector<uint8_t>>& icons,
                            std::string& error);

}
