// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine::editor::mcp {

    struct ToolResult;

    // One tools/call as the call log writes it. It holds nothing an agent wrote or a tool
    // returned (no arguments, paths, entity names or result content): the log is meant to be
    // shared, and a call's arguments may hold secrets.
    struct McpCallRecord {
        std::chrono::system_clock::time_point time;   // when the request arrived
        std::string tool;
        std::string group;          // McpRegistry::groupOf: "core" for the always-listed tools
        std::string client;         // the initialize handshake's clientInfo.name, else the transport
        double ms = 0.0;
        bool ok = true;
        std::string errorKind;      // empty when ok (errorKindOf)
        uint64_t resultBytes = 0;   // the tools/call result (content, structuredContent) as sent; 0 when cancelled
    };

    // The opt-in call log (JASPER_MCP_LOG=<path>, or Log Calls To File in the MCP Server window):
    // one JSON line per tools/call, appended by a background thread so a call never waits on the
    // disk. Past maxBytes the file moves to <path>.1 (replacing the older one) and a new one
    // starts. A failed write turns the log off for the rest of the session with one Console
    // warning; the calls themselves never fail because of it.
    class McpCallLog {
    public:
        static constexpr uint64_t DEFAULT_MAX_BYTES = 16ull * 1024 * 1024;

        McpCallLog() = default;
        ~McpCallLog();
        McpCallLog(const McpCallLog&) = delete;
        McpCallLog& operator=(const McpCallLog&) = delete;

        // Appends to `path` from now on (the previous file, if any, is written out and closed
        // first). Any thread but the writer's.
        void open(const std::filesystem::path& path, uint64_t maxBytes = DEFAULT_MAX_BYTES);
        // Writes out what is queued and stops the writer.
        void close();
        // Open and not turned off by a failed write
        bool isOpen() const;
        std::filesystem::path path() const;
        // Why a write failed (empty: none); the log is off since then
        std::string failure() const;

        // Queues one line. Any thread; does nothing while closed or after a failure.
        void append(const McpCallRecord& record);

        // {"ts","tool","group","ms","ok","error_kind","result_bytes","client"} in that order
        static std::string formatLine(const McpCallRecord& record);
        // null for a success; otherwise a fixed word, never text from the result: "cancelled",
        // "busy" (a job or a save holds what the tool needs), "dialog" (an editor dialog blocks
        // edits), "internal" (a fault of the tool, not of its arguments), the result's own
        // snake_case `code`, or "error"
        static std::string errorKindOf(const ToolResult& result, bool cancelled);

    private:
        mutable std::mutex mutex;
        std::condition_variable wake;
        std::thread writer;
        std::filesystem::path filePath;
        uint64_t limit = DEFAULT_MAX_BYTES;
        std::string pending;
        bool running = false;
        bool stopping = false;
        bool failed = false;
        std::string failureText;

        void run(std::filesystem::path path, uint64_t maxBytes);
        void fail(const std::string& reason);
    };

    // Calls and errors per tool since the editor started or the MCP Server window was cleared
    // (the window's per-tool counts). Not thread-safe: the server guards it.
    class McpToolCounts {
    public:
        struct Row {
            std::string tool;
            uint64_t calls = 0;
            uint64_t errors = 0;
            double errorRate() const { return calls ? double(errors) / double(calls) : 0.0; }
        };

        // A cancelled call counts as a call, not as an error
        void record(const std::string& tool, bool error);
        void clear() { counts.clear(); }
        // Most calls first, then by name
        std::vector<Row> rows() const;

    private:
        std::map<std::string, Row> counts;
    };

}
