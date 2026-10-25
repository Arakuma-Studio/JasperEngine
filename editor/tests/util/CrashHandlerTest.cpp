// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#if defined(_WIN32)
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <csignal>
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>
    extern char** environ;
#endif

using namespace jasperengine::test;

namespace {

    // Runs the helper; false when it did not end within the timeout (it is killed)
    bool runHelper(const std::string& logFile, const char* mode, std::chrono::seconds timeout) {
        const std::string helper = JASPER_TEST_LOG_CRASH_HELPER;
#if defined(_WIN32)
        std::string command = "\"" + helper + "\" \"" + logFile + "\" " + mode;
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return false;
        const DWORD waited = WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout.count() * 1000));
        if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return waited == WAIT_OBJECT_0;
#else
        pid_t pid = 0;
        char* argv[] = {const_cast<char*>(helper.c_str()), const_cast<char*>(logFile.c_str()), const_cast<char*>(mode), nullptr};
        if (posix_spawn(&pid, helper.c_str(), nullptr, nullptr, argv, environ) != 0) return false;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (waitpid(pid, &status, WNOHANG) == pid) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return false;
#endif
    }

}

TEST(LoggerCrashFlushTest, TailIsWrittenBeforeTheReport) {
    TempDir dir;
    const std::string log = dir.file("crash.log").string();
    ASSERT_TRUE(runHelper(log, "main", std::chrono::seconds(10))) << "the crashing helper hung";
    const std::string text = dir.read("crash.log");
    const size_t last = text.find("] line 5000\n");
    const size_t fatal = text.find("FATAL: ");
    ASSERT_NE(last, std::string::npos) << "the last line logged before the crash is missing";
    ASSERT_NE(fatal, std::string::npos) << "no crash report";
    EXPECT_LT(last, fatal) << "the log tail comes before the FATAL block";
    EXPECT_EQ(text.find("log tail incomplete"), std::string::npos);
}

TEST(LoggerCrashFlushTest, CrashInsideASinkReportsTheTailLost) {
    TempDir dir;
    const std::string log = dir.file("crash-sink.log").string();
    ASSERT_TRUE(runHelper(log, "sink", std::chrono::seconds(10))) << "the crashing helper hung";
    const std::string text = dir.read("crash-sink.log");
    EXPECT_NE(text.find("log tail incomplete: "), std::string::npos) << text.substr(text.size() > 2000 ? text.size() - 2000 : 0);
    EXPECT_NE(text.find("FATAL: "), std::string::npos);
}
