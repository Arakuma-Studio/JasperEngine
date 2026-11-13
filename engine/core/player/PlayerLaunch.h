// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A game's start, before its window exists (docs/specs/S30-shipping.md D3, R17): every
// platform's main fills the platform configuration from its build (applyPlatformDefaults), then
// calls launch(), which parses the command line (a web page's URL query), lets a prebuilt player
// read its game.json and content manifest, and applies the options over both.

#include "Export.h"
#include "player/LaunchOptions.h"

#include <string>
#include <vector>

namespace jasperengine::player {

    // A prebuilt player's step (engine/player/PlayerMain.cpp registers it): reads game.json
    // and checks the content into platformConfig(); -1 to go on, else the exit code
    using GameConfigurator = int (*)(const LaunchOptions& options);
    JASPERENGINE_API void setGameConfigurator(GameConfigurator configurator);
    JASPERENGINE_API bool hasGameConfigurator();

    // -1: start the game; else the process exit code (2: bad arguments, 5: content mismatch).
    // A player parses strictly (an unknown flag is an error), a Compile build leniently; the web
    // always leniently (a page's URL carries queries of its own)
    JASPERENGINE_API int launch(int argc, char** argv);
    JASPERENGINE_API int launch(const std::vector<std::string>& args, ParseMode mode);

}
