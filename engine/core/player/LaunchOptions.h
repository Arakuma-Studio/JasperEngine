// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The command line of a shipped game (docs/specs/S30-shipping.md R17-R19): headless runs, fixed
// frame rates, frame counts and screenshots, so CI and agents can run a build and look at it.
// The web player takes the same options from its page's URL query (?quit-after=30&...).
//
// Flag names and the strict number rules follow OpenEngine's Apps/Player/Source/PlayerRunOptions
// (MIT); the code is our own. Unlike theirs, --headless keeps the render device (D9), so
// --screenshot works headless.

#include "Export.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace jasperengine::player {

    struct LaunchOptions {
        bool headless = false;
        bool afterReady = false;
        bool silentAudio = false;
        bool failOnError = false;
        bool verifyContent = false;
        // 0: run until the game quits
        uint64_t quitAfter = 0;
        // 0: the last frame before quitting
        uint64_t screenshotFrame = 0;
        // 0: the measured frame time
        double fixedFps = 0.0;
        double readyTimeout = 120.0;
        std::filesystem::path screenshot;
        // where a development player writes its audio capture sessions
        std::filesystem::path audioCapture;
        std::filesystem::path content;
        // 0: the game's window size
        int width = 0;
        int height = 0;
        std::string scene;
        std::string logLevel;
        // what follows "--" (a strict parse), or every argument the options do not know (a
        // lenient one): the Lua System.args
        std::vector<std::string> scriptArgs;
    };

    // printed (with "run with --help" style advice in the text), exit code 2
    struct ParseError {
        std::string message;
    };

    enum class ParseMode {
        // a prebuilt player: an unknown "--" flag is an error
        Strict,
        // a Compile build, whose game may read flags of its own: unknown arguments go to scriptArgs
        Lenient
    };

    // args: the command line without the program name. The engine's own flags (--log-level,
    // --profile, --profile-capture, --profile-frames) are accepted and left to the engine.
    JASPERENGINE_API std::variant<LaunchOptions, ParseError> parse(std::span<const std::string> args,
                                                                   ParseMode mode = ParseMode::Strict);

    // "--name=value" for a web page's query items ("quit-after=30", "headless"): the same parse
    JASPERENGINE_API std::vector<std::string> argsFromQuery(const std::string& query);

    // the usage text (--help)
    JASPERENGINE_API std::string usage();

    // whole-string numbers (no sign for the unsigned one, no trailing text)
    JASPERENGINE_API bool parseUint64(const std::string& text, uint64_t& out);
    JASPERENGINE_API bool parseDouble(const std::string& text, double& out);

}
