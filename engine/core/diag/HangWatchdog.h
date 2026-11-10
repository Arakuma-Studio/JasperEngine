// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The heartbeat, the shutdown window, the re-report interval and the job-occupancy section are
// adapted from OpenEngine's MainThreadHangWatchdog (Apps/Editor/Source/Diagnostics/), Copyright
// (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef DIAG_HANGWATCHDOG_H
#define DIAG_HANGWATCHDOG_H

// The main-thread hang watchdog (docs/crash-diagnostics.md): a thread ("Hang Watchdog") polls
// the main loop's heartbeat and, when it stops for longer than the threshold, writes
// hang-NNN.txt into the session's report folder (CrashHandler.h) and a "JASPERENGINE HANG:"
// line to stderr. It never takes a lock the main thread may hold; the Console line comes from
// the main thread once it recovers (takeNotices).
//
// Busy scopes raise the threshold for known long work (an import, a native file dialog) but
// never silence it: a wedged import still reports once its budget is spent. Phase scopes only
// label what the main thread is doing. Both are for the main thread; elsewhere they do nothing.

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::diag {

    // One per main-loop iteration (one relaxed increment): the watchdog arms on the first one
    // after start()
    JASPERENGINE_API void heartbeat();
    // The heartbeats so far (any thread): the main loop's iterations, also where nothing renders
    JASPERENGINE_API uint64_t heartbeats();

    struct WatchdogConfig {
        uint32_t thresholdMs = 3000;      // editor 3 s, development games 5 s
        uint32_t shutdownMs = 15000;      // the window after beginShutdownWatch()
        bool enabled = true;
        const char* product = "JasperEngine";
        std::string version;
        // Tests: the poll period (0: no thread, the test calls watchdog::poll), the re-report
        // interval and the clock (steady nanoseconds)
        uint32_t pollMs = 250;
        uint32_t reReportMs = 5000;
        std::function<uint64_t()> clock;
        // Each report and each recovery, on the watchdog thread (tests); after the file is written
        std::function<void(const struct HangReport&)> onReport;
    };

    struct HangReport {
        uint32_t index = 0;               // the session's hang-NNN number (recoveries: the stall's)
        bool recovered = false;           // the "recovered after X s" event
        bool shutdown = false;            // reported in the shutdown window
        uint64_t stalledMs = 0;
        uint64_t thresholdMs = 0;         // the effective one (busy scopes included)
        std::string phase;
        std::vector<std::string> busyScopes;   // outermost first
        std::string text;                 // the report as written
        std::filesystem::path file;       // hang-NNN.txt, empty without a report folder
    };

    struct HealthSnapshot {
        bool enabled = false;
        bool responsive = true;
        bool shutdownWindow = false;
        uint64_t stalledMs = 0;           // since the last heartbeat the watchdog saw
        uint32_t thresholdMs = 0;
        uint32_t reports = 0;             // hang reports this session
        std::string phase;
        std::vector<std::string> busyScopes;
    };

    // What the main thread shows once a stall ended (the editor's toast, a game's log line)
    struct HangNotice {
        uint64_t stalledMs = 0;
        std::string label;                // the innermost busy scope, else the phase
        std::filesystem::path file;
    };

    // What the main thread was doing, read by the watchdog while it stalls (fixed buffers)
    enum class ContextSlot : uint8_t {
        PlayState,
        Scene,
        Lua,
        Count,
    };

    namespace watchdog {

        // On the main thread (it is the one watched). JASPERENGINE_HANG_WATCHDOG=0|1,
        // JASPERENGINE_HANG_WATCHDOG_MS and JASPERENGINE_HANG_WATCHDOG_SHUTDOWN_MS override the
        // config. Disabled: no thread.
        JASPERENGINE_API void start(const WatchdogConfig& config);
        // The main loop is over: the threshold becomes shutdownMs, the phase "shutdown", and the
        // job system is no longer read (its shutdown follows)
        JASPERENGINE_API void beginShutdownWatch();
        JASPERENGINE_API void stop();
        JASPERENGINE_API bool isRunning();
        // Any thread, lock-free with respect to the main thread
        JASPERENGINE_API HealthSnapshot health();
        JASPERENGINE_API void setThresholdMs(uint32_t thresholdMs);
        // Mobile background: no heartbeat is expected; resume re-arms on the next beat
        JASPERENGINE_API void setPaused(bool paused);
        // Engine::systemDraw: the frame number and the last frame's duration for reports
        JASPERENGINE_API void setFrameInfo(uint64_t frame, float lastFrameMs);
        JASPERENGINE_API void setContext(ContextSlot slot, std::string_view value);
        // Stalls that ended since the last call (main thread)
        JASPERENGINE_API std::vector<HangNotice> takeNotices();

        // Runaway Lua (editor Play): while the stall's phase is a Lua phase ("lua..."), the
        // watchdog asks `locate` once per stall to name where the script is (it fills the Lua
        // context slot from the main thread), and calls `interrupt` once the stall passes
        // breakAfterMs (0: never). Both run on the watchdog thread; set them from the main thread.
        struct LuaHooks {
            std::function<void()> locate;
            std::function<void(uint64_t stalledMs)> interrupt;
        };
        JASPERENGINE_API void setLuaHooks(LuaHooks hooks);
        JASPERENGINE_API void setBreakLuaAfterMs(uint32_t milliseconds);
        JASPERENGINE_API uint32_t breakLuaAfterMs();

        // Tests (pollMs = 0): one watchdog tick, as the thread would run it
        JASPERENGINE_API void poll();

    }

    // A known long operation of the main thread: a stall reports only past max(threshold,
    // budget), and the report names every active scope. budgetMs 0 = no limit. The label is
    // copied (96 bytes); 16 scopes nest.
    class JASPERENGINE_API BusyScope {
    public:
        explicit BusyScope(std::string_view label, uint32_t budgetMs = 120'000);
        ~BusyScope();
        BusyScope(const BusyScope&) = delete;
        BusyScope& operator=(const BusyScope&) = delete;

    private:
        bool entered = false;
    };

    // Labels the main thread's work without changing the budget; staticLabel must outlive it
    // (a string literal). Phases starting with "lua" are Lua phases.
    class JASPERENGINE_API PhaseScope {
    public:
        explicit PhaseScope(const char* staticLabel);
        ~PhaseScope();
        PhaseScope(const PhaseScope&) = delete;
        PhaseScope& operator=(const PhaseScope&) = delete;

    private:
        const char* previous = nullptr;
        bool entered = false;
    };

    namespace watchdog::detail {

        // Platform capture is initialized on the watched thread before the worker starts.
        JASPERENGINE_INTERNAL void initializeMainThreadCapture();
        // Windows deadline regression: delay the helper beyond the capture budget.
        JASPERENGINE_API void forceWalkerStallForTest(bool enabled);
        // Writes #N module+offset [symbol] lines; returns 0 with a reason on timeout/failure.
        JASPERENGINE_API size_t captureMainThreadStack(char* out, size_t capacity, const char** why);

    }

}

#endif // DIAG_HANGWATCHDOG_H
