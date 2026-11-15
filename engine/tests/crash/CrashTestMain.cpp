// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// jasperengine-crash-test --mode <mode> --out DIR [--decline] [--abandon]: the process the
// crash-diagnostics tests start (engine/tests/core/diag, engine/tests/crash/test_crash.py). It
// installs the crash handler with DIR as its report root, logs a line, then:
//
//   segv, abort, terminate, stack_overflow, worker_segv, fpe, pure_call
//            crashes that way (debugCrash), after logging "last line before the crash"
//   hook     sets a report hook that appends each report's id to DIR/hook-calls.txt and takes
//            it (--decline: leaves it pending)
//   session  exits cleanly (--abandon: exits without the clean shutdown, as a killed process)
//   list     writes DIR/reports.txt: "abnormal <0|1>", "last <id|->", then "<id> <kind> <handled>"
//   where    writes the reports directory install resolved to DIR/where.txt (DIR may be relative)
//   ring     logs 300 lines, writes the crash ring to DIR/ring.txt
//   hang     starts the watchdog (200 ms) and stalls the main thread for 700 ms in a phase

#include "diag/CrashHandler.h"
#include "diag/detail/CrashState.h"
#include "diag/HangWatchdog.h"
#include "log/Logger.h"
#include "util/CrashGuard.h"
#include "thread/ThreadUtil.h"

#include <chrono>
#include <csignal>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

using namespace jasperengine;

namespace je_crash_test {
    std::atomic<bool> reportEntered{false};
    bool recursiveFault = false;
#if defined(_WIN32)
    bool recursiveAbort = false;
#endif
    void pauseReport() {
        reportEntered.store(true, std::memory_order_release);
        if (recursiveFault) {
#if defined(_WIN32)
            if (recursiveAbort) std::abort();
            RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
            raise(SIGSEGV);
#endif
        }
        // Deterministically leave time for a different thread to fault inside this report.
        // The POSIX hook itself only uses lock-free atomics and async-signal-safe calls.
#if defined(_WIN32)
        Sleep(250);
#else
        timespec pause{0, 250000000};
        while (nanosleep(&pause, &pause) != 0) {}
#endif
    }

#if !defined(_WIN32)
    char sentinelPath[1024]{};
    bool previousReturns = false;
    void previousHandler(int sig, siginfo_t*, void*) {
        int fd = open(sentinelPath, O_CREAT | O_WRONLY, 0600);
        if (fd >= 0) {
            const ssize_t ignored = write(fd, sig == SIGTRAP ? "trap\n" : "chained\n", sig == SIGTRAP ? 5 : 8);
            (void)ignored; close(fd);
        }
        if (sig != SIGTRAP && !previousReturns) {
            struct sigaction action{};
            sigemptyset(&action.sa_mask);
            action.sa_handler = SIG_DFL;
            sigaction(sig, &action, nullptr);
            raise(sig);
        }
    }
#endif
#if defined(_MSC_VER)
    __declspec(noinline)
#else
    __attribute__((noinline))
#endif
    void crashTestStall() {
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }

    const char* argument(int argc, char** argv, const char* name) {
        for (int i = 1; i + 1 < argc; i++) {
            if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
        }
        return nullptr;
    }

    bool flag(int argc, char** argv, const char* name) {
        for (int i = 1; i < argc; i++) {
            if (std::strcmp(argv[i], name) == 0) return true;
        }
        return false;
    }

    void log(const std::string& text) {
        Logger::write(LogLevel::Info, logCategoryEngine(), text, nullptr, LogFlagNoRateLimit);
    }

    void appendLine(const std::filesystem::path& file, const std::string& line) {
        std::ofstream out(file, std::ios::app | std::ios::binary);
        out << line << "\n";
    }

}

using namespace je_crash_test;

int main(int argc, char** argv) {
    const char* mode = argument(argc, argv, "--mode");
    const char* out = argument(argc, argv, "--out");
    if (!mode || !out) {
        std::fprintf(stderr, "jasperengine-crash-test --mode <mode> --out DIR [--decline] [--abandon]\n");
        return 2;
    }
    const std::filesystem::path root(out);
    std::filesystem::create_directories(root);

    Logger::Config logging;
    logging.asyncDrain = false;
    Logger::init(logging);

    diag::crash::Config config;
    config.product = "jasperengine-crash-test";
    config.root = root;
    config.version = "test";
    config.backend = "none";
#if !defined(_WIN32)
    if (std::strcmp(mode, "chain") == 0 || std::strcmp(mode, "chain_return") == 0 ||
            std::strcmp(mode, "chain_after_return") == 0 || std::strncmp(mode, "recursive_chain", 15) == 0) {
        previousReturns = std::strcmp(mode, "recursive_chain_return") == 0;
        const std::string file = (root / "sentinel").string();
        std::memcpy(sentinelPath, file.c_str(), file.size() + 1);
        struct sigaction sentinel{};
        sigemptyset(&sentinel.sa_mask);
        sentinel.sa_sigaction = previousHandler;
        sentinel.sa_flags = SA_SIGINFO;
        sigaction((std::strcmp(mode, "chain") == 0 || std::strncmp(mode, "recursive_chain", 15) == 0) ? SIGSEGV : SIGTRAP, &sentinel, nullptr);
        if (std::strcmp(mode, "chain_after_return") == 0) sigaction(SIGSEGV, &sentinel, nullptr);
    }
#endif
#if defined(_WIN32)
    if (std::strcmp(mode, "recursive_abort") == 0) {
        // Recursive faults must bypass a previous abort callback that cannot make progress.
        std::signal(SIGABRT, [](int) { Sleep(10000); });
    }
#endif
    diag::crash::install(config);
    log(std::string("crash test mode ") + mode);

    const std::string name = mode;
#if !defined(_WIN32)
    if (name == "bus") {
        log("last line before the crash");
        raise(SIGBUS);
        return 1;
    }
    if (name == "chain_after_return") {
        log("last line before the crash");
        raise(SIGTRAP); // The saved handler returns after a complete engine report.
        raise(SIGSEGV); // A later fault must still reach its own saved handler.
        return 1;
    }
    if (name == "chain_return") {
        raise(SIGTRAP);
        diag::crash::shutdown();
        Logger::shutdown();
        return 0;
    }
#endif
    if (name == "concurrent" || name == "recursive" || name == "recursive_chain" || name == "recursive_chain_return" || name == "recursive_abort") {
        log("last line before the crash");
        recursiveFault = name != "concurrent";
#if defined(_WIN32)
        recursiveAbort = name == "recursive_abort";
#endif
        diag::crash::detail::state().beforeReportForTesting = pauseReport;
        auto first = thread::spawnNamed("First fault", [] { diag::crash::debugCrash("segv"); });
        if (!recursiveFault) {
            while (!reportEntered.load(std::memory_order_acquire)) std::this_thread::yield();
            auto second = thread::spawnNamed("Second fault", [] { diag::crash::debugCrash("segv"); });
            second.join();
        }
        first.join();
        return 1;
    }
#if defined(_WIN32)
    if (name == "cached_thread_name") {
        log("last line before the crash");
        auto worker = thread::spawnNamed("Cached fault", [] {
            // prepareThread already captured the name. A live lookup from the writer
            // would see this newer name (and could allocate behind a held heap lock).
            using SetDescription = HRESULT (WINAPI*)(HANDLE, PCWSTR);
            auto setDescription = reinterpret_cast<SetDescription>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
            if (setDescription) setDescription(GetCurrentThread(), L"Changed after preparation");
            diag::crash::debugCrash("segv");
        });
        worker.join();
        return 1;
    }
#endif
    if (name == "chain") { log("last line before the crash"); diag::crash::debugCrash("segv"); }
    if (name == "worker_stack_overflow") {
        log("last line before the crash");
        auto worker = thread::spawnNamed("Overflow worker", [] { diag::crash::debugCrash("stack_overflow"); });
        worker.join();
        return 1;
    }
    if (name == "guard_recover") {
#if !defined(_WIN32)
        CrashInfo info{};
        if (callWithCrashGuard([] { raise(SIGSEGV); }, &info)) return 1;
        std::ofstream(root / "ring.txt", std::ios::binary) << diag::crash::ringContents();
        diag::crash::shutdown();
        Logger::shutdown();
        return 0;
#endif
    }
    if (name == "guard_worker") {
        CrashInfo info{};
        callWithCrashGuard([] {
            auto worker = thread::spawnNamed("Guard worker", [] { diag::crash::debugCrash("segv"); });
            worker.join();
        }, &info);
        return 1;
    }
    if (diag::crash::isDebugCrashMode(name) && name != "hang") {
        log("last line before the crash");
        diag::crash::debugCrash(name);
        return 1;
    }
    if (name == "hook") {
        const bool decline = flag(argc, argv, "--decline");
        diag::crash::setReportHook([root, decline](const diag::crash::ReportInfo& report) {
            appendLine(root / "hook-calls.txt", report.id + " " + report.kind);
            return !decline;
        });
    } else if (name == "session") {
        if (flag(argc, argv, "--abandon")) {
            Logger::shutdown();
            std::_Exit(3);
        }
    } else if (name == "list") {
        std::ofstream file(root / "reports.txt", std::ios::binary);
        file << "abnormal " << (diag::crash::lastSessionEndedAbnormally() ? 1 : 0) << "\n";
        const auto last = diag::crash::lastSessionReport();
        file << "last " << (last ? last->id : std::string("-")) << "\n";
        for (const diag::crash::ReportInfo& report : diag::crash::listReports()) {
            file << report.id << " " << report.kind << " " << (report.handled ? 1 : 0) << "\n";
        }
    } else if (name == "where") {
        std::ofstream(root / "where.txt", std::ios::binary) << diag::crash::reportsDirectory().string() << "\n";
    } else if (name == "ring") {
        for (int i = 1; i <= 300; i++) log("ring line " + std::to_string(i));
        std::ofstream(root / "ring.txt", std::ios::binary) << diag::crash::ringContents();
    } else if (name == "hang") {
        diag::WatchdogConfig watch;
        watch.thresholdMs = 200;
        watch.pollMs = 50;
        watch.product = "jasperengine-crash-test";
        diag::watchdog::start(watch);
        diag::heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        {
            diag::PhaseScope phase("crash-test:stall");
            crashTestStall();
        }
        diag::heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        diag::watchdog::stop();
    } else {
        std::fprintf(stderr, "jasperengine-crash-test: unknown mode '%s'\n", mode);
        return 2;
    }
    diag::crash::shutdown();
    Logger::shutdown();
    return 0;
}
