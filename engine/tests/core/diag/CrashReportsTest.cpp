// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Report folders (CrashHandler.h): retention and classification in this process, and what
// needs an installed handler (sessions, the hook, the ring) in jasperengine-crash-test
// (engine/tests/crash/CrashTestMain.cpp), started per test.

#include "diag/CrashHandler.h"
#include "diag/UserReport.h"
#include "jobs/JobSystem.h"
#include "AllocationCounter.h"
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

namespace CrashReportsTest_cpp {

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

// ---- the Gameplay Journal beside a hang report, user reports (docs/specs/shipped-game-diagnostics.md)

namespace {

    // the file named `name` in the one report folder under the root that has it ("" when none)
    std::string findReportFile(const TempDir& dir, const std::string& name, fs::path* folder = nullptr) {
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(dir.path() / "reports", ec)) {
            if (entry.path().filename() == name) {
                if (folder) *folder = entry.path().parent_path();
                return dir.read(fs::relative(entry.path(), dir.path()).string());
            }
        }
        return {};
    }

    // a 1920x1080 frame of noise (the worst case for JPEG)
    std::vector<uint8_t> noiseFrame(int width, int height) {
        std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
        uint32_t state = 12345u;
        for (size_t i = 0; i < rgba.size(); i++) {
            state = state * 1664525u + 1013904223u;
            rgba[i] = static_cast<uint8_t>(state >> 24);
        }
        return rgba;
    }

    std::vector<uint8_t> captured;
    int capturedWidth = 0, capturedHeight = 0;
    // the stand-in readback's own time: R31 budgets the capture beyond it
    double readbackMs = 0.0;

    bool readCaptured(std::vector<uint8_t>& rgba, int& width, int& height, std::string&) {
        const auto start = std::chrono::steady_clock::now();
        rgba = captured;
        width = capturedWidth;
        height = capturedHeight;
        readbackMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return true;
    }

    // frames of the engine as far as a user report's capture goes: the main queue, then update
    bool pumpUntil(const bool& done) {
        for (int i = 0; i < 2000 && !done; i++) {
            jobs::drainMainQueue();
            diag::userreport::update();
            if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done;
    }

}

TEST(CrashHang, WatchdogWritesTheJournalBesideTheHangReport) {
    TempDir dir;
    ASSERT_EQ(runCrashTest({"--mode", "hang", "--journal", "--out", dir.str()}), 0);
    fs::path hangFolder, journalFolder;
    ASSERT_FALSE(findReportFile(dir, "hang-001.txt", &hangFolder).empty()) << "no hang-001.txt";
    const std::string journal = findReportFile(dir, "journal.txt", &journalFolder);
    ASSERT_FALSE(journal.empty()) << "no journal.txt";
    EXPECT_EQ(hangFolder, journalFolder) << "beside the hang report";
    EXPECT_EQ(journal.rfind("JasperEngine journal v1\n", 0), 0u) << journal;
    EXPECT_NE(journal.find(" test.two name=forest-2 seed=42\n"), std::string::npos) << journal;
}

TEST(CrashUser, EncodeLadderFitsTheCap) {
    const std::vector<uint8_t> noise = noiseFrame(1920, 1080);
    const diag::userreport::Encoded fitted = diag::userreport::encode(noise, 1920, 1080, 1280, 100u << 10);
    ASSERT_FALSE(fitted.jpeg.empty()) << fitted.error;
    EXPECT_LE(fitted.jpeg.size(), 100u << 10);
    EXPECT_EQ(static_cast<uint8_t>(fitted.jpeg[0]), 0xFF);
    EXPECT_EQ(static_cast<uint8_t>(fitted.jpeg[1]), 0xD8);
    const bool ladder = fitted.width == 1280 && fitted.height == 720 &&
        (fitted.quality == 80 || fitted.quality == 70 || fitted.quality == 60 || fitted.quality == 50 || fitted.quality == 40);
    const bool half = fitted.width == 640 && fitted.height == 360 && fitted.quality == 40;
    EXPECT_TRUE(ladder || half) << fitted.width << "x" << fitted.height << " q" << fitted.quality;

    // through the draft: nothing fits 1 KiB, so no screenshot and the reason
    captured = noise;
    capturedWidth = 1920;
    capturedHeight = 1080;
    bool called = false, ok = true;
    std::string error;
    diag::userreport::begin([&](bool shot, const std::string& why) {
        called = true;
        ok = shot;
        error = why;
    }, 1280, 1024);
    ASSERT_TRUE(diag::userreport::wantsCapture());
    diag::userreport::beforeCaptureFrame();
    diag::userreport::frameDrawn(&readCaptured);
    ASSERT_TRUE(pumpUntil(called)) << "the callback never ran";
    EXPECT_FALSE(ok);
    EXPECT_EQ(error, "screenshot_too_large");
    EXPECT_TRUE(diag::userreport::hasDraft());
    EXPECT_FALSE(diag::userreport::draftHasScreenshot());
    diag::userreport::reset();
}

TEST(CrashUser, CreateWritesAUserFolder) {
    TempDir dir;
    ASSERT_EQ(runCrashTest({"--mode", "user", "--journal", "--out", dir.str()}), 0);
    const std::vector<std::string> results = lines(dir.read("user-results.txt"));
    ASSERT_EQ(results.size(), 3u) << dir.read("user-results.txt");
    ASSERT_EQ(results[0].rfind("ok ", 0), 0u) << results[0];
    const std::string id = results[0].substr(3, results[0].find(' ', 3) - 3);
    EXPECT_EQ(id.substr(id.size() - 6), "-user1");
    EXPECT_EQ(results[0].substr(results[0].size() - 5), " user");
    EXPECT_EQ(results[1], "reason user report (gameplay): the door does not open");

    const fs::path folder = dir.path() / "reports" / id;
    const std::string user = dir.read("reports/" + id + "/user.txt");
    EXPECT_EQ(user.rfind("JasperEngine user report v1\n", 0), 0u) << user;
    for (const char* key : {"product: jasperengine-crash-test\n", "version: test\n", "build_id: ", "platform: ", "backend: none\n",
                            "session: ", "time: ", "category: gameplay\n", "build: nightly\n", "field.level: forest-2\n",
                            "field.try: 1\n", "note:\nthe door does not open\nsecond line\n"}) {
        EXPECT_NE(user.find(key), std::string::npos) << key << " in\n" << user;
    }
    const std::string device = dir.read("reports/" + id + "/device.json");
    for (const char* key : {"product", "version", "build_id", "platform", "backend", "os_version", "gpu", "gpu_driver", "device_tier",
                            "tier_reason", "gpu_quirks", "memory_mb", "cores", "screen", "width", "height", "dpi_scale",
                            "thermal_state", "low_memory_warnings", "uptime_s", "session"}) {
        EXPECT_NE(device.find("\"" + std::string(key) + "\""), std::string::npos) << key << " in\n" << device;
    }
    const std::string journal = dir.read("reports/" + id + "/journal.txt");
    EXPECT_NE(journal.find(" test.three hard=true\n"), std::string::npos) << journal;
    EXPECT_NE(dir.read("reports/" + id + "/log-tail.txt").find("crash test mode user"), std::string::npos);

    const auto report = crash::readReport(folder);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->kind, "user");
    EXPECT_EQ(report->reason, "user report (gameplay): the door does not open");
    EXPECT_EQ(report->category, "gameplay");
    EXPECT_EQ(report->product, "jasperengine-crash-test");
}

TEST(CrashUser, FourthPendingReportIsRefused) {
    TempDir dir;
    ASSERT_EQ(runCrashTest({"--mode", "user", "--count", "4", "--out", dir.str()}), 0);
    const std::vector<std::string> results = lines(dir.read("user-results.txt"));
    ASSERT_EQ(results.size(), 6u) << dir.read("user-results.txt");
    for (int i = 0; i < 3; i++) { EXPECT_EQ(results[i].rfind("ok ", 0), 0u) << results[i]; }
    EXPECT_EQ(results[3], "error too_many_pending: upload a report or Crash.markHandled(id) first");
    size_t folders = 0;
    for (const auto& entry : fs::directory_iterator(dir.path() / "reports")) folders += entry.is_directory() ? 1 : 0;
    EXPECT_EQ(folders, 3u) << "the refused one writes nothing";
}

TEST(CrashUser, UnsentReportIsOfferedNextStart) {
    TempDir dir;
    ASSERT_EQ(runCrashTest({"--mode", "user", "--out", dir.str()}), 0);
    const std::vector<std::string> results = lines(dir.read("user-results.txt"));
    ASSERT_EQ(results.size(), 3u);
    const std::string id = results[0].substr(3, results[0].find(' ', 3) - 3);
    EXPECT_EQ(results[2], "pending") << "this session's own user report waits for the next start";

    ASSERT_EQ(runCrashTest({"--mode", "hook", "--decline", "--out", dir.str()}), 0);
    const std::vector<std::string> calls = lines(dir.read("hook-calls.txt"));
    ASSERT_EQ(calls.size(), 1u) << dir.read("hook-calls.txt");
    EXPECT_EQ(calls[0], id + " user");
}

TEST(CrashUserBench, CreateReportWrites) {
    TempDir dir;
    ASSERT_EQ(runCrashTest({"--mode", "user_bench", "--journal", "--out", dir.str()}), 0);
    const double createMs = std::stod(dir.read("user-bench.txt"));

    // the capture's main-thread share beyond the readback, and the encode, at 1920x1080
    captured = noiseFrame(1920, 1080);
    for (size_t i = 0; i < captured.size(); i++) captured[i] = static_cast<uint8_t>((i / 4 % 1920) * 255 / 1919);
    capturedWidth = 1920;
    capturedHeight = 1080;
    bool called = false;
    diag::userreport::begin([&](bool, const std::string&) { called = true; });
    diag::userreport::beforeCaptureFrame();
    const auto captureStart = std::chrono::steady_clock::now();
    diag::userreport::frameDrawn(&readCaptured);
    // R31: the main thread's share beyond the readback's wait (here a copy of the frame), which
    // the capture's log line reports on its own
    const double captureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - captureStart).count() - readbackMs;
    ASSERT_TRUE(pumpUntil(called));
    diag::userreport::reset();
    const auto encodeStart = std::chrono::steady_clock::now();
    const diag::userreport::Encoded encoded = diag::userreport::encode(captured, 1920, 1080, 1280, 600u << 10);
    const double encodeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - encodeStart).count();
    EXPECT_EQ(encoded.width, 1280);

    std::printf("[bench] User report: createReport %.2f ms, capture main thread %.2f ms (+ the readback, %.2f ms), encode 1920x1080 -> 1280x720 %.1f ms\n",
                createMs, captureMs, readbackMs, encodeMs);
    const double slack = testsupport::perfSlack();
    if (slack > 0.0) {
        EXPECT_LE(createMs, 10.0 * slack) << "R14";
        EXPECT_LE(captureMs, 2.0 * slack) << "R31";
        EXPECT_LE(encodeMs, 60.0 * slack) << "R31";
    }
}

}  // namespace CrashReportsTest_cpp
