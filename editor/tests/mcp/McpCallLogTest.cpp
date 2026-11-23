// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The MCP call log's line and error kinds, its failed write (off for the session, one Console
// warning), and the MCP Server window's per-tool counts. The log's fields, the arguments it never
// holds and its rotation are tested end to end in tools/mcp/tests/test_protocol.py.

#include <gtest/gtest.h>

#include "Out.h"
#include "mcp/McpCall.h"
#include "mcp/McpCallLog.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace McpCallLogTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

namespace {

    std::filesystem::path callLogScratch(const char* name) {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() /
            ("je-mcp-call-log-" + std::string(name) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        return dir;
    }

}

TEST(McpCallLog, LineHasTheFieldsInOrderAndNothingElse) {
    McpCallRecord record;
    record.time = std::chrono::system_clock::time_point(std::chrono::milliseconds(1795262400123LL));
    record.tool = "set_properties";
    record.group = "core";
    record.client = "claude \"code\"";
    record.ms = 12.345;
    record.ok = false;
    record.errorKind = "busy";
    record.resultBytes = 4096;
    EXPECT_EQ(McpCallLog::formatLine(record),
              "{\"ts\":\"2026-11-21T12:00:00.123Z\",\"tool\":\"set_properties\",\"group\":\"core\",\"ms\":12.3,\"ok\":false,"
              "\"error_kind\":\"busy\",\"result_bytes\":4096,\"client\":\"claude \\\"code\\\"\"}\n");

    record.ok = true;
    record.errorKind.clear();
    record.ms = -1.0;
    const std::string line = McpCallLog::formatLine(record);
    EXPECT_NE(line.find("\"ok\":true,\"error_kind\":null,"), std::string::npos) << line;
    EXPECT_NE(line.find("\"ms\":0.0,"), std::string::npos) << line;
}

TEST(McpCallLog, ErrorKindNeverCopiesResultText) {
    EXPECT_EQ(McpCallLog::errorKindOf(ToolResult::success("done"), false), "");
    EXPECT_EQ(McpCallLog::errorKindOf(ToolResult::success("done"), true), "cancelled");
    EXPECT_EQ(McpCallLog::errorKindOf(ToolResult::error("held", Json{{"busy", {{"kind", "scene_save"}}}}), false), "busy");
    EXPECT_EQ(McpCallLog::errorKindOf(ToolResult::error("answer it", Json{{"dialog", Json::object()}}), false), "dialog");
    EXPECT_EQ(McpCallLog::errorKindOf(ToolResult::error("bad_value: x", Json{{"code", "bad_value"}}), false), "bad_value");
    // a code that is not a plain identifier could be the agent's own text
    EXPECT_EQ(McpCallLog::errorKindOf(ToolResult::error("x", Json{{"code", "sk-SECRET value"}}), false), "error");
    EXPECT_EQ(McpCallLog::errorKindOf(ToolResult::error("internal error in get_scene (not caused by your arguments)"), false),
              "internal");
    EXPECT_EQ(McpCallLog::errorKindOf(ToolResult::error("Internal error: tool x returned without a result."), false), "internal");
    EXPECT_EQ(McpCallLog::errorKindOf(ToolResult::error("Unknown argument \"sk-SECRET\" for x."), false), "error");
}

TEST(McpCallLog, FailedWriteTurnsItOffWithOneWarning) {
    if (!Out::memorySinkId()) Out::startLogging();
    const std::filesystem::path dir = callLogScratch("fail");
    const uint64_t cursor = Out::lastLogId();
    McpCallLog log;
    // a directory where the file should be: it cannot be opened for appending
    log.open(dir);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (log.failure().empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_FALSE(log.failure().empty()) << "the writer reports the failure";
    EXPECT_FALSE(log.isOpen());
    McpCallRecord record;
    record.tool = "editor_status";
    log.append(record);
    log.append(record);
    log.close();
    int warnings = 0;
    for (const jasperengine::editor::LogEntry& entry : Out::getLogsSince(cursor)) {
        if (entry.type == LogType::Warning && entry.message.find("MCP call log") != std::string::npos) ++warnings;
    }
    EXPECT_EQ(warnings, 1) << "one warning for the session, none per later call";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(McpCallLog, ClosedLogWritesNothingAndReopens) {
    const std::filesystem::path dir = callLogScratch("reopen");
    const std::filesystem::path file = dir / "calls.jsonl";
    McpCallLog log;
    McpCallRecord record;
    record.tool = "editor_status";
    log.append(record);
    EXPECT_FALSE(std::filesystem::exists(file)) << "nothing is written before open";
    log.open(file);
    EXPECT_TRUE(log.isOpen());
    EXPECT_EQ(log.path(), file);
    log.append(record);
    log.close();
    EXPECT_FALSE(log.isOpen());
    EXPECT_TRUE(log.path().empty());
    log.append(record);
    log.open(file);
    log.append(record);
    log.close();
    // close() writes out what was queued; the call made while closed is not there
    const std::uintmax_t size = std::filesystem::file_size(file);
    EXPECT_EQ(size, 2 * McpCallLog::formatLine(record).size());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(McpToolCounts, MostCallsFirstWithErrorRates) {
    McpToolCounts counts;
    counts.record("screenshot", false);
    counts.record("set_properties", true);
    counts.record("set_properties", false);
    counts.record("set_properties", false);
    counts.record("editor_status", false);
    counts.record("screenshot", true);
    const std::vector<McpToolCounts::Row> rows = counts.rows();
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].tool, "set_properties");
    EXPECT_EQ(rows[0].calls, 3u);
    EXPECT_EQ(rows[0].errors, 1u);
    EXPECT_NEAR(rows[0].errorRate(), 1.0 / 3.0, 1e-9);
    EXPECT_EQ(rows[1].tool, "screenshot");
    EXPECT_DOUBLE_EQ(rows[1].errorRate(), 0.5);
    EXPECT_EQ(rows[2].tool, "editor_status") << "ties keep the names' order";
    EXPECT_DOUBLE_EQ(rows[2].errorRate(), 0.0);
    counts.clear();
    EXPECT_TRUE(counts.rows().empty());
}

}  // namespace McpCallLogTest_cpp
