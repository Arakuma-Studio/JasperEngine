// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor's side of the crash and hang diagnostics (docs/crash-diagnostics.md): the engine's
// report folders under the config directory (next to editor.log), the main-thread hang watchdog
// with Preferences > Diagnostics, Play's runaway-Lua limit, the "closed unexpectedly last time"
// notice and the "was not responding" toast. editor/util/CrashHandler stays the editor's
// signal handler where the engine's crash backend is not part of the build.

#include "AppSettings.h"
#include "diag/CrashHandler.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace jasperengine::editor {

    class EditorDiagnostics {
    public:
        // App::initializeSettings, after the config directory is known (once)
        static void install(const std::filesystem::path& configDirectory);
        // AppSettings::setDiagnosticsSettings: the watchdog follows at once
        static void applySettings(const DiagnosticsSettings& settings);
        // The main loop is over (App::shutdownBackgroundWork) / the engine is down (engineShutdown)
        static void beginShutdown();
        static void end();

        // Play started or stopped: the Lua limit applies while it runs
        static void setPlaying(bool playing, const std::string& sceneName);

        // `--allow-debug-crash` or JASPERENGINE_ALLOW_DEBUG_CRASH=1: debug_crash may crash it
        static void setDebugCrashAllowed(bool allowed);
        static bool isDebugCrashAllowed();

        // The abnormal previous session's notice (R27): shown until dismissed
        static std::optional<diag::crash::ReportInfo> notice();
        static bool isNoticeOpen();
        // Writes `handled` and closes the notice when it is that report's; false: unknown id
        static bool dismiss(const std::string& id);
        // "JasperEngine closed unexpectedly last time (SIGSEGV at 14:32). A crash report was saved."
        static std::string noticeText(const diag::crash::ReportInfo& report);

        // Each frame on the main thread: recovered stalls become a Console warning and the
        // footer toast (at most one per 60 s, shown 8 s)
        static void pollNotices();
        struct Toast {
            std::string text;
            std::filesystem::path file;
        };
        static std::optional<Toast> activeToast();
        static void closeToast();
        static std::string hangToastText(uint64_t stalledMs, const std::string& label);

        // The preferred file of a report folder: crash.txt, the newest hang report, session.json
        static std::filesystem::path primaryFile(const diag::crash::ReportInfo& report);
    };

}
