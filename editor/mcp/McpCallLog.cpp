// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpCallLog.h"

#include "McpCall.h"
#include "McpJson.h"

#include "Out.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <system_error>

namespace jasperengine::editor::mcp {

namespace {

// The writer wakes this often, or sooner once this much is queued: a reader (call_profile.py, a
// test polling the file) sees a call within a fraction of a second, and a burst of calls is one write
constexpr auto CALL_LOG_FLUSH_INTERVAL = std::chrono::milliseconds(200);
constexpr size_t CALL_LOG_FLUSH_BYTES = 64 * 1024;

std::string callLogJsonString(const std::string& text) {
    return Json(text).dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string callLogTimestamp(std::chrono::system_clock::time_point time) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    const long long millis = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char date[32];
    std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc);
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03dZ", date, static_cast<int>(((millis % 1000) + 1000) % 1000));
    return out;
}

// A tool's own error code is a fixed identifier (bad_value, write_failed); anything else could
// be text the agent sent, so it is not written
bool isCallLogWord(const std::string& text) {
    if (text.empty() || text.size() > 48 || text[0] < 'a' || text[0] > 'z') return false;
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

}

McpCallLog::~McpCallLog() {
    close();
}

void McpCallLog::open(const std::filesystem::path& path, uint64_t maxBytes) {
    close();
    std::lock_guard<std::mutex> lock(mutex);
    filePath = path;
    limit = maxBytes > 0 ? maxBytes : DEFAULT_MAX_BYTES;
    pending.clear();
    failed = false;
    failureText.clear();
    stopping = false;
    running = true;
    writer = std::thread(&McpCallLog::run, this, filePath, limit);
}

void McpCallLog::close() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!running) return;
        stopping = true;
    }
    wake.notify_all();
    if (writer.joinable()) writer.join();
    std::lock_guard<std::mutex> lock(mutex);
    running = false;
    stopping = false;
    pending.clear();
}

bool McpCallLog::isOpen() const {
    std::lock_guard<std::mutex> lock(mutex);
    return running && !stopping && !failed;
}

std::filesystem::path McpCallLog::path() const {
    std::lock_guard<std::mutex> lock(mutex);
    return running ? filePath : std::filesystem::path();
}

std::string McpCallLog::failure() const {
    std::lock_guard<std::mutex> lock(mutex);
    return failureText;
}

void McpCallLog::append(const McpCallRecord& record) {
    std::string line = formatLine(record);
    bool full = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!running || stopping || failed) return;
        pending += line;
        full = pending.size() >= CALL_LOG_FLUSH_BYTES;
    }
    if (full) wake.notify_all();
}

void McpCallLog::fail(const std::string& reason) {
    std::string where;
    {
        std::lock_guard<std::mutex> lock(mutex);
        failed = true;
        failureText = reason;
        pending.clear();
        where = FileUtils::pathToUtf8(filePath);
    }
    Out::warning("MCP call log %s: %s; no more calls are logged this session.", where.c_str(), reason.c_str());
}

void McpCallLog::run(std::filesystem::path path, uint64_t maxBytes) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out;
    out.open(path, std::ios::binary | std::ios::app);
    if (!out) {
        fail("cannot open the file for appending");
        return;
    }
    uint64_t size = std::filesystem::file_size(path, ec);
    if (ec) size = 0;
    std::filesystem::path rotated = path;
    rotated += ".1";

    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
        wake.wait_for(lock, CALL_LOG_FLUSH_INTERVAL, [this]() { return stopping || pending.size() >= CALL_LOG_FLUSH_BYTES; });
        std::string batch;
        batch.swap(pending);
        const bool last = stopping;
        lock.unlock();

        size_t at = 0;
        while (at < batch.size()) {
            size_t end = batch.find('\n', at);
            end = end == std::string::npos ? batch.size() : end + 1;
            const uint64_t length = end - at;
            // whole lines only: each file stays under the cap unless one line alone is larger
            if (size > 0 && size + length > maxBytes) {
                out.close();
                std::filesystem::rename(path, rotated, ec);
                if (ec) {
                    fail("cannot rotate it to " + FileUtils::pathToUtf8(rotated.filename()) + " (" + ec.message() + ")");
                    return;
                }
                out.open(path, std::ios::binary | std::ios::app);
                if (!out) {
                    fail("cannot open a new file after rotating");
                    return;
                }
                size = 0;
            }
            out.write(batch.data() + at, static_cast<std::streamsize>(length));
            size += length;
            at = end;
        }
        if (!batch.empty()) {
            out.flush();
            if (!out) {
                fail("a write failed (disk full or the file is gone)");
                return;
            }
        }

        lock.lock();
        // append() refuses lines once stopping is set, so nothing is left behind
        if (last) break;
    }
}

std::string McpCallLog::formatLine(const McpCallRecord& record) {
    const double ms = std::isfinite(record.ms) ? std::round(std::max(0.0, record.ms) * 10.0) / 10.0 : 0.0;
    std::string line = "{\"ts\":" + callLogJsonString(callLogTimestamp(record.time));
    line += ",\"tool\":" + callLogJsonString(record.tool);
    line += ",\"group\":" + callLogJsonString(record.group);
    line += ",\"ms\":" + Json(ms).dump();
    line += std::string(",\"ok\":") + (record.ok ? "true" : "false");
    line += ",\"error_kind\":" + (record.errorKind.empty() ? std::string("null") : callLogJsonString(record.errorKind));
    line += ",\"result_bytes\":" + std::to_string(record.resultBytes);
    line += ",\"client\":" + callLogJsonString(record.client);
    line += "}\n";
    return line;
}

std::string McpCallLog::errorKindOf(const ToolResult& result, bool cancelled) {
    if (cancelled) return "cancelled";
    if (!result.isError) return std::string();
    const Json& structured = result.structured;
    if (structured.is_object()) {
        if (structured.contains("busy")) return "busy";
        if (structured.contains("dialog")) return "dialog";
        const auto code = structured.find("code");
        if (code != structured.end() && code->is_string() && isCallLogWord(code->get<std::string>())) return code->get<std::string>();
    }
    if (result.text.rfind("internal error in ", 0) == 0 || result.text.rfind("Internal error", 0) == 0) return "internal";
    return "error";
}

void McpToolCounts::record(const std::string& tool, bool error) {
    Row& row = counts[tool];
    row.tool = tool;
    ++row.calls;
    if (error) ++row.errors;
}

std::vector<McpToolCounts::Row> McpToolCounts::rows() const {
    std::vector<Row> out;
    out.reserve(counts.size());
    for (const auto& item : counts) out.push_back(item.second);
    std::stable_sort(out.begin(), out.end(), [](const Row& a, const Row& b) { return a.calls > b.calls; });
    return out;
}

}  // namespace jasperengine::editor::mcp
