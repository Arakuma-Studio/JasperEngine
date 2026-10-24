// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Out.h"

#include <gtest/gtest.h>

#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {
    // the editor's store (main() starts it in the editor)
    void ensureLogging() {
        if (!Out::memorySinkId()) Out::startLogging();
    }
}

TEST(Out, ReadAfterLogOnAnyThread) {
    ensureLogging();
    const uint64_t cursor = Out::lastLogId();
    std::thread worker([]() { Out::error("worker error %d", 42); });
    worker.join();
    const std::vector<LogEntry> logs = Out::getLogsSince(cursor);
    ASSERT_FALSE(logs.empty()) << "the entry is there right after the call returns";
    EXPECT_EQ(logs.back().message, "worker error 42");
    EXPECT_EQ(logs.back().type, LogType::Error);
    EXPECT_EQ(logs.back().category, "editor");
}

TEST(Out, TypesFromLevelFlagsAndCategory) {
    ensureLogging();
    const uint64_t cursor = Out::lastLogId();
    Out::info("an info");
    Out::success("a success");
    Out::warning(std::string("a warning"));
    Out::build("main.cpp:1:2: warning: unused");
    Out::build("[ 50%] Building CXX object");
    const std::vector<LogEntry> logs = Out::getLogsSince(cursor);
    ASSERT_EQ(logs.size(), 5u);
    EXPECT_EQ(logs[0].type, LogType::Info);
    EXPECT_EQ(logs[1].type, LogType::Success);
    EXPECT_EQ(logs[2].type, LogType::Warning);
    EXPECT_EQ(logs[3].type, LogType::Build);
    EXPECT_EQ(logs[3].level, LogLevel::Warn) << "a compiler warning is a Warn record of category build";
    EXPECT_EQ(logs[4].level, LogLevel::Info);
    const std::vector<LogEntry> problems = Out::getLogsSince(cursor, 0, true);
    EXPECT_EQ(problems.size(), 3u) << "warnings and build output";
}

TEST(Out, ClearKeepsCursors) {
    ensureLogging();
    Out::info("before clear");
    const uint64_t cursor = Out::lastLogId();
    Out::clearConsole();
    EXPECT_TRUE(Out::getLogsSince(0).empty());
    EXPECT_EQ(Out::lastLogId(), cursor) << "ids keep counting";
    Out::info("after clear");
    const std::vector<LogEntry> logs = Out::getLogsSince(cursor);
    ASSERT_EQ(logs.size(), 1u);
    EXPECT_GT(logs[0].id, cursor);
}
