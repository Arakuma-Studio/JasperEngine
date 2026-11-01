// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The log's buffer, listeners and thread rules, its Lua API, the debug overlay's commands and
// stats, and what the overlay costs when it is off (feature debug-overlay, spec 10, tests 1-6,
// 12, 13 and the off-cost targets of 9). The overlay's document (GPU) is tested by
// engine/tests/htmlui/TestDebugOverlay.cpp.

#include <gtest/gtest.h>

#include "ScriptDataTestSupport.h"

#include "Engine.h"
#include "Input.h"
#include "Log.h"
#include "debug/DebugOverlay.h"
#include "debug/DebugStats.h"
#include "script/LuaBinding.h"
#include "util/FunctionSubscribe.h"

#include "lua.hpp"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;

namespace {

    // a System that keeps the platform log quiet (the cost test logs 10,000 lines)
    class QuietSystem : public scriptdata::TestSystem {
    public:
        std::atomic<int> lines{0};
        void platformLog(const int, const char*, va_list) override { lines++; }
    };

    class LogListenerTest : public ::testing::Test {
    protected:
        QuietSystem system;
        System* previous = nullptr;
        lua_State* L = nullptr;
        size_t bufferSize = 0;
        LogStackTraces stackTraces = LogStackTraces::Warnings;

        void SetUp() override {
            L = scriptdata::engineLua();
            ASSERT_NE(L, nullptr);
            system.root = (std::filesystem::temp_directory_path() / ("jasper-log-test-" + std::to_string(::getpid()))).string();
            previous = SystemTestAccess::swap(&system);
            bufferSize = Log::getBufferSize();
            stackTraces = Log::getStackTraces();
            Log::dispatchPending();
            Log::clearEntries();
            Log::setBufferSize(500);
        }

        void TearDown() override {
            Log::onMessageReceived.clear();
            Log::setBufferSize(bufferSize);
            Log::setStackTraces(stackTraces);
            Log::clearEntries();
            DebugOverlay::shutdown();
            SystemTestAccess::swap(previous);
            lua_settop(L, 0);
        }

        std::string run(const std::string& code, std::vector<std::string>* results = nullptr) {
            const int top = lua_gettop(L);
            if (luaL_loadbuffer(L, code.data(), code.size(), "=test") != LUA_OK || lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
                const char* message = lua_tostring(L, -1);
                std::string error = message ? message : "(no message)";
                lua_settop(L, top);
                return error;
            }
            if (results) {
                results->clear();
                for (int i = top + 1; i <= lua_gettop(L); i++) {
                    size_t length = 0;
                    const char* text = luaL_tolstring(L, i, &length);
                    results->push_back(std::string(text, length));
                    lua_pop(L, 1);
                }
            }
            lua_settop(L, top);
            return "";
        }
    };

}

// 1. a bounded ring: the newest lines, ids increasing, filtered by level and count
TEST_F(LogListenerTest, RingBufferKeepsTheNewestLines) {
    Log::setBufferSize(3);
    Log::print("one");
    Log::warn("two");
    Log::error("three");
    Log::warn("four");
    std::vector<LogEntry> entries = Log::getEntries();
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].message, "two");
    EXPECT_EQ(entries[1].message, "three");
    EXPECT_EQ(entries[2].message, "four");
    EXPECT_LT(entries[0].id, entries[1].id);
    EXPECT_LT(entries[1].id, entries[2].id);
    EXPECT_EQ(entries[2].level, LogLevel::Warn);
    EXPECT_EQ(Log::getFirstBufferedId(), entries[0].id);

    const std::vector<LogEntry> problems = Log::getEntries(2, logLevelBit(LogLevel::Warn) | logLevelBit(LogLevel::Error));
    ASSERT_EQ(problems.size(), 2u);
    EXPECT_EQ(problems[0].message, "three");
    EXPECT_EQ(problems[1].message, "four");
    // counts are of every line since the clear, buffered or not
    EXPECT_EQ(Log::getCount(LogLevel::Warn), 2u);
    EXPECT_EQ(Log::getCount(LogLevel::Info), 1u);

    Log::setBufferSize(0);
    Log::print("not kept");
    EXPECT_TRUE(Log::getEntries().empty());
    EXPECT_EQ(Log::getFirstBufferedId(), Log::getLastId() + 1);
}

// 2. a Lua error's traceback is split off the message
TEST_F(LogListenerTest, SplitsTheStackTrace) {
    Log::error("Lua Error: x\nstack traceback:\n\tfoo.lua:1: in main chunk");
    const std::vector<LogEntry> entries = Log::getEntries(1);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].message, "Lua Error: x");
    EXPECT_EQ(entries[0].stackTrace, "stack traceback:\n\tfoo.lua:1: in main chunk");
    EXPECT_TRUE(entries[0].fromLua);

    Log::print("no trace here");
    EXPECT_EQ(Log::getEntries(1)[0].stackTrace, "");
    EXPECT_FALSE(Log::getEntries(1)[0].fromLua);
}

// 3. main-thread listeners at once; a line logged inside a listener is recorded, not dispatched
TEST_F(LogListenerTest, MainThreadListenerIsSynchronousWithoutRecursion) {
    std::vector<std::string> seen;
    const LogListenerId id = Log::addListener([&seen](const LogEntry& entry) {
        seen.push_back(entry.message);
        Log::print("inside %s", entry.message.c_str());
    });
    Log::warn("hello");
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], "hello");
    const std::vector<LogEntry> entries = Log::getEntries();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[1].message, "inside hello");
    EXPECT_TRUE(entries[0].mainThread);

    Log::removeListener(id);
    Log::print("after");
    EXPECT_EQ(seen.size(), 1u);
}

// 4. a worker thread's lines: AnyThread listeners on that thread, MainThread ones at dispatchPending, in order
TEST_F(LogListenerTest, WorkerThreadLinesWaitForTheMainThread) {
    std::mutex mutex;
    std::vector<std::string> anyThread;
    std::vector<std::thread::id> anyThreadIds;
    std::vector<std::string> mainThread;
    const LogListenerId any = Log::addListener([&](const LogEntry& entry) {
        std::lock_guard<std::mutex> lock(mutex);
        anyThread.push_back(entry.message);
        anyThreadIds.push_back(std::this_thread::get_id());
    }, LogDelivery::AnyThread);
    const LogListenerId main = Log::addListener([&](const LogEntry& entry) {
        EXPECT_TRUE(Log::isMainThread());
        mainThread.push_back(entry.message);
    });

    std::thread::id workerId;
    std::thread worker([&workerId]() {
        workerId = std::this_thread::get_id();
        for (int i = 0; i < 100; i++)
            Log::print("line %d", i);
    });
    worker.join();

    ASSERT_EQ(anyThread.size(), 100u);
    for (const std::thread::id& id : anyThreadIds)
        EXPECT_EQ(id, workerId);
    EXPECT_TRUE(mainThread.empty());
    EXPECT_EQ(Log::getPendingCount(), 100u);
    EXPECT_FALSE(Log::getEntries(1)[0].mainThread);

    Log::dispatchPending();
    ASSERT_EQ(mainThread.size(), 100u);
    for (int i = 0; i < 100; i++)
        EXPECT_EQ(mainThread[i], "line " + std::to_string(i));
    EXPECT_EQ(Log::getPendingCount(), 0u);

    Log::removeListener(any);
    Log::removeListener(main);
}

// a listener removed while a worker line waits does not get it
TEST_F(LogListenerTest, RemovedListenerStopsAtOnce) {
    int calls = 0;
    const LogListenerId id = Log::addListener([&calls](const LogEntry&) { calls++; });
    std::thread([]() { Log::print("from a worker"); }).join();
    Log::removeListener(id);
    Log::dispatchPending();
    EXPECT_EQ(calls, 0);
}

// removeListener waits for a call running on another thread: once it returns, what the
// listener uses can go. Workers log while the main thread adds and removes listeners.
TEST_F(LogListenerTest, RemoveListenerWaitsForRunningCalls) {
    struct Probe {
        std::atomic<bool> alive{true};
    };
    static std::atomic<int> callsAfterRemoval{0};
    static std::atomic<int> calls{0};
    callsAfterRemoval = 0;
    calls = 0;
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int w = 0; w < 4; w++) {
        workers.emplace_back([&stop, w]() {
            int i = 0;
            while (!stop.load())
                Log::print("worker %d line %d", w, i++);
        });
    }
    for (int round = 0; round < 200; round++) {
        Probe* probe = new Probe();
        const LogListenerId any = Log::addListener([probe](const LogEntry&) {
            calls++;
            if (!probe->alive.load()) callsAfterRemoval++;
            // a slow listener: the removal comes while calls run
            std::this_thread::sleep_for(std::chrono::microseconds(20));
            if (!probe->alive.load()) callsAfterRemoval++;
        }, LogDelivery::AnyThread);
        const LogListenerId main = Log::addListener([probe](const LogEntry&) {
            if (!probe->alive.load()) callsAfterRemoval++;
        });
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        Log::dispatchPending();
        Log::removeListener(any);
        Log::removeListener(main);
        probe->alive = false;
        delete probe;
    }
    stop = true;
    for (std::thread& worker : workers)
        worker.join();
    // the flood's report of the lines the queue dropped comes with the next worker line
    std::thread([]() { Log::print("flushed"); }).join();
    Log::dispatchPending();
    EXPECT_GT(calls.load(), 0);
    EXPECT_EQ(callsAfterRemoval.load(), 0);
}

// a listener that removes itself from inside its call does not wait for itself
TEST_F(LogListenerTest, ListenerRemovesItselfInsideItsCall) {
    std::atomic<int> calls{0};
    std::atomic<LogListenerId> id{0};
    id = Log::addListener([&calls, &id](const LogEntry&) {
        calls++;
        Log::removeListener(id.load());
    }, LogDelivery::AnyThread);
    std::thread([]() { Log::print("one"); Log::print("two"); }).join();
    EXPECT_EQ(calls.load(), 1);

    int mainCalls = 0;
    LogListenerId mainId = 0;
    mainId = Log::addListener([&mainCalls, &mainId](const LogEntry&) {
        mainCalls++;
        Log::removeListener(mainId);
    });
    Log::print("three");
    Log::print("four");
    EXPECT_EQ(mainCalls, 1);
    Log::dispatchPending();
}

// past 1,000 lines waiting for the main thread, worker lines are counted and reported once
TEST_F(LogListenerTest, PendingQueueOverflowIsReported) {
    std::vector<std::string> got;
    const LogListenerId id = Log::addListener([&got](const LogEntry& entry) { got.push_back(entry.message); });
    std::thread([]() {
        for (int i = 0; i < 1100; i++)
            Log::print("flood %d", i);
    }).join();
    EXPECT_EQ(Log::getPendingCount(), 1000u);
    Log::dispatchPending();
    ASSERT_EQ(got.size(), 1000u);
    EXPECT_EQ(got.front(), "flood 0");
    EXPECT_EQ(got.back(), "flood 999");

    // the next worker line has room again: the report comes with it
    got.clear();
    std::thread([]() { Log::print("after the flood"); }).join();
    Log::dispatchPending();
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], "after the flood");
    EXPECT_NE(got[1].find("100 lines logged by other threads were not given to the log listeners"), std::string::npos) << got[1];
    // every line is in the buffer and counted, dispatched or not
    EXPECT_EQ(Log::getCount(LogLevel::Info), 1101u);
    Log::removeListener(id);
}

// 5. Lua: RegisterEvent on Log.onMessageReceived, a traceback with warnings; the script released, no more calls
TEST_F(LogListenerTest, LuaListenerGetsMessageTraceAndLevel) {
    Log::setStackTraces(LogStackTraces::Warnings);
    ASSERT_EQ(run(R"(
        LogTest = {calls = {}}
        function LogTest:onLog(message, stackTrace, level)
            self.calls[#self.calls + 1] = {message = message, stackTrace = stackTrace, level = level}
        end
        RegisterEvent(LogTest, Log.onMessageReceived, "onLog")
        local function warnHere() Log.warn("w") end
        warnHere()
    )"), "");
    std::vector<std::string> results;
    ASSERT_EQ(run("local c = LogTest.calls[1] return #LogTest.calls, c.message, c.level == LogLevel.WARN, c.stackTrace", &results), "");
    ASSERT_EQ(results.size(), 4u);
    EXPECT_EQ(results[0], "1");
    EXPECT_EQ(results[1], "w");
    EXPECT_EQ(results[2], "true");
    EXPECT_EQ(results[3].rfind("stack traceback:", 0), 0u) << results[3];
    EXPECT_NE(results[3].find("warnHere"), std::string::npos) << results[3];
    EXPECT_TRUE(Log::getEntries(1)[0].fromLua);

    // print and Log.print: no trace with "warnings"; any values, tab-joined
    ASSERT_EQ(run(R"(Log.print("a", 1, true) print("b", nil))"), "");
    ASSERT_EQ(run("return #LogTest.calls, LogTest.calls[2].message, LogTest.calls[2].stackTrace, LogTest.calls[3].message", &results), "");
    EXPECT_EQ(results[0], "3");
    EXPECT_EQ(results[1], "a\t1\ttrue");
    EXPECT_EQ(results[2], "");
    EXPECT_EQ(results[3], "b\tnil");

    // the script released (LuaBinding::releaseLuaScriptInstance): its subscriptions go
    lua_getglobal(L, "LogTest");
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    LuaBinding::removeScriptSubscriptions(nullptr, ref);
    LuaBinding::releaseLuaRef(ref);
    Log::warn("after release");
    ASSERT_EQ(run("return #LogTest.calls", &results), "");
    EXPECT_EQ(results[0], "3");
}

// a Lua listener that raises is logged (not dispatched again) and stays subscribed
TEST_F(LogListenerTest, FailingLuaListenerStaysSubscribed) {
    ASSERT_EQ(run(R"(
        Failing = {count = 0}
        function Failing:onLog(message) self.count = self.count + 1 error("listener broke") end
        RegisterEvent(Failing, Log.onMessageReceived, "onLog")
    )"), "");
    Log::print("one");
    Log::print("two");
    std::vector<std::string> results;
    ASSERT_EQ(run("return Failing.count", &results), "");
    EXPECT_EQ(results[0], "2");
    size_t errors = 0;
    for (const LogEntry& entry : Log::getEntries()) {
        if (entry.level == LogLevel::Error && entry.message.find("listener broke") != std::string::npos) errors++;
    }
    EXPECT_EQ(errors, 2u);
}

// 6. Log.getEntries, Log.clear, Log.count, Log.bufferSize from Lua
TEST_F(LogListenerTest, LuaBufferApi) {
    Log::setStackTraces(LogStackTraces::Off);
    std::vector<std::string> results;
    ASSERT_EQ(run(R"(
        Log.print("info line")
        Log.warn("warn line")
        Log.error("error line")
        local errors = Log.getEntries(5, {LogLevel.ERROR})
        local all = Log.getEntries()
        local last = Log.getEntries(1)[1]
        return #errors, errors[1].message, #all, last.message, last.level, Log.count(LogLevel.WARN), type(last.id),
               type(last.time), type(last.frame), last.stackTrace
    )", &results), "");
    ASSERT_EQ(results.size(), 10u);
    EXPECT_EQ(results[0], "1");
    EXPECT_EQ(results[1], "error line");
    EXPECT_EQ(results[2], "3");
    EXPECT_EQ(results[3], "error line");
    EXPECT_EQ(results[4], "4");
    EXPECT_EQ(results[5], "1");
    EXPECT_EQ(results[6], "number");
    EXPECT_EQ(results[7], "number");
    EXPECT_EQ(results[8], "number");
    EXPECT_EQ(results[9], "");

    ASSERT_EQ(run("Log.clear() return #Log.getEntries(), Log.count(LogLevel.ERROR)", &results), "");
    EXPECT_EQ(results[0], "0");
    EXPECT_EQ(results[1], "0");
    ASSERT_EQ(run("Log.bufferSize = 0 Log.print('x') local n = #Log.getEntries() Log.bufferSize = 7 return n, Log.bufferSize", &results), "");
    EXPECT_EQ(results[0], "0");
    EXPECT_EQ(results[1], "7");
    EXPECT_NE(run("Log.getEntries(-1)"), "");
    EXPECT_NE(run("Log.getEntries(1, {99})"), "");
    // at most Log::maxBufferSize
    ASSERT_EQ(run("Log.bufferSize = 5000000 return Log.bufferSize", &results), "");
    EXPECT_EQ(results[0], std::to_string(Log::maxBufferSize));
}

// DebugOverlay.statsMode takes the three modes only; get_play_state never sees another
TEST_F(LogListenerTest, InvalidStatsModeIsRefused) {
    DebugOverlay::setStatsMode(DebugOverlaySettings::Stats::Fps);
    const std::string error = run("DebugOverlay.statsMode = 3");
    EXPECT_NE(error.find("DebugOverlay.statsMode takes DebugOverlay.STATS_OFF, STATS_FPS or STATS_FULL"), std::string::npos) << error;
    EXPECT_NE(run("DebugOverlay.statsMode = 1.5"), "");
    EXPECT_NE(run("DebugOverlay.statsMode = 'full'"), "");
    EXPECT_EQ(DebugOverlay::getStatsMode(), DebugOverlaySettings::Stats::Fps);
    std::vector<std::string> results;
    ASSERT_EQ(run("DebugOverlay.statsMode = DebugOverlay.STATS_FULL return DebugOverlay.statsMode", &results), "");
    EXPECT_EQ(results[0], "2");
    // from C++ too (a cast from a bad int)
    DebugOverlay::setStatsMode(static_cast<DebugOverlaySettings::Stats>(7));
    EXPECT_EQ(DebugOverlay::getStatsMode(), DebugOverlaySettings::Stats::Full);
    EXPECT_NE(Log::getEntries(1, logLevelBit(LogLevel::Warn))[0].message.find("7 is not a stats mode"), std::string::npos);
}

// settings changed while the game runs: what changed applies, the console and stats stay
TEST_F(LogListenerTest, UpdateSettingsKeepsTheState) {
    DebugOverlaySettings settings;
    settings.enabled = true;
    DebugOverlay::configure(settings);
    DebugOverlay::showConsole();
    DebugOverlay::setStatsMode(DebugOverlaySettings::Stats::Full);
    settings.openOnError = true;
    DebugOverlay::updateSettings(settings);
    EXPECT_TRUE(DebugOverlay::isConsoleVisible());
    EXPECT_EQ(DebugOverlay::getStatsMode(), DebugOverlaySettings::Stats::Full);
    EXPECT_TRUE(DebugOverlay::getSettings().openOnError);
    settings.stats = DebugOverlaySettings::Stats::Fps;
    DebugOverlay::updateSettings(settings);
    EXPECT_EQ(DebugOverlay::getStatsMode(), DebugOverlaySettings::Stats::Fps);
    settings.toggleKey = "f12";
    DebugOverlay::updateSettings(settings);
    DebugOverlay::handleKey(D_KEY_F12, 0, true, false);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible());
    settings.enabled = false;
    DebugOverlay::updateSettings(settings);
    EXPECT_FALSE(DebugOverlay::isEnabled());
}

// 12. commands: words to the function; unknown names; `lua` only with allowLua
TEST_F(LogListenerTest, ConsoleCommands) {
    std::vector<std::string> got;
    ASSERT_TRUE(DebugOverlay::addCommand("god", "god mode", [&got](const std::vector<std::string>& args) { got = args; }));
    DebugOverlay::executeCommand("god 1 two");
    EXPECT_EQ(got, (std::vector<std::string>{"1", "two"}));
    DebugOverlay::executeCommand("god \"a b\" 'c d'");
    EXPECT_EQ(got, (std::vector<std::string>{"a b", "c d"}));
    EXPECT_FALSE(DebugOverlay::addCommand("Bad Name", "", [](const std::vector<std::string>&) {}));
    EXPECT_FALSE(DebugOverlay::addCommand("help", "", [](const std::vector<std::string>&) {}));

    DebugOverlay::executeCommand("nope");
    EXPECT_NE(Log::getEntries(1)[0].message.find("unknown command 'nope' (type help)"), std::string::npos);

    DebugOverlaySettings settings;
    settings.allowLua = false;
    DebugOverlay::configure(settings);
    Log::clearEntries();
    DebugOverlay::executeCommand("lua 1+1");
    bool printed2 = false;
    for (const LogEntry& entry : Log::getEntries()) printed2 = printed2 || entry.message == "2";
    EXPECT_FALSE(printed2);
    EXPECT_NE(Log::getEntries(1)[0].message.find("Lua commands are off"), std::string::npos);

    settings.allowLua = true;
    DebugOverlay::configure(settings);
    DebugOverlay::executeCommand("lua 1+1");
    EXPECT_EQ(Log::getEntries(1)[0].message, "2");
    DebugOverlay::executeCommand("lua x = \"q r\"");
    std::vector<std::string> results;
    ASSERT_EQ(run("return x", &results), "");
    EXPECT_EQ(results[0], "q r");

    // built-ins
    DebugOverlay::executeCommand("timescale 0.5");
    EXPECT_FLOAT_EQ(Engine::getTimeScale(), 0.5f);
    Engine::setTimeScale(1.0f);
    DebugOverlay::executeCommand("stats full");
    EXPECT_EQ(DebugOverlay::getStatsMode(), DebugOverlaySettings::Stats::Full);
    DebugOverlay::executeCommand("help");
    EXPECT_NE(Log::getEntries(1)[0].message.find("god - god mode"), std::string::npos);
    DebugOverlay::removeCommand("god");
    EXPECT_FALSE(DebugOverlay::hasCommand("god"));
}

// Lua commands: fn(args); owned by the calling script's self and removed with it
TEST_F(LogListenerTest, LuaCommandsGoWithTheirScript) {
    ASSERT_EQ(run(R"(
        Cheats = {}
        function Cheats:init()
            DebugOverlay.addCommand("give", "give <item>", function(args) self.given = args[1] .. #args end)
        end
        Cheats:init()
    )"), "");
    DebugOverlay::executeCommand("give sword");
    std::vector<std::string> results;
    ASSERT_EQ(run("return Cheats.given", &results), "");
    EXPECT_EQ(results[0], "sword1");

    lua_getglobal(L, "Cheats");
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    LuaBinding::removeScriptSubscriptions(nullptr, ref);
    LuaBinding::releaseLuaRef(ref);
    EXPECT_FALSE(DebugOverlay::hasCommand("give"));

    // an error in a command is logged
    ASSERT_EQ(run(R"(DebugOverlay.addCommand("boom", "", function() error("kaboom") end, {}))"), "");
    DebugOverlay::executeCommand("boom");
    EXPECT_NE(Log::getEntries(1, logLevelBit(LogLevel::Error))[0].message.find("kaboom"), std::string::npos);
    Engine::clearAllSubscriptions(false);
    EXPECT_FALSE(DebugOverlay::hasCommand("boom"));
}

// 13. stats: the history ring, the snapshot, getStats from Lua
TEST_F(LogListenerTest, Stats) {
    DebugStats::reset();
    for (int i = 0; i < 300; i++)
        DebugStats::recordFrame(i % 10 == 0 ? 0.05 : 0.016);
    EXPECT_EQ(DebugStats::frameHistory().size(), DebugStats::historySize);
    EXPECT_EQ(DebugStats::recordedFrames(), 300u);
    const DebugStatsSnapshot s = DebugStats::sample();
    EXPECT_GT(s.fps, 0.0f);
    EXPECT_NEAR(s.frameMsMin, 16.0f, 0.01f);
    EXPECT_NEAR(s.frameMsMax, 50.0f, 0.01f);
    EXPECT_NEAR(s.frameMs1PercentLow, 50.0f, 0.01f);
    EXPECT_GE(s.frameMsAvg, s.frameMsMin);
    EXPECT_LE(s.frameMsAvg, s.frameMsMax);
    EXPECT_GT(s.luaMemoryBytes, 0u);
#if defined(__linux__)
    EXPECT_GT(s.processMemoryBytes, 0u);
#endif
    std::vector<std::string> results;
    ASSERT_EQ(run("local s = DebugOverlay.getStats() return s.fps > 0, s.drawCalls >= 0, s.luaMemoryBytes > 0, s.entities >= 0, "
                  "DebugOverlay.statsMode == DebugOverlay.STATS_OFF, type(Engine.isDevelopmentBuild)", &results), "");
    EXPECT_EQ(results, (std::vector<std::string>{"true", "true", "true", "true", "true", "boolean"}));
}

// 7 (no GPU) and the off-cost targets of 9: not enabled, no Scene; < 5 us a line, < 0.01 ms a frame
TEST_F(LogListenerTest, CostsNothingWhenOff) {
    DebugOverlay::configure(DebugOverlaySettings());
    ASSERT_FALSE(DebugOverlay::isEnabled());
    DebugOverlay::showConsole();   // warned, a no-op
    EXPECT_FALSE(DebugOverlay::isConsoleVisible());

    const auto lineStart = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; i++)
        Log::print("line %d of the cost test", i);
    const double lineUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - lineStart).count() / 10000.0;
    EXPECT_LT(lineUs, 5.0) << lineUs << " us per line";

    // what a frame and its input cost the overlay: the frame-time ring, the update check, the hooks
    const auto frameStart = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 1000; frame++) {
        DebugStats::recordFrame(1.0 / 60.0);
        if (DebugOverlay::needsUpdate()) DebugOverlay::update(1.0 / 60.0);
        DebugOverlay::handleKey(D_KEY_GRAVE_ACCENT, 0, true, false);
        DebugOverlay::handleTouch(0, 10.0f, 10.0f, 0);
        DebugOverlay::handleTouch(1, 10.0f, 10.0f, 0);
        DebugOverlay::handleTouch(2, 10.0f, 10.0f, 0);
        DebugOverlay::update(1.0);
        DebugOverlay::handleTouch(0, 10.0f, 10.0f, 2);
        DebugOverlay::handleTouch(1, 10.0f, 10.0f, 2);
        DebugOverlay::handleTouch(2, 10.0f, 10.0f, 2);
    }
    const double frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count() / 1000.0;
    EXPECT_LT(frameMs, 0.01) << frameMs << " ms per frame";
    EXPECT_FALSE(DebugOverlay::isConsoleVisible());
    EXPECT_EQ(DebugOverlay::getScene(), nullptr);
}
