// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The compile-time window, logging and crash settings a Compile build bakes in (the Exporter sets
// them as definitions of the game's executable), as the runtime PlatformConfig the platform
// layers and the engine read
// (docs/specs/S30-shipping.md R1). Included by each platform's main source, where the macros
// are visible; the defaults are the ones the platforms always had.

#include "PlatformConfig.h"

#ifndef JASPERENGINE_VSYNC_ENABLED
#define JASPERENGINE_VSYNC_ENABLED 1
#endif

// 0 = windowed, 1 = maximized, 2 = fullscreen
#ifndef JASPERENGINE_WINDOW_MODE
#define JASPERENGINE_WINDOW_MODE 0
#endif

#ifndef JASPERENGINE_WINDOW_RESIZABLE
#define JASPERENGINE_WINDOW_RESIZABLE 1
#endif

#ifndef JASPERENGINE_WINDOW_TITLE
#define JASPERENGINE_WINDOW_TITLE "JasperEngine"
#endif

#ifndef JASPERENGINE_APP_ID
#define JASPERENGINE_APP_ID "JasperEngine"
#endif

#ifndef JASPERENGINE_STORAGE_ID
#define JASPERENGINE_STORAGE_ID "JasperEngine"
#endif

#ifndef DEFAULT_WINDOW_WIDTH
#define DEFAULT_WINDOW_WIDTH 960
#endif

#ifndef DEFAULT_WINDOW_HEIGHT
#define DEFAULT_WINDOW_HEIGHT 540
#endif

namespace jasperengine {

    // fills the config from this build's macros; first thing in a platform's main, before
    // player::launch() (a player's game.json, then the launch options, go over it)
    inline PlatformConfig& applyPlatformDefaults() {
        PlatformConfig& config = platformConfig();
        config.windowMode = JASPERENGINE_WINDOW_MODE;
        config.width = DEFAULT_WINDOW_WIDTH;
        config.height = DEFAULT_WINDOW_HEIGHT;
        config.resizable = JASPERENGINE_WINDOW_RESIZABLE != 0;
        config.vsync = JASPERENGINE_VSYNC_ENABLED != 0;
        config.title = JASPERENGINE_WINDOW_TITLE;
        config.appId = JASPERENGINE_APP_ID;
        config.storageId = JASPERENGINE_STORAGE_ID;
        // Player > Logging and Crash Handling, when the Exporter set them (the engine's defaults
        // stay otherwise)
    #ifdef JASPERENGINE_LOG_LEVEL
        config.builtLogLevel = JASPERENGINE_LOG_LEVEL;
    #endif
    #ifdef JASPERENGINE_LOG_FILE
        config.builtLogFile = JASPERENGINE_LOG_FILE != 0 ? 1 : 0;
    #endif
    #ifdef JASPERENGINE_LOG_BACKTRACE
        config.builtLogBacktrace = JASPERENGINE_LOG_BACKTRACE != 0 ? 1 : 0;
    #endif
    #ifdef JASPERENGINE_CRASH_LOG_TAIL
        config.builtCrashLogTail = JASPERENGINE_CRASH_LOG_TAIL != 0;
    #endif
    #ifdef JASPERENGINE_CRASH_DUMP_WITH_HEAP
        config.builtCrashDumpWithHeap = JASPERENGINE_CRASH_DUMP_WITH_HEAP != 0;
    #endif
    #ifdef JASPERENGINE_CRASH_HANG_MS
        config.builtHangMs = JASPERENGINE_CRASH_HANG_MS;
    #endif
    #ifdef JASPERENGINE_CRASH_WEB_OVERLAY
        config.builtCrashWebOverlay = JASPERENGINE_CRASH_WEB_OVERLAY != 0;
    #endif
        return config;
    }

}
