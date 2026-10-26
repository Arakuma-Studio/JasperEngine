// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DIAG_CRASHHANDLER_H
#define DIAG_CRASHHANDLER_H

// The process's crash handler (docs/crash-diagnostics.md), shared by the editor and every
// exported game: a fatal signal, an unhandled SEH exception, std::terminate, abort, a pure call
// or an invalid CRT parameter writes a report folder
//
//   <root>/reports/<yyyymmddThhmmssZ>-<pid>/crash.txt      header, reason, registers, frames, modules
//                                           crash.dmp      Windows minidump
//                                           log-tail.txt   the last log lines (the crash ring)
//                                           hang-001.txt   hang watchdog reports (HangWatchdog.h)
//                                           handled        the notice was dismissed / the hook took it
//   <root>/sessions/<pid>.json                             this session is running
//
// and then hands the crash on to whatever handled it before (debuggerd tombstones, sanitizers,
// ReportCrash, WER), so the OS report still happens. install() does every allocation; the
// signal path (CrashHandlerPosix.cpp) only uses async-signal-safe calls, Windows writes from a
// thread started at install (CrashHandlerWin.cpp). The engine never sends a report anywhere:
// a game developer's hook gets the pending ones at the next start (setReportHook).

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::diag::crash {

    // What a Windows minidump holds (D4). Small: threads, stacks, modules. WithHeap adds the
    // memory the stacks point to. Editor: the editor's set (full memory in Debug builds).
    enum class DumpKind : uint8_t {
        Small,
        WithHeap,
        Editor,
    };

    struct Config {
        const char* product = "JasperEngine";     // "JasperEngine Editor", the game's name
        std::filesystem::path root;               // JASPERENGINE_CRASH_REPORT_DIR overrides it
        size_t ringBytes = 64 << 10;               // the crash log ring (editor 256 KiB)
        DumpKind dumpKind = DumpKind::Small;
        bool includeLogTail = true;
        std::string version;                       // the editor's or the game's version
        std::string backend;                       // graphics backend: glcore, vulkan, metal ...
        // When set, the FATAL block (reason, report folder, frames) is appended there too, after
        // the logger wrote what it still had queued (editor.log)
        std::filesystem::path logFile;
        // Retention at install (R11): the newest keepReports folders, at most keepBytes; an
        // unhandled folder younger than keepUnhandledDays survives the size cap (0: no such rule)
        size_t keepReports = 5;
        uint64_t keepBytes = 50ull << 20;
        int keepUnhandledDays = 0;
        // Web: the page's "The game stopped working. [Reload]" overlay (crash_pre.js)
        bool webOverlay = true;
    };

    // The editor's config: 256 KiB ring, 20 reports and 2 GiB kept, editor dumps
    JASPERENGINE_API Config editorConfig(const std::filesystem::path& root, const std::string& version);

    // One report folder (a session that ended in a crash, a hang, or without saying why)
    struct ReportInfo {
        std::string id;                            // the folder name: <yyyymmddThhmmssZ>-<pid>
        std::string kind;                          // crash, hang or abnormal_exit
        std::string reason;                        // "SIGSEGV (invalid memory access)", a hang's phase ...
        std::string time;                          // when it happened (UTC, ISO 8601)
        int64_t timeUnix = 0;
        std::string product;
        std::string version;
        std::filesystem::path folder;
        std::vector<std::filesystem::path> files;  // inside folder
        bool handled = false;
    };

    // Once, on the main thread, as early as possible. Prepares the report folders, the session
    // marker, the ring and retention everywhere; arms a handler where this build has a backend
    // (the web's page hooks today). Returns whether one was armed: on Windows, Linux, macOS,
    // Android and iOS the native backends are not part of this build yet, nothing is armed, and
    // the process keeps whatever handler it had (the editor's editor/util/CrashHandler).
    JASPERENGINE_API bool install(const Config& config);
    JASPERENGINE_API bool isInstalled();
    // A clean exit: the session marker goes (an unclean one leaves it for the next start)
    JASPERENGINE_API void shutdown();

    // The calling thread gets an alternate signal stack (POSIX) or a stack guarantee (Windows),
    // so its stack overflow still reports; released when the thread ends. thread::spawnNamed
    // and the job system's workers call it; the main thread gets one at install.
    JASPERENGINE_API void prepareThread();

    // A line for log-tail.txt (lock-free, a torn line at a wrap is acceptable). The logger feeds
    // it once install() ran; a guarded crash that was recovered adds a breadcrumb.
    JASPERENGINE_API void ringWrite(std::string_view line);
    // The ring's contents, oldest first (tests, the web)
    JASPERENGINE_API std::string ringContents();

    // A key: value line of crash.txt (16 slots of 128 bytes; an empty value removes it)
    JASPERENGINE_API void setTag(std::string_view key, std::string_view value);

    // Where reports go: <root>/reports (empty before install)
    JASPERENGINE_API std::filesystem::path reportsDirectory();
    // This session's folder (it exists once something was reported)
    JASPERENGINE_API std::filesystem::path sessionFolder();
    JASPERENGINE_API const std::string& sessionId();

    // Report folders, newest first (limit 0: all)
    JASPERENGINE_API std::vector<ReportInfo> listReports(size_t limit = 0);
    // Reports of earlier sessions nobody handled yet, newest first
    JASPERENGINE_API std::vector<ReportInfo> pendingReports();
    JASPERENGINE_API std::optional<ReportInfo> findReport(std::string_view id);
    // Writes `handled` into the report's folder; false for an unknown id
    JASPERENGINE_API bool markHandled(std::string_view id);
    // The previous session (found at install) ended without a clean shutdown
    JASPERENGINE_API bool lastSessionEndedAbnormally();
    // That session's report, while it is unhandled
    JASPERENGINE_API std::optional<ReportInfo> lastSessionReport();

    // Called once per pending report, now for the ones already pending and at install for those
    // found later; true marks it handled. The engine does no network I/O: the hook decides.
    using ReportHook = std::function<bool(const ReportInfo&)>;
    JASPERENGINE_API void setReportHook(ReportHook hook);

    // Development builds and tests: crash on purpose. segv, abort, terminate, stack_overflow,
    // worker_segv, fpe, pure_call (Windows), hang (a 6 s main-thread sleep). False for an
    // unknown mode; otherwise it does not return (but hang).
    JASPERENGINE_API bool debugCrash(std::string_view mode, uint32_t hangMs = 6000);
    JASPERENGINE_API bool isDebugCrashMode(std::string_view mode);
    // JASPERENGINE_DEBUG_CRASH=<mode>[@<frame>] (web: ?jasperengine_debug_crash=<mode>[@<frame>]),
    // parsed once; Engine::systemDraw fires it at that frame in development builds, or without
    // one 30 frames after every scene loaded
    struct DebugCrashRequest {
        std::string mode;
        uint64_t frame = 0;   // 0: after loading
    };
    JASPERENGINE_API std::optional<DebugCrashRequest> debugCrashRequest();

    // Writes `name` into this session's report folder (creating it), outside any handler: the
    // hang watchdog's reports. Returns the file's path, empty on failure.
    JASPERENGINE_API std::filesystem::path writeSessionFile(const std::string& name, std::string_view text);

    // The crash.txt header fields (also the hang reports')
    JASPERENGINE_API const std::string& buildId();
    JASPERENGINE_API const char* platformName();

    // Test seams (CrashTestMain, unit tests): retention over a root, the classification of a
    // session folder, the marker scan of a root without installing
    JASPERENGINE_API void pruneReports(const std::filesystem::path& reportsDir, size_t keep, uint64_t maxBytes,
                                       int keepUnhandledDays);
    JASPERENGINE_API std::optional<ReportInfo> readReport(const std::filesystem::path& folder);

}

#endif // DIAG_CRASHHANDLER_H
