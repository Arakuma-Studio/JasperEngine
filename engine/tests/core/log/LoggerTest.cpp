// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "log/LogTestSupport.h"
#include "LogCapture.h"
#include "support/AllocationCounter.h"
#include "TempDir.h"

#include "Log.h"
#include "util/StringUtils.h"
#include "script/LuaBinding.h"
#include "lua.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <regex>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    JE_LOG_CATEGORY(logCategoryLoggerTest, "logger-test", LogLevel::Info)

    int sideEffects = 0;
    int sideEffect() {
        return ++sideEffects;
    }

}

TEST(LoggerTest, FilteredCallFormatsNothing) {
    LogCategory& category = logCategoryLoggerTest();
    Logger::setCategoryLevel(category, LogLevel::Warn);
    sideEffects = 0;
    testsupport::ThreadAllocationCount allocations;
    for (int i = 0; i < 1000000; i++) {
        JE_LOG_INFO(category, "never formatted %d", sideEffect());
    }
    EXPECT_EQ(sideEffects, 0) << "a filtered call evaluated its arguments";
    EXPECT_EQ(allocations.count(), 0u);
    Logger::setCategoryLevel(category, LogLevel::Info);
}

TEST(LoggerTest, CompiledOutLevelsAreNotRun) {
    sideEffects = 0;
    // JE_LOG_TRACE is below JE_LOG_MIN_COMPILED_LEVEL in every build
    JE_LOG_TRACE(logCategoryLoggerTest(), "%d", sideEffect());
    EXPECT_EQ(sideEffects, 0);
}

TEST(LoggerTest, MacroRecordsCarryTheirSource) {
    auto sink = recordingSink();
    JE_LOG_WARN(logCategoryLoggerTest(), "with a source %d", 7);
    const uint32_t line = __LINE__ - 1;
    Logger::flush();
    const auto found = sink->containing("with a source 7");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].level, LogLevel::Warn);
    EXPECT_EQ(found[0].category, "logger-test");
    EXPECT_NE(found[0].file.find("LoggerTest.cpp"), std::string::npos) << found[0].file;
    EXPECT_EQ(found[0].line, line);
}

// A Fatal reaches an Async sink from the calling thread, under the sink's lock with a deadline.
// (Under SANITIZE=thread, that lock taken through std::timed_mutex reports an unlock of an unlocked
// mutex: GCC 11's TSan does not intercept what try_lock_until calls.)
TEST(LoggerTest, FatalReachesAnAsyncSinkFromTheCallingThread) {
    auto sink = recordingSink();
    JE_LOG_FATAL(logCategoryLoggerTest(), "the end %d", 7);
    EXPECT_EQ(sink->containing("the end 7").size(), 1u) << "before any flush";
    Logger::flush();
    EXPECT_EQ(sink->containing("the end 7").size(), 1u);
}

// A burst of errors in one test: its summary comes when the second is over, in whichever test is
// running by then. A test's capture starts after it, and the rate limit starts anew for that test.
TEST(LoggerTest, ASummaryOfAnEarlierTestIsNotInTheNextTestsCapture) {
    const auto burst = []() { JE_LOG_ERROR(logCategoryLoggerTest(), "an earlier test's burst"); };
    for (int i = 0; i < 500; i++)
        burst();
    LogCapture::markTestStart();
    Logger::flushSummaries();
    const LogCapture capture = LogCapture::sinceTestStart();
    EXPECT_FALSE(capture.contains("similar messages")) << capture.describeErrors();
    EXPECT_TRUE(capture.errors().empty()) << capture.describeErrors();
    // its own errors still count: the first of a burst from here on goes through
    burst();
    Logger::flush();
    EXPECT_EQ(capture.count(D_LOG_ERROR, "an earlier test's burst"), 1u);
    // (what was logged on purpose is not this test's failure, nor a later one's)
    LogCapture::markTestStart();
}

TEST(LoggerTest, LegacyCallsMapToLevels) {
    // a game's default: Info (the test binary records everything)
    const LogLevel previous = Logger::categoryLevel(logCategoryEngine());
    Logger::setCategoryLevel(logCategoryEngine(), LogLevel::Info);
    auto sink = recordingSink();
    Log::print("legacy print");
    Log::verbose("legacy verbose");
    Log::warn("legacy warn");
    Log::error("legacy error");
    Logger::flush();
    ASSERT_EQ(sink->containing("legacy print").size(), 1u);
    EXPECT_EQ(sink->containing("legacy print")[0].level, LogLevel::Info);
    EXPECT_EQ(sink->containing("legacy print")[0].category, "engine");
    EXPECT_TRUE(sink->containing("legacy verbose").empty()) << "Trace is below the engine category's Info";
    EXPECT_EQ(sink->containing("legacy warn")[0].level, LogLevel::Warn);
    EXPECT_EQ(sink->containing("legacy error")[0].level, LogLevel::Error);
    EXPECT_TRUE(sink->containing("legacy error")[0].file.empty()) << "legacy calls carry no source";
    Logger::setCategoryLevel(logCategoryEngine(), previous);
}

TEST(LoggerTest, LongTextIsTruncated) {
    auto sink = recordingSink();
    const std::string text(LogRecord::MAX_TEXT_BYTES + 5000, 'x');
    Logger::write(LogLevel::Info, logCategoryLoggerTest(), text);
    Logger::flush();
    const auto found = sink->containing("[truncated ");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_LE(found[0].text.size(), LogRecord::MAX_TEXT_BYTES);
    EXPECT_TRUE(found[0].flags & LogFlagTruncated);
}

// The log is UTF-8 for every sink: bytes that are not (a child's output in another code page, a
// cut string) arrive repaired, one U+FFFD per bad byte, through write and the printf form alike
TEST(LoggerTest, InvalidUtf8ReachesSinksRepaired) {
    auto sink = recordingSink();
    Logger::write(LogLevel::Info, logCategoryLoggerTest(), "utf8 write \xFF\xC3 caf\xC3\xA9");
    Logger::writef(LogLevel::Info, logCategoryLoggerTest(), nullptr, "%s", "utf8 writef \xE2\x82 \xF0\x9F\x98\x80");
    Logger::flush();
    const auto written = sink->containing("utf8 write ");
    ASSERT_EQ(written.size(), 1u);
    EXPECT_EQ(written[0].text, "utf8 write \xEF\xBF\xBD\xEF\xBF\xBD caf\xC3\xA9");
    const auto formatted = sink->containing("utf8 writef ");
    ASSERT_EQ(formatted.size(), 1u);
    EXPECT_EQ(formatted[0].text, "utf8 writef \xEF\xBF\xBD\xEF\xBF\xBD \xF0\x9F\x98\x80");
}

// a text cut to the record's limit is cut between characters, never inside one
TEST(LoggerTest, TruncationKeepsWholeCharacters) {
    auto sink = recordingSink();
    std::string text = "cut ";
    while (text.size() < LogRecord::MAX_TEXT_BYTES + 100) text += "\xE2\x82\xAC";
    Logger::write(LogLevel::Info, logCategoryLoggerTest(), text);
    Logger::flush();
    const auto found = sink->containing("cut ");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_TRUE(found[0].flags & LogFlagTruncated);
    bool invalid = false;
    StringUtils::decodeUtf8ToCodepoints(found[0].text, invalid);
    EXPECT_FALSE(invalid) << "a character was cut in half";
    EXPECT_EQ(found[0].text.find("\xEF\xBF\xBD"), std::string::npos) << "and none was replaced";
}

// A repaired record whose call emits the rate limit's summary (a logger without a drain thread
// sweeps from inside the call, back through logSubmit) keeps its own text; the summary's sample,
// cut at 80 bytes, is cut between characters and the summary is UTF-8
TEST(LoggerTest, RepairedRecordSurvivesTheSummaryItsCallEmits) {
    FreshLogger logger(1024 * 1024, false);
    auto sink = recordingSink();
    static char burstKey;
    static char recordKey;
    const std::string head(79, 'r');
    const std::string burst = head + "\xC3\xA9 burst";
    for (int i = 0; i < 250; i++) {
        Logger::write(LogLevel::Info, logCategoryLoggerTest(), burst, nullptr, 0, &burstKey);
    }
    // the burst's window ends; the next call sweeps it
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    Logger::write(LogLevel::Info, logCategoryLoggerTest(), "repaired \xFF record", nullptr, 0, &recordKey);
    Logger::flush();
    const auto repaired = sink->containing("record");
    ASSERT_EQ(repaired.size(), 1u);
    EXPECT_EQ(repaired[0].text, "repaired \xEF\xBF\xBD record");
    const auto summaries = sink->containing("suppressed 50 similar messages");
    ASSERT_EQ(summaries.size(), 1u);
    EXPECT_EQ(summaries[0].text, "suppressed 50 similar messages: \"" + head + "\"");
}

TEST(LoggerTest, IdsMonotonicAcrossThreads) {
    FreshLogger logger(32 * 1024 * 1024);
    auto async = recordingSink();
    auto inlineSink = recordingSink(LogSink::Delivery::Inline);
    constexpr int threads = 8;
    constexpr int perThread = 10000;
    std::vector<std::thread> workers;
    static char keys[threads * perThread];
    for (int t = 0; t < threads; t++) {
        workers.emplace_back([t]() {
            for (int i = 0; i < perThread; i++) {
                // a key per record: the rate limit stays out of it
                Logger::write(LogLevel::Info, logCategoryLoggerTest(), "monotonic", nullptr, 0, &keys[t * perThread + i]);
            }
        });
    }
    for (std::thread& worker : workers) worker.join();
    Logger::flush();
    for (const RecordingSink* sink : {&*async, &*inlineSink}) {
        std::vector<uint64_t> ids;
        for (const Recorded& r : sink->taken()) {
            if (r.text == "monotonic") ids.push_back(r.id);
        }
        EXPECT_EQ(ids.size(), static_cast<size_t>(threads * perThread)) << sink->name();
        EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end())) << sink->name();
        EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end()) << sink->name() << ": duplicate ids";
    }
    EXPECT_EQ(Logger::stats().dropped, 0u);
}

TEST(LoggerTest, QueueFullDropsLowFirst) {
    FreshLogger logger(256 * 1024);
    // an Async sink that holds the drain thread until released
    std::mutex mutex;
    std::condition_variable released;
    bool release = false;
    auto blocking = recordingSink();
    blocking->onWrite = [&](const LogRecord& record) {
        if (record.text != "block") return;
        std::unique_lock<std::mutex> lock(mutex);
        released.wait(lock, [&]() { return release; });
    };
    Logger::write(LogLevel::Warn, logCategoryLoggerTest(), "block");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    static char keys[20000];
    int sent = 0;
    while (Logger::stats().dropped == 0 && sent < 20000) {
        Logger::write(LogLevel::Info, logCategoryLoggerTest(), std::string(200, 'i'), nullptr, 0, &keys[sent++]);
    }
    const uint64_t droppedInfo = Logger::stats().dropped;
    EXPECT_GT(droppedInfo, 0u) << "the queue never filled";

    const auto start = std::chrono::steady_clock::now();
    Logger::write(LogLevel::Warn, logCategoryLoggerTest(), "warning on a full queue");
    const double waitedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(waitedMs, 50.0) << "a warning waits at most 5 ms for room";

    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    released.notify_all();
    Logger::flush();
    Logger::write(LogLevel::Info, logCategoryLoggerTest(), "after the stall");
    Logger::flush();
    const auto notices = blocking->containing("messages (queue full)");
    EXPECT_EQ(notices.size(), 1u) << "exactly one dropped notice";
    if (!notices.empty()) {
        EXPECT_EQ(notices[0].level, LogLevel::Warn);
        EXPECT_NE(notices[0].text.find("[log] dropped "), std::string::npos) << notices[0].text;
    }
}

TEST(LoggerTest, ErrorCarriesBacktrace) {
    const bool previous = Logger::backtraceOnError();
    Logger::setBacktraceOnError(true);
    // a fresh second of the backtrace budget
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    auto sink = recordingSink();
    sink->symbolize = true;
    JE_LOG_ERROR(logCategoryLoggerTest(), "error with frames");
    Logger::write(LogLevel::Error, logCategoryLua(), "lua error", nullptr, LogFlagLua | LogFlagNoBacktrace);
    Logger::flush();
    const auto withFrames = sink->containing("error with frames");
    ASSERT_EQ(withFrames.size(), 1u);
    EXPECT_GE(withFrames[0].frameCount, 3u);
#if !defined(_WIN32) || defined(_MSC_VER)
    bool named = false;
    for (const std::string& frame : withFrames[0].frames) {
        named = named || frame.find("ErrorCarriesBacktrace") != std::string::npos;
    }
    EXPECT_TRUE(named) << "the frames do not name the test function";
#endif
    const auto lua = sink->containing("lua error");
    ASSERT_EQ(lua.size(), 1u);
    EXPECT_EQ(lua[0].frameCount, 0u);

    // ten per second: the error above was the first of this second
    const uint64_t omittedBefore = Logger::stats().backtracesOmitted;
    static char keys[10];
    for (int i = 0; i < 10; i++) {
        Logger::write(LogLevel::Error, logCategoryLoggerTest(), "error burst", nullptr, 0, &keys[i]);
    }
    Logger::flush();
    const auto burst = sink->containing("error burst");
    ASSERT_EQ(burst.size(), 10u);
    EXPECT_GT(burst[8].frameCount, 0u);
    EXPECT_EQ(burst[9].frameCount, 0u) << "the 11th backtrace in a second is omitted";
    EXPECT_EQ(Logger::stats().backtracesOmitted, omittedBefore + 1);
    Logger::setBacktraceOnError(previous);
}

TEST(LoggerTest, SafetyValve) {
    auto sink = recordingSink();
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    ASSERT_EQ(luaL_dostring(L, "for i = 1, 10000 do print('valve probe') end"), LUA_OK) << lua_tostring(L, -1);
    // the summary goes out once the second of the burst is over
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    Logger::flush();
    const auto printed = sink->containing("valve probe");
    size_t plain = 0;
    size_t summaries = 0;
    for (const Recorded& r : printed) {
        if (r.text == "valve probe") plain++;
        else if (r.text.find("suppressed 9800 similar messages: \"valve probe\"") == 0) summaries++;
    }
    EXPECT_EQ(plain, 200u);
    EXPECT_EQ(summaries, 1u);
    if (!printed.empty()) {
        EXPECT_EQ(printed[0].category, "lua");
    }

    for (int i = 0; i < 3; i++) {
        JE_LOG_ONCE(Warn, logCategoryLoggerTest(), "logged once");
    }
    Logger::flush();
    EXPECT_EQ(sink->containing("logged once").size(), 1u);
}

TEST(LoggerTest, LuaPrintCarriesChunkAndLine) {
    auto sink = recordingSink();
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_EQ(luaL_loadbuffer(L, "local x = 1\nprint('where am i')", 31, "=probe.lua"), LUA_OK);
    ASSERT_EQ(lua_pcall(L, 0, 0, 0), LUA_OK);
    Logger::flush();
    const auto found = sink->containing("where am i");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].file, "probe.lua");
    EXPECT_EQ(found[0].line, 2u);
    EXPECT_TRUE(found[0].flags & LogFlagLua);
}

TEST(LoggerTest, LogFromSinkNoRecursion) {
    auto other = recordingSink();
    auto chatty = recordingSink();
    chatty->onWrite = [](const LogRecord& record) {
        if (record.text == "outer") JE_LOG_WARN(logCategoryLoggerTest(), "inner");
    };
    JE_LOG_WARN(logCategoryLoggerTest(), "outer");
    Logger::flush();
    Logger::flush();
    EXPECT_EQ(chatty->containing("outer").size(), 1u);
    EXPECT_TRUE(chatty->containing("inner").empty()) << "a sink got the record it logged";
    EXPECT_EQ(other->containing("inner").size(), 1u) << "the other sinks get it";
}

TEST(LoggerTest, PreInitFallback) {
    FreshLogger logger;
    Logger::shutdown();
    ::testing::internal::CaptureStderr();
    JE_LOG_ERROR(logCategoryLoggerTest(), "before init");
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("(ERROR): [logger-test] before init"), std::string::npos) << err;
    Logger::init(Logger::Config());
}

TEST(LoggerTest, ShutdownDrainsAll) {
    FreshLogger logger(8 * 1024 * 1024);
    // a slow sink (still busy when shutdown starts), counting into what outlives it
    auto counted = std::make_shared<std::atomic<int>>(0);
    Logger::addSink(std::make_unique<CallbackSink>("slow", [counted](const LogRecord& record) {
        std::this_thread::sleep_for(std::chrono::microseconds(20));
        if (record.text == "drained") counted->fetch_add(1);
    }));
    static char keys[1000];
    for (int i = 0; i < 1000; i++) Logger::write(LogLevel::Info, logCategoryLoggerTest(), "drained", nullptr, 0, &keys[i]);
    Logger::shutdown();
    EXPECT_EQ(counted->load(), 1000);
    Logger::init(Logger::Config());
}

TEST(LoggerTest, FileFormatCompat) {
    TempDir dir;
    RotatingFileSink::Options options;
    options.path = dir.file("editor.log").string();
    options.banner = "=== banner ===\n";
    auto sink = ScopedSink<RotatingFileSink>(std::make_unique<RotatingFileSink>(options));
    Logger::write(LogLevel::Info, logCategoryEditor(), "an info");
    Logger::write(LogLevel::Warn, logCategoryEditor(), "a warning");
    Logger::write(LogLevel::Error, logCategoryEditor(), "an error", nullptr, LogFlagNoBacktrace);
    Logger::write(LogLevel::Info, logCategoryEditor(), "a success", nullptr, LogFlagSuccess);
    Logger::write(LogLevel::Error, logCategoryBuild(), "main.cpp:3:1: error: oops");
    JE_LOG_WARN(logCategoryRender(), "a render warning");
    Logger::flush();
    const std::string text = dir.read("editor.log");
    const std::regex line(R"(^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d \[(INFO |WARN |ERROR|OK   |BUILD)\] )");
    size_t lines = 0;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string one = text.substr(start, end - start);
        start = end + 1;
        if (one.empty() || one == "=== banner ===" || one.find("logger-test") != std::string::npos) continue;
        if (one.rfind("  #", 0) == 0) continue;   // backtrace frames
        EXPECT_TRUE(std::regex_search(one, line)) << one;
        lines++;
    }
    EXPECT_GE(lines, 6u);
    EXPECT_NE(text.find("[ERROR] an error\n"), std::string::npos) << text;
    EXPECT_NE(text.find("[OK   ] a success\n"), std::string::npos) << text;
    EXPECT_NE(text.find("[BUILD] main.cpp:3:1: error: oops\n"), std::string::npos) << text;
    EXPECT_NE(text.find("[WARN ] a render warning [render] ("), std::string::npos) << text;
    EXPECT_STREQ(logConsolePrefix(LogLevel::Error), "(ERROR): ");
    EXPECT_STREQ(logConsolePrefix(LogLevel::Info), "");
}

TEST(LoggerTest, FileLinesReachTheFileQuickly) {
    TempDir dir;
    RotatingFileSink::Options options;
    options.path = dir.file("quick.log").string();
    auto sink = ScopedSink<RotatingFileSink>(std::make_unique<RotatingFileSink>(options));
    const auto start = std::chrono::steady_clock::now();
    Logger::write(LogLevel::Info, logCategoryLoggerTest(), "quick line");
    while (dir.read("quick.log").find("quick line") == std::string::npos &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    EXPECT_LE(ms, 100.0) << "the line took " << ms << " ms to reach the file";
}

TEST(LoggerTest, LevelSpecs) {
    std::string error;
    EXPECT_TRUE(Logger::applyLevelSpec("info,logger-test=error", &error)) << error;
    EXPECT_EQ(Logger::categoryLevel(logCategoryLoggerTest()), LogLevel::Error);
    EXPECT_FALSE(Logger::applyLevelSpec("loud", &error));
    EXPECT_NE(error.find("unknown log level"), std::string::npos);
    EXPECT_TRUE(Logger::applyLevelSpec("logger-test=info"));
    EXPECT_EQ(Logger::categoryLevel(logCategoryLoggerTest()), LogLevel::Info);
}
