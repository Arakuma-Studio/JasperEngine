// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The arming, re-report, shutdown-window and job-system cases are ported from OpenEngine's
// Tests/Editor/MainThreadHangWatchdogTests.cpp, Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License. See NOTICE.md.

#include "diag/HangWatchdog.h"
#include "jobs/JobSystem.h"

#include <gtest/gtest.h>

#include <cstdlib>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::diag;

namespace {

    constexpr uint64_t kMs = 1000000;

    // A watchdog without its thread (pollMs 0) on a clock the test moves
    class ManualWatchdog {
    public:
        explicit ManualWatchdog(uint32_t thresholdMs = 200, uint32_t shutdownMs = 1000) {
            WatchdogConfig config;
            config.thresholdMs = thresholdMs;
            config.shutdownMs = shutdownMs;
            config.pollMs = 0;
            config.product = "HangWatchdogTest";
            config.clock = [this]() { return now.load(); };
            config.onReport = [this](const HangReport& report) {
                std::lock_guard<std::mutex> lock(mutex);
                (report.recovered ? recoveries : reports).push_back(report);
            };
            watchdog::start(config);
        }
        ~ManualWatchdog() { watchdog::stop(); }

        void at(uint64_t ms) {
            now.store(ms * kMs);
            watchdog::poll();
        }

        std::vector<HangReport> reportsSoFar() {
            std::lock_guard<std::mutex> lock(mutex);
            return reports;
        }
        size_t recoveredCount() {
            std::lock_guard<std::mutex> lock(mutex);
            return recoveries.size();
        }

    private:
        std::atomic<uint64_t> now{0};
        std::mutex mutex;
        std::vector<HangReport> reports;
        std::vector<HangReport> recoveries;
    };

    void setEnvironment(const char* name, const char* value) {
#if defined(_WIN32)
        _putenv_s(name, value ? value : "");
#else
        if (value) ::setenv(name, value, 1);
        else ::unsetenv(name);
#endif
    }

    void stallFunctionOfTheTest(int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

}

TEST(HangWatchdog, InjectedStallReports) {
    std::mutex mutex;
    std::vector<HangReport> reports;
    size_t recoveries = 0;
    WatchdogConfig config;
    config.thresholdMs = 200;
    config.pollMs = 20;
    config.onReport = [&](const HangReport& report) {
        std::lock_guard<std::mutex> lock(mutex);
        if (report.recovered) recoveries++;
        else reports.push_back(report);
    };
    watchdog::start(config);
    for (int i = 0; i < 4; i++) {
        heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    bool sawUnresponsive = false;
    std::thread asker([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(450));
        sawUnresponsive = !watchdog::health().responsive;
    });
    {
        PhaseScope phase("test:stall");
        stallFunctionOfTheTest(700);
    }
    asker.join();
    heartbeat();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    watchdog::stop();

    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_GE(reports[0].stalledMs, 200u);
    EXPECT_EQ(reports[0].phase, "test:stall");
    EXPECT_NE(reports[0].text.find("phase: test:stall"), std::string::npos);
    EXPECT_NE(reports[0].text.find("stack: "), std::string::npos);
    EXPECT_EQ(recoveries, 1u);
    EXPECT_TRUE(sawUnresponsive) << "health() answers from another thread while the main thread stalls";
}

TEST(HangWatchdog, SteadyBeatsNeverReport) {
    std::atomic<int> reports{0};
    WatchdogConfig config;
    config.thresholdMs = 200;
    config.pollMs = 20;
    config.onReport = [&](const HangReport&) { reports++; };
    watchdog::start(config);
    for (int i = 0; i < 18; i++) {
        heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    watchdog::stop();
    EXPECT_EQ(reports.load(), 0);
}

TEST(HangWatchdog, NotArmedBeforeFirstBeat) {
    ManualWatchdog dog;
    dog.at(0);
    dog.at(10000);
    EXPECT_TRUE(dog.reportsSoFar().empty()) << "startup without a heartbeat never reports";
    heartbeat();
    dog.at(10010);
    dog.at(10500);
    EXPECT_EQ(dog.reportsSoFar().size(), 1u);
}

TEST(HangWatchdog, ReReportsEvery5s) {
    ManualWatchdog dog;
    heartbeat();
    dog.at(1000);
    dog.at(1300);
    ASSERT_EQ(dog.reportsSoFar().size(), 1u);
    dog.at(2000);
    dog.at(6290);
    EXPECT_EQ(dog.reportsSoFar().size(), 1u) << "not again within 5 s";
    dog.at(6310);
    ASSERT_EQ(dog.reportsSoFar().size(), 2u);
    EXPECT_GE(dog.reportsSoFar()[1].stalledMs, 5300u);
    heartbeat();
    dog.at(7000);
    EXPECT_EQ(dog.recoveredCount(), 1u) << "one recovered line per stall";
    EXPECT_EQ(dog.reportsSoFar()[0].index, 1u);
    EXPECT_EQ(dog.reportsSoFar()[1].index, 2u);
}

TEST(HangWatchdog, BusyScopeRaisesBudget) {
    ManualWatchdog dog;
    heartbeat();
    dog.at(0);
    {
        BusyScope scope("Importing model.glb", 2000);
        dog.at(700);
        EXPECT_TRUE(dog.reportsSoFar().empty()) << "inside the scope's budget";
        dog.at(2500);
        ASSERT_EQ(dog.reportsSoFar().size(), 1u);
        const HangReport report = dog.reportsSoFar()[0];
        ASSERT_EQ(report.busyScopes.size(), 1u);
        EXPECT_EQ(report.busyScopes[0], "Importing model.glb");
        EXPECT_EQ(report.thresholdMs, 2000u);
        EXPECT_NE(report.text.find("busy_scopes: Importing model.glb"), std::string::npos);
    }
}

TEST(HangWatchdog, NestedScopesListedInnermostLast) {
    ManualWatchdog dog;
    heartbeat();
    dog.at(0);
    BusyScope outer("Loading project", 1000);
    BusyScope inner("Importing model.glb", 500);
    dog.at(1200);
    ASSERT_EQ(dog.reportsSoFar().size(), 1u);
    const std::vector<std::string> scopes = dog.reportsSoFar()[0].busyScopes;
    ASSERT_EQ(scopes.size(), 2u);
    EXPECT_EQ(scopes[0], "Loading project");
    EXPECT_EQ(scopes[1], "Importing model.glb");
    EXPECT_EQ(dog.reportsSoFar()[0].thresholdMs, 1000u) << "the largest budget of the stack";
}

TEST(HangWatchdog, BusyScopeNeverSilences) {
    ManualWatchdog dog;
    heartbeat();
    dog.at(0);
    BusyScope scope("Importing model.glb");
    dog.at(119000);
    EXPECT_TRUE(dog.reportsSoFar().empty());
    dog.at(121000);
    EXPECT_EQ(dog.reportsSoFar().size(), 1u) << "a wedged import still reports past its 120 s budget";
}

TEST(HangWatchdog, UnlimitedScopeDoesNotReport) {
    ManualWatchdog dog;
    heartbeat();
    dog.at(0);
    BusyScope dialog("Open File dialog", 0);
    dog.at(600000);
    EXPECT_TRUE(dog.reportsSoFar().empty());
}

TEST(HangWatchdog, ShutdownWindowReports) {
    ManualWatchdog dog(200, 1000);
    heartbeat();
    dog.at(0);
    watchdog::beginShutdownWatch();
    dog.at(10);
    dog.at(800);
    EXPECT_TRUE(dog.reportsSoFar().empty()) << "within the shutdown window";
    dog.at(1200);
    ASSERT_EQ(dog.reportsSoFar().size(), 1u);
    EXPECT_TRUE(dog.reportsSoFar()[0].shutdown);
    EXPECT_EQ(dog.reportsSoFar()[0].phase, "shutdown");
}

TEST(HangWatchdog, HealthyTeardownSilent) {
    {
        ManualWatchdog dog(200, 1000);
        heartbeat();
        dog.at(0);
        watchdog::beginShutdownWatch();
        dog.at(10);
        dog.at(400);
        EXPECT_TRUE(dog.reportsSoFar().empty());
    }
    EXPECT_FALSE(watchdog::isRunning());
}

TEST(HangWatchdog, JobSystemNotReadAfterBeginShutdownWatch) {
    ManualWatchdog dog(200, 1000);
    heartbeat();
    dog.at(0);
    watchdog::beginShutdownWatch();
    dog.at(10);
    dog.at(1500);
    ASSERT_EQ(dog.reportsSoFar().size(), 1u);
    EXPECT_NE(dog.reportsSoFar()[0].text.find("job system: not read"), std::string::npos);
}

TEST(HangWatchdog, ReportsJobOccupancy) {
    jobs::Config config;
    config.workers = 2;
    jobs::JobSystem system(config);
    std::atomic<bool> release{false};
    std::atomic<int> started{0};
    auto block = [&]() {
        started++;
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    jobs::SubmitOptions a;
    a.name = "occupancy job A";
    jobs::SubmitOptions b;
    b.name = "occupancy job B";
    auto first = system.submit(block, a);
    auto second = system.submit(block, b);
    for (int i = 0; i < 500 && started.load() < 2; i++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_EQ(started.load(), 2);

    std::string text;
    {
        ManualWatchdog dog;
        heartbeat();
        dog.at(0);
        dog.at(500);
        ASSERT_EQ(dog.reportsSoFar().size(), 1u);
        text = dog.reportsSoFar()[0].text;
    }
    release = true;
    first.wait();
    second.wait();
    EXPECT_NE(text.find("occupancy job A for "), std::string::npos) << text;
    EXPECT_NE(text.find("occupancy job B for "), std::string::npos) << text;
}

TEST(HangWatchdog, LuaPhaseIsLocatedAndInterrupted) {
    ManualWatchdog dog;
    std::atomic<int> located{0};
    std::atomic<uint64_t> interruptedAfter{0};
    watchdog::LuaHooks hooks;
    hooks.locate = [&]() {
        located++;
        watchdog::setContext(ContextSlot::Lua, "player.lua:12 update");
    };
    hooks.interrupt = [&](uint64_t stalledMs) { interruptedAfter = stalledMs; };
    watchdog::setLuaHooks(hooks);
    watchdog::setBreakLuaAfterMs(1000);
    heartbeat();
    dog.at(0);
    {
        PhaseScope phase("lua:update");
        dog.at(300);
        ASSERT_EQ(dog.reportsSoFar().size(), 1u);
        EXPECT_NE(dog.reportsSoFar()[0].text.find("lua: player.lua:12 update"), std::string::npos);
        EXPECT_EQ(interruptedAfter.load(), 0u);
        dog.at(1100);
        EXPECT_GE(interruptedAfter.load(), 1000u);
        dog.at(1500);
    }
    watchdog::setBreakLuaAfterMs(0);
    watchdog::setContext(ContextSlot::Lua, "");
    EXPECT_EQ(located.load(), 1) << "once per stall";
}

TEST(HangWatchdog, PausedWatchdogDoesNotReport) {
    ManualWatchdog dog;
    heartbeat();
    dog.at(0);
    watchdog::setPaused(true);
    dog.at(5000);
    EXPECT_TRUE(dog.reportsSoFar().empty()) << "a backgrounded game expects no heartbeat";
    watchdog::setPaused(false);
    dog.at(6000);
    EXPECT_TRUE(dog.reportsSoFar().empty()) << "re-armed only by the next beat";
    heartbeat();
    dog.at(6010);
    dog.at(6500);
    EXPECT_EQ(dog.reportsSoFar().size(), 1u);
}

TEST(HangWatchdog, RecoveryLeavesANotice) {
    ManualWatchdog dog;
    watchdog::takeNotices();
    heartbeat();
    dog.at(0);
    {
        BusyScope scope("Saving scene", 300);
        dog.at(400);
    }
    // the scope's end beats
    dog.at(450);
    const std::vector<HangNotice> notices = watchdog::takeNotices();
    ASSERT_EQ(notices.size(), 1u);
    EXPECT_EQ(notices[0].label, "Saving scene");
    EXPECT_GE(notices[0].stalledMs, 400u);
    EXPECT_TRUE(watchdog::takeNotices().empty());
}

TEST(HangWatchdog, DisabledByEnvironmentStartsNoThread) {
    setEnvironment("JASPERENGINE_HANG_WATCHDOG", "0");
    WatchdogConfig config;
    watchdog::start(config);
    const bool running = watchdog::isRunning();
    setEnvironment("JASPERENGINE_HANG_WATCHDOG", nullptr);
    watchdog::stop();
    EXPECT_FALSE(running);
    EXPECT_FALSE(watchdog::health().enabled);
}
