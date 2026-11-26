// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Patch And Run's development content folder (docs/specs/build-and-run-on-device.md R7, D1-D2):
// the editor mirrors a Development Build's cooked content into <app data>/je-dev-content/ of an
// installed Android app or iOS Simulator app, and a development player runs from there instead of
// its package when the folder's content-manifest.json is for this very player. Release players
// never look: the check is compiled only when JASPERENGINE_DEVELOPMENT_BUILD is set.

#include "Export.h"

#include <functional>
#include <optional>
#include <string>

namespace jasperengine::player {

    // the folder's name inside the app's data folder (Android: files/; iOS: Library/)
    inline constexpr const char* DEVELOPMENT_CONTENT_DIR = "je-dev-content";

    struct DevelopmentContentAccess {
        // the one stat a development player makes at start
        std::function<bool(const std::string& path)> exists;
        std::function<bool(const std::string& path, std::string& text)> read;
        std::function<void(const std::string& message)> warn;
    };

    struct DevelopmentContent {
        std::string root;
        // content-manifest.json's patch (System.getBuildInfo().contentPatch)
        int patch = 0;
    };

    // The folder's content when its manifest names this player: `runningPlayerId` (runtimePlayerId())
    // and `packagedPlayerSha256`, the player hash of the content the package holds (it was built
    // for the running binary). Nothing when `contentGiven` (an explicit --content wins, no file is
    // read). Another player's content logs one warning and gives nothing.
    JASPERENGINE_API std::optional<DevelopmentContent> readDevelopmentContent(const std::string& folder, bool contentGiven, const std::string& runningPlayerId,
                                                                              const std::function<std::string()>& packagedPlayerSha256,
                                                                              const DevelopmentContentAccess& access);

    // The gate: a release player compiles to a constant nothing (Development false), touching no file
    template <bool Development>
    std::optional<DevelopmentContent> findDevelopmentContentFor(const std::string& folder, bool contentGiven, const std::string& runningPlayerId,
                                                                const std::function<std::string()>& packagedPlayerSha256,
                                                                const DevelopmentContentAccess& access) {
        if constexpr (Development) {
            return readDevelopmentContent(folder, contentGiven, runningPlayerId, packagedPlayerSha256, access);
        } else {
            (void)folder;
            (void)contentGiven;
            (void)runningPlayerId;
            (void)packagedPlayerSha256;
            (void)access;
            return std::nullopt;
        }
    }

#if defined(JASPERENGINE_DEVELOPMENT_BUILD) && JASPERENGINE_DEVELOPMENT_BUILD
    inline constexpr bool DEVELOPMENT_CONTENT_CHECKED = true;
#else
    inline constexpr bool DEVELOPMENT_CONTENT_CHECKED = false;
#endif

    // what a player calls at start
    inline std::optional<DevelopmentContent> findDevelopmentContent(const std::string& folder, bool contentGiven, const std::string& runningPlayerId,
                                                                    const std::function<std::string()>& packagedPlayerSha256,
                                                                    const DevelopmentContentAccess& access) {
        return findDevelopmentContentFor<DEVELOPMENT_CONTENT_CHECKED>(folder, contentGiven, runningPlayerId, packagedPlayerSha256, access);
    }

}
