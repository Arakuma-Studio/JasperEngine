// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DeviceLogPump.h"

#include "thread/ThreadUtil.h"

#include <chrono>
#include <regex>
#include <string>
#include <utility>

using namespace jasperengine;

namespace jasperengine::editor::platform {

DeviceLogPump::DeviceLogPump(Sink sink): DeviceLogPump(std::move(sink), Options()) {}

DeviceLogPump::DeviceLogPump(Sink sink, Options options): sink(std::move(sink)), options(options) {}

DeviceLogPump::~DeviceLogPump() {
    stop();
}

double DeviceLogPump::steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void DeviceLogPump::push(LogLevel level, std::string text, double now) {
    if (now < 0.0) now = steadySeconds();
    std::lock_guard<std::mutex> lock(mutex);
    if (windowStart < 0.0 || now - windowStart >= 1.0) {
        if (windowDropped > 0) {
            pending.push_back({LogLevel::Warn, std::to_string(windowDropped) + " log lines dropped (over " +
                                                   std::to_string(options.maxLinesPerSecond) + " lines/s)"});
        }
        windowStart = now;
        windowLines = 0;
        windowDropped = 0;
    }
    if (options.maxLinesPerSecond > 0 && windowLines >= options.maxLinesPerSecond) {
        windowDropped++;
        droppedTotal++;
        return;
    }
    windowLines++;
    pending.push_back({level, std::move(text)});
    if (lastBatch < 0.0) lastBatch = now;
}

void DeviceLogPump::tick(double now, bool force) {
    if (now < 0.0) now = steadySeconds();
    std::vector<DeviceLogLine> batch;
    {
        std::lock_guard<std::mutex> lock(mutex);
        // the drop count of a window nothing followed
        if (force && windowDropped > 0) {
            pending.push_back({LogLevel::Warn, std::to_string(windowDropped) + " log lines dropped (over " +
                                                   std::to_string(options.maxLinesPerSecond) + " lines/s)"});
            windowDropped = 0;
        }
        if (pending.empty()) return;
        if (!force && now - lastBatch < options.interval) return;
        batch.swap(pending);
        lastBatch = now;
    }
    batchCount++;
    if (sink) sink(std::move(batch));
}

void DeviceLogPump::start() {
    std::lock_guard<std::mutex> lock(mutex);
    if (running) return;
    running = true;
    thread = jasperengine::thread::spawnNamed("je-device-log", [this]() {
        std::unique_lock<std::mutex> lock(mutex);
        while (running) {
            wake.wait_for(lock, std::chrono::duration<double>(options.interval));
            if (!running) break;
            lock.unlock();
            tick();
            lock.lock();
        }
    });
}

void DeviceLogPump::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        running = false;
    }
    wake.notify_all();
    if (thread.joinable()) thread.join();
    tick(-1.0, true);
}

SimulatorLogMerge::SimulatorLogMerge(std::string subsystem, Emit emit, double hold)
    : subsystem(std::move(subsystem)), emit(std::move(emit)), hold(hold) {}

namespace {

    // the categories the engine's console sink writes without their name (logShowsCategory)
    bool simulatorUnnamedCategory(const std::string& category) {
        return category == "engine" || category == "editor" || category == "lua" || category == "build";
    }

}

// "2026-11-20 10:00:00.123 Df Game[1234:5678] [com.example:render] text"
void SimulatorLogMerge::unifiedLine(const std::string& line, double now) {
    if (now < 0.0) now = DeviceLogPump::steadySeconds();
    static const std::regex compact(R"(^\S+ \S+ (\w+)\s+\S+\[\d+:\w+\] (.*)$)");
    std::smatch match;
    std::lock_guard<std::mutex> lock(mutex);
    if (!std::regex_match(line, match, compact)) {
        if (line.rfind("Filtering the log data", 0) == 0 || line.rfind("Timestamp", 0) == 0) return;
        emit(LogLevel::Info, line);
        return;
    }
    const std::string kind = match[1].str();
    const std::string message = match[2].str();
    // the engine's lines: "[<bundle id>:<category>] text"
    const std::string ours = "[" + subsystem + ":";
    const size_t close = message.rfind(ours, 0) == 0 ? message.find("] ", ours.size()) : std::string::npos;
    // the engine logs a warning at the default type (AppleLogSink)
    LogLevel level = kind == "Db" ? LogLevel::Debug : kind == "E" ? LogLevel::Error : kind == "F" ? LogLevel::Fatal : LogLevel::Info;
    if (kind == "Df" && close != std::string::npos) level = LogLevel::Warn;
    emit(level, message);
    if (close == std::string::npos) return;
    // the console's copy: the text, after "[category] " for the categories it names
    const std::string category = message.substr(ours.size(), close - ours.size());
    const std::string key = (simulatorUnnamedCategory(category) ? std::string() : "[" + category + "] ") + message.substr(close + 2);
    for (auto it = console.begin(); it != console.end(); ++it) {
        if (it->key == key) {
            console.erase(it);
            return;
        }
    }
    unified.emplace_back(key, now);
}

void SimulatorLogMerge::consoleLine(const std::string& line, bool stderrStream, double now) {
    if (now < 0.0) now = DeviceLogPump::steadySeconds();
    std::string text = line;
    if (!text.empty() && text.back() == '\r') text.pop_back();
    if (text.empty()) return;
    // the console sink's level prefix (logConsolePrefix)
    static const std::pair<const char*, LogLevel> prefixes[] = {
        {"(VERBOSE): ", LogLevel::Trace}, {"(DEBUG): ", LogLevel::Debug}, {"(WARN): ", LogLevel::Warn},
        {"(ERROR): ", LogLevel::Error}, {"(FATAL): ", LogLevel::Fatal}};
    LogLevel level = stderrStream ? LogLevel::Warn : LogLevel::Info;
    std::string key = text;
    for (const auto& [prefix, prefixLevel] : prefixes) {
        if (text.rfind(prefix, 0) == 0) {
            level = prefixLevel;
            key = text.substr(std::char_traits<char>::length(prefix));
            break;
        }
    }
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = unified.begin(); it != unified.end(); ++it) {
        if (it->first == key) {
            unified.erase(it);
            return;
        }
    }
    console.push_back({level, std::move(text), std::move(key), now});
}

void SimulatorLogMerge::flush(double now, bool all) {
    if (now < 0.0) now = DeviceLogPump::steadySeconds();
    std::lock_guard<std::mutex> lock(mutex);
    while (!console.empty() && (all || now - console.front().at >= hold)) {
        emit(console.front().level, console.front().text);
        console.pop_front();
    }
    while (!unified.empty() && (all || now - unified.front().second >= hold)) unified.pop_front();
}

}  // namespace jasperengine::editor::platform
