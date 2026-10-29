// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The checks a build, `publish prepare` and `publish verify` make of a payload before it can ship
// (docs/specs/S30-shipping.md R30), every finding listed: binaries of the wrong architecture or
// format (ELF, PE, Mach-O and each slice of a universal binary, Android libraries against their
// ABI directory, the wasm magic), a missing exec bit, case-colliding paths, escaping or absolute
// symlinks (none at all for Windows and the web), Windows-illegal names and over-long paths,
// steam_appid.txt, missing content (game.json, content-manifest.json, the start scene, scenes,
// prefabs, shaders), missing notices, a player-mismatch build, an unsigned macOS arm64 slice,
// and for Android an APK that apksigner rejects or a library not 16 KB aligned. Portions adapted
// from OpenEngine Tools/Scripts/publish_game.py (MIT), validate_payload() rewritten for our layout.

#include "ShippingTypes.h"
#include "publish/TreeManifest.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor::publish {

    struct ValidationTarget {
        BuildPlatform platform = BuildPlatform::Linux;
        std::string arch = "x64";
        std::string backend;
        // the main executable, payload-relative (empty: found from the layout)
        std::string executable;
        // runs `apksigner verify --verbose <apk>`: true when it accepts (tests inject one);
        // null: the SDK's apksigner (a missing one is a finding)
        std::function<bool(const std::filesystem::path& apk, std::string& output)> verifyApk;
    };

    TargetOs targetOs(BuildPlatform platform);

    // true when there are no findings
    bool validatePayload(const std::filesystem::path& root, const ValidationTarget& target, std::vector<std::string>& findings);

    // the names Windows refuses (CON, NUL, COM1..., a trailing dot or space, ':'), "" when legal
    std::string windowsNameProblem(const std::string& relativePath);

}
