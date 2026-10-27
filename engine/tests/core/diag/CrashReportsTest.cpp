// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Report folders (CrashHandler.h): retention and classification in this process, and what
// needs an installed handler (sessions, the hook, the ring) in jasperengine-crash-test
// (engine/tests/crash/CrashTestMain.cpp), started per test.

#include "diag/CrashHandler.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

using namespace jasperengine;
using namespace jasperengine::test;
namespace fs = std::filesystem;
namespace crash = jasperengine::diag::crash;

namespace {

    // Runs jasperengine-crash-test; its exit code, or -1 when it did not end within 10 s
    int runCrashTest(const std::vector<std::string>& args) {
        const std::string binary = JASPER_TEST_CRASH_TEST_BINARY;
#if defined(_WIN32)
        std::string command = "\"" + binary + "\"";
        for (const std::string& arg : args) command += " \"" + arg + "\"";
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return -1;
        const DWORD waited = WaitForSingleObject(process.hProcess, 10000);
        DWORD code = 0;
        if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
        else GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return waited == WAIT_OBJECT_0 ? static_cast<int>(code) : -1;
#else
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(binary.c_str()));
        for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        pid_t pid = 0;
        if (posix_spawn(&pid, binary.c_str(), nullptr, nullptr, argv.data(), environ) != 0) return -1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (waitpid(pid, &status, WNOHANG) == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return -1;
#endif
    }

    // A report folder as a crashed session leaves it
    fs::path fakeReport(const TempDir& dir, const std::string& id, const std::string& file = "crash.txt") {
        const std::string text = "JasperEngine crash report v1\nproduct: Test Game\nversion: 1.0\nreason: SIGSEGV (invalid memory access)\n";
        dir.write("reports/" + id + "/" + file, text);
        return dir.path() / "reports" / id;
    }

    std::vector<std::string> lines(const std::string& text) {
        std::vector<std::string> out;
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty()) out.push_back(line);
        }
        return out;
    }

    std::string sessionId(int day) {
        char id[48];
        std::snprintf(id, sizeof(id), "202601%02dT120000Z-%d", day, 1000 + day);
        return id;
    }

}

TEST(CrashRetention, PrunesOldest) {
    TempDir dir;
    for (int day = 1; day <= 25; day++) fakeReport(dir, sessionId(day));
    crash::pruneReports(dir.path() / "reports", 20, 1ull << 40, 0);
    for (int day = 1; day <= 5; day++) EXPECT_FALSE(dir.exists("reports/" + sessionId(day))) << day;
    for (int day = 6; day <= 25; day++) EXPECT_TRUE(dir.exists("reports/" + sessionId(day))) << day;
}

TEST(CrashRetention, SizeCapSparesRecentUnhandledFolders) {
    TempDir dir;
    const std::string big(4096, 'x');
    for (int day = 1; day <= 4; day++) {
        fakeReport(dir, sessionId(day));
        dir.write("reports/" + sessionId(day) + "/crash.dmp", big);
    }
    dir.write("reports/" + sessionId(1) + "/handled", "");
    // the folders are from 2026-01: "recent" means within a very long window for this test
    crash::pruneReports(dir.path() / "reports", 20, 6000, 100000);
    EXPECT_TRUE(dir.exists("reports/" + sessionId(4)));
    EXPECT_TRUE(dir.exists("reports/" + sessionId(3))) << "unhandled and recent: kept over the size cap";
    EXPECT_TRUE(dir.exists("reports/" + sessionId(2)));
    EXPECT_FALSE(dir.exists("reports/" + sessionId(1))) << "handled: the size cap removes it";

    crash::pruneReports(dir.path() / "reports", 20, 6000, 0);
    EXPECT_TRUE(dir.exists("reports/" + sessionId(4)));
    EXPECT_FALSE(dir.exists("reports/" + sessionId(3))) << "a game has no recent-unhandled rule";
}

TEST(CrashReports, ClassifiesFolders) {
    TempDir dir;
    const fs::path crashed = fakeReport(dir, sessionId(1));
    const fs::path hung = fakeReport(dir, sessionId(2), "hang-001.txt");
    dir.write("reports/" + sessionId(2) + "/hang-001.txt",
              "JasperEngine hang report v1\nproduct: Test Game\nphase: loading\nbusy_scopes: Importing model.glb\n");
    dir.write("reports/" + sessionId(3) + "/session.json", "{\"product\": \"Test Game\", \"version\": \"2.0\"}\n");

    const auto crashReport = crash::readReport(crashed);
    ASSERT_TRUE(crashReport);
    EXPECT_EQ(crashReport->kind, "crash");
    EXPECT_EQ(crashReport->reason, "SIGSEGV (invalid memory access)");
    EXPECT_EQ(crashReport->product, "Test Game");
    EXPECT_FALSE(crashReport->handled);

    const auto hang = crash::readReport(hung);
    ASSERT_TRUE(hang);
    EXPECT_EQ(hang->kind, "hang");
    EXPECT_EQ(hang->reason, "not responding (Importing model.glb)");

    const auto gone = crash::readReport(dir.path() / "reports" / sessionId(3));
    ASSERT_TRUE(gone);
    EXPECT_EQ(gone->kind, "abnormal_exit");
    EXPECT_EQ(gone->version, "2.0");
    EXPECT_EQ(gone->time, "2026-01-03T12:00:00Z");
}

TEST(CrashHook, DeliversPendingOnce) {
    TempDir dir;
    fakeReport(dir, sessionId(1));
    ASSERT_EQ(runCrashTest({"--mode", "hook", "--out", dir.str()}), 0);
    std::vector<std::string> calls = lines(dir.read("hook-calls.txt"));
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0], sessionId(1) + " crash");
    EXPECT_TRUE(dir.exists("reports/" + sessionId(1) + "/handled")) << "the hook took it";

    ASSERT_EQ(runCrashTest({"--mode", "hook", "--out", dir.str()}), 0);
    EXPECT_EQ(lines(dir.read("hook-calls.txt")).size(), 1u) << "a handled report is not offered again";
}

TEST(CrashHook, DeclinedReportStaysPending) {
    TempDir dir;
    fakeReport(dir, sessionId(1));
    ASSERT_EQ(runCrashTest({"--mode", "hook", "--decline", "--out", dir.str()}), 0);
    ASSERT_EQ(runCrashTest({"--mode", "hook", "--decline", "--out", dir.str()}), 0);
    EXPECT_EQ(lines(dir.read("hook-calls.txt")).size(), 2u) << "offered again at the next start";
    EXPECT_FALSE(dir.exists("reports/" + sessionId(1) + "/handled"));
}

TEST(CrashSession, CleanExitLeavesNothing) {
    TempDir dir;
    ASSERT_EQ(runCrashTest({"--mode", "session", "--out", dir.str()}), 0);
    ASSERT_EQ(runCrashTest({"--mode", "list", "--out", dir.str()}), 0);
    const std::vector<std::string> report = lines(dir.read("reports.txt"));
    ASSERT_GE(report.size(), 2u);
    EXPECT_EQ(report[0], "abnormal 0");
    EXPECT_EQ(report[1], "last -");
    EXPECT_EQ(report.size(), 2u) << "no report folder for a clean session";
}

TEST(CrashSession, AbandonedSessionIsAbnormalExit) {
    TempDir dir;
    ASSERT_EQ(runCrashTest({"--mode", "session", "--abandon", "--out", dir.str()}), 3);
    ASSERT_EQ(runCrashTest({"--mode", "list", "--out", dir.str()}), 0);
    const std::vector<std::string> report = lines(dir.read("reports.txt"));
    ASSERT_EQ(report.size(), 3u) << dir.read("reports.txt");
    EXPECT_EQ(report[0], "abnormal 1");
    ASSERT_EQ(report[1].rfind("last ", 0), 0u);
    const std::string id = report[1].substr(5);
    EXPECT_EQ(report[2], id + " abnormal_exit 0");
    EXPECT_TRUE(dir.exists("reports/" + id + "/session.json"));
    EXPECT_FALSE(dir.exists("sessions/" + id.substr(id.find('-') + 1) + ".json")) << "the stale marker is consumed";
}

TEST(CrashSession, RelativeRootBecomesAbsolute) {
    TempDir dir;
    // the child runs in this process's working directory: name the root relative to it
    const fs::path relative = fs::relative(dir.path(), fs::current_path());
    ASSERT_TRUE(relative.is_relative());
    ASSERT_EQ(runCrashTest({"--mode", "where", "--out", relative.string()}), 0);
    std::string where = dir.read("where.txt");
    while (!where.empty() && (where.back() == '\n' || where.back() == '\r')) where.pop_back();
    const fs::path reports(where);
    EXPECT_TRUE(reports.is_absolute()) << where << ": crash.txt and the stderr line name the folder absolutely";
    EXPECT_TRUE(fs::equivalent(reports, dir.path() / "reports")) << where;
}

TEST(CrashRing, LogTailKeepsTheLastLines) {
    TempDir dir;
    ASSERT_EQ(runCrashTest({"--mode", "ring", "--out", dir.str()}), 0);
    const std::string ring = dir.read("ring.txt");
    EXPECT_NE(ring.find("ring line 300\n"), std::string::npos);
    EXPECT_NE(ring.find("crash test mode ring"), std::string::npos);
    EXPECT_NE(ring.find("INFO "), std::string::npos) << "each line carries its level";
}

TEST(CrashHang, WatchdogWritesAHangReportIntoTheSession) {
    TempDir dir;
    ASSERT_EQ(runCrashTest({"--mode", "hang", "--out", dir.str()}), 0);
    std::string text;
    for (const auto& entry : fs::recursive_directory_iterator(dir.path() / "reports")) {
        if (entry.path().filename() == "hang-001.txt") text = dir.read(fs::relative(entry.path(), dir.path()).string());
    }
    ASSERT_FALSE(text.empty()) << "no hang-001.txt";
    EXPECT_EQ(text.rfind("JasperEngine hang report v1\n", 0), 0u);
    EXPECT_NE(text.find("phase: crash-test:stall"), std::string::npos);
    EXPECT_NE(text.find("product: jasperengine-crash-test"), std::string::npos);
    EXPECT_NE(text.find("crashTestStall"), std::string::npos) << text;
}

TEST(CrashGuard, WorkerCrashReachesHandler) {
    TempDir dir;
    const int result = runCrashTest({"--mode", "guard_worker", "--out", dir.str()});
    EXPECT_NE(result, -1) << "crashing worker hung";
    EXPECT_NE(result, 0);
    bool reported = false;
    for (const auto& entry : fs::recursive_directory_iterator(dir.path())) {
        if (entry.path().filename() == "crash.txt") reported = true;
    }
    EXPECT_TRUE(reported) << "a worker crash during a Lua guard must reach the engine handler";
}

#if !defined(_WIN32)
TEST(CrashChain, PreviousHandlerMayReturn) {
    TempDir dir;
    EXPECT_EQ(runCrashTest({"--mode", "chain_return", "--out", dir.str()}), 0);
    EXPECT_TRUE(dir.exists("sentinel"));
}
TEST(CrashChain, PreviousHandlerRuns) {
    TempDir dir;
    EXPECT_NE(runCrashTest({"--mode", "chain", "--out", dir.str()}), -1);
    EXPECT_TRUE(dir.exists("sentinel"));
}
TEST(CrashGuard, RecoveredCrashOnlyAddsBreadcrumb) {
    TempDir dir;
    ASSERT_EQ(runCrashTest({"--mode", "guard_recover", "--out", dir.str()}), 0);
    EXPECT_NE(dir.read("ring.txt").find("recovered a guarded callback crash"), std::string::npos);
    EXPECT_TRUE(fs::is_empty(dir.path() / "reports"));
}
#endif
