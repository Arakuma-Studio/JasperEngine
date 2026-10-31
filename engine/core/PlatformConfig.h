// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What the platform layers read when they create the window, the graphics context and the
// paths (docs/specs/S30-shipping.md R1, R2, R17): a Compile build fills it from its compile-time
// macros (platform/common/PlatformDefaults.h), a prebuilt player from game.json, and the launch
// options (--headless, --resolution, --content) adjust either before the window exists.

#include "Export.h"

#include <string>

namespace jasperengine {

    struct PlatformConfig {
        // 0 = windowed, 1 = maximized, 2 = fullscreen
        int windowMode = 0;
        int width = 960;
        int height = 540;
        bool resizable = true;
        bool vsync = true;
        std::string title = "JasperEngine";
        // X11 class / Wayland app_id, the .desktop file's name
        std::string appId = "JasperEngine";
        // the web build's saved-keys prefix
        std::string storageId = "JasperEngine";
        // a PNG the window and taskbar icon is set from at start (a player's content icon.png;
        // empty: the platform's built-in or none)
        std::string iconPath;

        // never shown: X11 unmapped, Win32 not shown, macOS without a Dock icon (rendering goes on)
        bool headless = false;
        // no audio output is opened
        bool silentAudio = false;

        // the root assets/, lua/, game.json and the cooked scenes live in (empty: the platform's
        // own paths, relative to the working directory or set at build time)
        std::string contentRoot;

        // Player > Logging and Crash Handling of a prebuilt player (game.json). -1 / empty: the
        // build's own (below)
        std::string logLevel;
        int logFile = -1;
        int logBacktrace = -1;
        int crashHandler = -1;
        int crashLogTail = -1;
        int crashDumpWithHeap = -1;
        int hangWatchdog = -1;
        int hangMs = 0;
        int crashWebOverlay = -1;

        // The build's own Player > Logging and Crash Handling: a Compile build's settings, which
        // the Exporter sets as macros of the game's executable and applyPlatformDefaults() copies
        // here (docs/specs/S33-build-speed.md R5), else the engine's defaults. -1 / empty: none
        // (the platform's or the logger's default)
        std::string builtLogLevel;
        int builtLogFile = -1;
        int builtLogBacktrace = -1;
        bool builtCrashLogTail = true;
        bool builtCrashDumpWithHeap = false;
        int builtHangMs = 5000;
        bool builtCrashWebOverlay = true;
    };

    JASPERENGINE_API PlatformConfig& platformConfig();

}
