// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// je_log_crash_helper <log file> <main|sink>: the process CrashHandlerTest crashes. It logs
// 5,000 lines to the file through the logger's queue, then crashes: on the main thread
// (main), or inside a sink on the logger's drain thread (sink). The editor's crash handler
// writes its report to the same file.

#include "util/CrashHandler.h"
#include "log/LogSinks.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

using namespace jasperengine;

namespace {

    void crash() {
        volatile int* nothing = nullptr;
        *nothing = 1;
    }

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "je_log_crash_helper <log file> <main|sink>\n");
        return 2;
    }
    const bool inSink = std::strcmp(argv[2], "sink") == 0;
    Logger::Config config;
    config.queueBytes = 4 * 1024 * 1024;
    Logger::init(config);
    RotatingFileSink::Options file;
    file.path = argv[1];
    file.maxBytes = 64 * 1024 * 1024;
    Logger::addSink(std::make_unique<RotatingFileSink>(file));
    if (inSink) {
        Logger::addSink(std::make_unique<CallbackSink>("crasher", [](const LogRecord& record) {
            if (record.text == "crash now") crash();
        }));
    }
    editor::CrashHandler::install(argv[1]);

    // a burst the rate limit would hold back: these are what a crash must not lose
    for (int i = 1; i <= 5000; i++) {
        Logger::write(LogLevel::Info, logCategoryEditor(), "line " + std::to_string(i), nullptr, LogFlagNoRateLimit);
    }
    if (inSink) {
        Logger::write(LogLevel::Warn, logCategoryEditor(), "crash now");
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 1;
    }
    crash();
    return 1;
}
