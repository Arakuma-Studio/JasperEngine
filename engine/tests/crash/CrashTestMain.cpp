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
//   ring     logs 300 lines, writes the crash ring to DIR/ring.txt
//   hang     starts the watchdog (200 ms) and stalls the main thread for 700 ms in a phase

#include "diag/CrashHandler.h"
#include "diag/HangWatchdog.h"
#include "log/Logger.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace jasperengine;

namespace {

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
        std::ofstream out(file, std::ios::app);
        out << line << "\n";
    }

}

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
    diag::crash::install(config);
    log(std::string("crash test mode ") + mode);

    const std::string name = mode;
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
        std::ofstream file(root / "reports.txt");
        file << "abnormal " << (diag::crash::lastSessionEndedAbnormally() ? 1 : 0) << "\n";
        const auto last = diag::crash::lastSessionReport();
        file << "last " << (last ? last->id : std::string("-")) << "\n";
        for (const diag::crash::ReportInfo& report : diag::crash::listReports()) {
            file << report.id << " " << report.kind << " " << (report.handled ? 1 : 0) << "\n";
        }
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
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
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
