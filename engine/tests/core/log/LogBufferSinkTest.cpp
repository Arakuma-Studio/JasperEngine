// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The log's buffer and listeners (feature debug-overlay) as S09's resident Inline sink "buffer"
// (engine/core/Log.cpp, docs/specs/S09-core-utilities.md "Upstream absorption"): it gets what
// every other sink gets, records of any category, level and thread, with the Logger's ids, in id
// order, before Logger::init and after Logger::shutdown as well; listeners run outside the
// Logger's lock on the thread that logged; the parts Log::emitEntry gives are kept as given.
// Run under ThreadSanitizer too (SANITIZE=thread tools/build.sh unit-tests).

#include "log/LogTestSupport.h"

#include "Log.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    JE_LOG_CATEGORY(logCategoryBufferTest, "buffer-test", LogLevel::Trace)

    // the records an Inline sink next to the buffer got, by id
    struct Witness {
        std::mutex mutex;
        std::map<uint64_t, std::string> texts;
        std::map<uint64_t, LogLevel> levels;
    };

    // a sink that sees what the buffer sees (Inline, every level), into a Witness that outlives it
    Logger::SinkId addWitness(const std::shared_ptr<Witness>& witness){
        return Logger::addSink(std::make_unique<CallbackSink>("witness", [witness](const LogRecord& record) {
            if (!record.category || std::string(record.category->name) != "buffer-test") return;
            std::lock_guard<std::mutex> lock(witness->mutex);
            witness->texts[record.id] = std::string(record.text);
            witness->levels[record.id] = record.level;
        }, LogSink::Delivery::Inline));
    }

    std::vector<LogEntry> testEntries(uint64_t afterId){
        std::vector<LogEntry> mine;
        for (const LogEntry& entry : Log::getEntries(0, LOG_LEVELS_ALL, afterId)) {
            if (entry.category && std::string(entry.category->name) == "buffer-test") mine.push_back(entry);
        }
        return mine;
    }

    class LogBufferSinkTest : public ::testing::Test {
    protected:
        size_t bufferSize = 0;
        void SetUp() override {
            bufferSize = Log::getBufferSize();
            Log::dispatchPending();
            Log::setBufferSize(Log::maxBufferSize);
        }
        void TearDown() override {
            Log::dispatchPending();
            Log::setBufferSize(bufferSize);
        }
    };

    // one distinct line: the rate limit keys a pass-through format by its text
    void logDistinct(LogLevel level, const char* prefix, int a, int b){
        char text[96];
        std::snprintf(text, sizeof(text), "%s %d.%d", prefix, a, b);
        switch (level) {
            case LogLevel::Trace: JE_LOG_AT(LogLevel::Trace, logCategoryBufferTest(), "%s", text); break;
            case LogLevel::Debug: JE_LOG_AT(LogLevel::Debug, logCategoryBufferTest(), "%s", text); break;
            case LogLevel::Warn: JE_LOG_WARN(logCategoryBufferTest(), "%s", text); break;
            case LogLevel::Error: JE_LOG_ERROR(logCategoryBufferTest(), "%s", text); break;
            default: JE_LOG_INFO(logCategoryBufferTest(), "%s", text); break;
        }
    }

}

TEST_F(LogBufferSinkTest, IsAResidentInlineSinkOfTheLogger) {
    const uint32_t id = Log::getBufferSinkId();
    ASSERT_NE(id, 0u);
    EXPECT_EQ(Logger::findSink("buffer"), id);
    bool listed = false;
    for (const Logger::SinkInfo& info : Logger::sinkInfo()) {
        if (info.id != id) continue;
        listed = true;
        EXPECT_EQ(info.delivery, LogSink::Delivery::Inline);
        EXPECT_EQ(info.level, LogLevel::Trace);
    }
    EXPECT_TRUE(listed);
}

// what upstream's Log::* put in its buffer the sink puts there too, and every other record: the
// JE_LOG_* macros of any category, with the Logger's ids and levels
TEST_F(LogBufferSinkTest, GetsWhatTheOtherSinksGet) {
    auto witness = std::make_shared<Witness>();
    const Logger::SinkId sink = addWitness(witness);
    const uint64_t start = Log::getLastId();
    const LogLevel levels[] = {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error};
    for (int i = 0; i < 50; i++) logDistinct(levels[i % 5], "same", 0, i);
    Logger::removeSink(sink);

    const std::vector<LogEntry> entries = testEntries(start);
    std::lock_guard<std::mutex> lock(witness->mutex);
    ASSERT_EQ(entries.size(), witness->texts.size());
    ASSERT_EQ(entries.size(), 50u);
    uint64_t previous = 0;
    for (const LogEntry& entry : entries) {
        EXPECT_GT(entry.id, previous) << "in id order";
        previous = entry.id;
        ASSERT_TRUE(witness->texts.count(entry.id)) << entry.id;
        EXPECT_EQ(entry.message, witness->texts[entry.id]);
        EXPECT_EQ(entry.level, witness->levels[entry.id]);
        EXPECT_TRUE(entry.mainThread);
        EXPECT_FALSE(entry.fromLua);
    }
    EXPECT_EQ(Log::getLastId(), entries.back().id);
}

// From many threads at once: every record reaches the buffer, the AnyThread listeners (on the
// thread that logged, once each) and the MainThread ones (at dispatchPending, in id order),
// exactly the records another Inline sink got
TEST_F(LogBufferSinkTest, ManyThreadsDeliverEveryRecordOnce) {
    constexpr int threads = 6;
    constexpr int perThread = 150;   // 900 lines: under the 1,000 the main thread's queue holds
    auto witness = std::make_shared<Witness>();
    const Logger::SinkId sink = addWitness(witness);

    struct Seen {
        std::mutex mutex;
        std::map<uint64_t, std::thread::id> anyThread;
        std::vector<uint64_t> mainThread;
        std::atomic<int> duplicates{0};
    };
    auto seen = std::make_shared<Seen>();
    const LogListenerId any = Log::addListener([seen](const LogEntry& entry) {
        if (!entry.category || std::string(entry.category->name) != "buffer-test") return;
        std::lock_guard<std::mutex> lock(seen->mutex);
        if (!seen->anyThread.emplace(entry.id, std::this_thread::get_id()).second) seen->duplicates++;
    }, LogDelivery::AnyThread);
    const LogListenerId main = Log::addListener([seen](const LogEntry& entry) {
        if (!entry.category || std::string(entry.category->name) != "buffer-test") return;
        EXPECT_TRUE(Log::isMainThread());
        seen->mainThread.push_back(entry.id);
    });

    const uint64_t start = Log::getLastId();
    std::mutex idsMutex;
    std::map<std::thread::id, int> threadIndex;
    std::vector<std::thread> workers;
    std::atomic<bool> go{false};
    for (int t = 0; t < threads; t++) {
        workers.emplace_back([t, &go, &idsMutex, &threadIndex]() {
            {
                std::lock_guard<std::mutex> lock(idsMutex);
                threadIndex[std::this_thread::get_id()] = t;
            }
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < perThread; i++) logDistinct(i % 3 == 0 ? LogLevel::Warn : LogLevel::Info, "worker", t, i);
        });
    }
    go = true;
    for (std::thread& worker : workers) worker.join();
    Logger::removeSink(sink);

    const std::vector<LogEntry> entries = testEntries(start);
    ASSERT_EQ(entries.size(), static_cast<size_t>(threads * perThread));
    {
        std::lock_guard<std::mutex> lock(witness->mutex);
        EXPECT_EQ(witness->texts.size(), entries.size());
        for (const LogEntry& entry : entries) {
            EXPECT_EQ(witness->texts[entry.id], entry.message);
            EXPECT_FALSE(entry.mainThread);
        }
    }
    {
        std::lock_guard<std::mutex> lock(seen->mutex);
        EXPECT_EQ(seen->duplicates.load(), 0);
        EXPECT_EQ(seen->anyThread.size(), entries.size());
        // each on the thread that logged it: "worker <t>.<i>"
        for (const LogEntry& entry : entries) {
            auto found = seen->anyThread.find(entry.id);
            ASSERT_NE(found, seen->anyThread.end());
            const int t = threadIndex[found->second];
            EXPECT_EQ(entry.message.rfind("worker " + std::to_string(t) + ".", 0), 0u) << entry.message;
        }
    }
    EXPECT_TRUE(seen->mainThread.empty()) << "MainThread listeners wait for the frame";
    Log::dispatchPending();
    ASSERT_EQ(seen->mainThread.size(), entries.size());
    EXPECT_TRUE(std::is_sorted(seen->mainThread.begin(), seen->mainThread.end())) << "in id order";
    Log::removeListener(any);
    Log::removeListener(main);
}

// a listener runs outside the Logger's lock: it can wait for another thread that logs (under
// the lock that thread would never get it)
TEST_F(LogBufferSinkTest, ListenersRunOutsideTheLoggersLock) {
    std::atomic<int> nested{0};
    const LogListenerId id = Log::addListener([&nested](const LogEntry& entry) {
        if (entry.message != "outer") return;
        std::thread([]() { JE_LOG_INFO(logCategoryBufferTest(), "from a thread the listener waits for"); }).join();
        nested++;
    }, LogDelivery::AnyThread);
    std::atomic<bool> done{false};
    std::thread logger([&done]() {
        JE_LOG_INFO(logCategoryBufferTest(), "outer");
        done = true;
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done.load() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_TRUE(done.load()) << "a listener was called under the Logger's lock";
    logger.join();
    EXPECT_EQ(nested.load(), 1);
    Log::removeListener(id);
    Log::dispatchPending();
}

// Log::emitEntry's parts are the line's, even with "stack traceback:" in the message; the text
// the other sinks get is the message and the trace under it
TEST_F(LogBufferSinkTest, GivenPartsAreKept) {
    auto witness = std::make_shared<Witness>();
    const Logger::SinkId sink = addWitness(witness);
    const uint64_t start = Log::getLastId();
    Log::emitEntry(LogLevel::Warn, "a message about stack traceback: in text", "stack traceback:\n\tx.lua:1: in main chunk", true,
                   &logCategoryBufferTest());
    Logger::removeSink(sink);
    const std::vector<LogEntry> entries = testEntries(start);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].message, "a message about stack traceback: in text");
    EXPECT_EQ(entries[0].stackTrace, "stack traceback:\n\tx.lua:1: in main chunk");
    EXPECT_TRUE(entries[0].fromLua);
    std::lock_guard<std::mutex> lock(witness->mutex);
    EXPECT_EQ(witness->texts[entries[0].id], entries[0].message + "\n" + entries[0].stackTrace);
}

// Resident: the records of a process without a running Logger (before init, after shutdown)
// are buffered too, with ids that go on increasing, and the sink is still there after init
TEST_F(LogBufferSinkTest, BuffersBeforeInitAndAfterShutdown) {
    FreshLogger logger;
    Logger::shutdown();
    EXPECT_EQ(Logger::findSink("buffer"), Log::getBufferSinkId()) << "shutdown keeps the resident sink";
    const uint64_t start = Log::getLastId();
    ::testing::internal::CaptureStderr();
    JE_LOG_WARN(logCategoryBufferTest(), "without a logger");
    std::thread([]() { JE_LOG_ERROR(logCategoryBufferTest(), "without a logger, another thread"); }).join();
    const std::string err = ::testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("without a logger"), std::string::npos) << "the stderr fallback still writes";
    std::vector<LogEntry> entries = testEntries(start);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].message, "without a logger");
    EXPECT_LT(entries[0].id, entries[1].id);
    EXPECT_FALSE(entries[1].mainThread);

    Logger::init(Logger::Config());
    JE_LOG_WARN(logCategoryBufferTest(), "with a logger again");
    entries = testEntries(start);
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_GT(entries[2].id, entries[1].id);
    Log::dispatchPending();
}

// the rate limit's summary and the queue's drop notice are records like any: buffered
TEST_F(LogBufferSinkTest, RateLimitedFloodIsSummarisedInTheBuffer) {
    const uint64_t start = Log::getLastId();
    for (int i = 0; i < 1000; i++) JE_LOG_INFO(logCategoryBufferTest(), "one call site %d", i);
    // the window ends: the summary comes at the next sweep
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    Logger::flush();
    const std::vector<LogEntry> entries = testEntries(start);
    size_t lines = 0;
    bool summary = false;
    for (const LogEntry& entry : entries) {
        if (entry.message.rfind("one call site ", 0) == 0) lines++;
        if (entry.message.find("suppressed") != std::string::npos && entry.message.find("one call site") != std::string::npos) summary = true;
    }
    EXPECT_LE(lines, 400u) << "held back past the limit";
    EXPECT_GE(lines, 200u);
    EXPECT_TRUE(summary);
}

// the editor's own categories (Out::*, build output, the MCP server) are not the game's log
TEST_F(LogBufferSinkTest, EditorCategoriesAreNotBuffered) {
    const uint64_t start = Log::getLastId();
    Logger::write(LogLevel::Warn, logCategoryEditor(), "an editor line");
    Logger::write(LogLevel::Warn, logCategoryBuild(), "a build line");
    Logger::write(LogLevel::Warn, logCategoryMcp(), "an mcp line");
    Logger::write(LogLevel::Warn, logCategoryEngine(), "an engine line");
    const std::vector<LogEntry> entries = Log::getEntries(0, LOG_LEVELS_ALL, start);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].message, "an engine line");
    EXPECT_EQ(Logger::sinkLevel(Log::getBufferSinkId(), &logCategoryEditor()), LogLevel::Off);
}
