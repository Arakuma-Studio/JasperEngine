// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Out.h"
#include "EditorHost.h"
#include "EditorVersion.h"
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>

using namespace jasperengine::editor;

ConsoleWindow* Out::consoleWindow = nullptr;

namespace {

// Several sessions fit, so a crash log survives the restarts before it is sent.
constexpr std::uintmax_t kMaxLogFileSize = 2 * 1024 * 1024;

std::mutex logFileMutex;
std::ofstream logFileStream;
std::filesystem::path logFilePath;

const char* logTypeLabel(LogType type) {
    switch (type) {
        case LogType::Warning: return "WARN ";
        case LogType::Error:   return "ERROR";
        case LogType::Success: return "OK   ";
        case LogType::Build:   return "BUILD";
        default:               return "INFO ";
    }
}

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
    return buffer;
}

// Every message as logged, for Out::getLogsSince. Bounded by count and by size: build
// output can make single messages large.
constexpr size_t kMaxLogBufferBytes = 16 * 1024 * 1024;

std::mutex logBufferMutex;
std::deque<LogEntry> logBuffer;
size_t logBufferBytes = 0;
uint64_t lastLogBufferId = 0;

void addToLogBuffer(LogType type, const std::string& message) {
    const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(logBufferMutex);
    LogEntry entry;
    entry.id = ++lastLogBufferId;
    entry.type = type;
    entry.message = message;
    entry.timeSeconds = now;
    logBufferBytes += entry.message.size();
    logBuffer.push_back(std::move(entry));
    while (logBuffer.size() > 1 && (logBuffer.size() > Out::MAX_LOG_ENTRIES || logBufferBytes > kMaxLogBufferBytes)) {
        logBufferBytes -= logBuffer.front().message.size();
        logBuffer.pop_front();
    }
}

bool isProblem(LogType type) {
    return type == LogType::Warning || type == LogType::Error || type == LogType::Build;
}

void writeToLogFile(LogType type, const std::string& message) {
    std::lock_guard<std::mutex> lock(logFileMutex);
    if (!logFileStream.is_open()) {
        return;
    }
    // Flushed per line, the lines just before a crash are the ones worth having.
    logFileStream << timestamp() << " [" << logTypeLabel(type) << "] " << message << std::endl;
}

}

void Out::setLogFile(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(logFileMutex);

    // Startup settles the config directory twice, the banner is worth writing once.
    if (logFileStream.is_open()) {
        if (logFilePath == path) {
            return;
        }
        logFileStream.close();
    }

    std::error_code ec;
    const std::uintmax_t existingSize = std::filesystem::file_size(path, ec);
    if (!ec && existingSize > kMaxLogFileSize) {
        std::filesystem::path rotated = path;
        rotated += ".1";
        std::filesystem::remove(rotated, ec);
        std::filesystem::rename(path, rotated, ec);
    }

    logFileStream.open(path, std::ios::out | std::ios::app);
    if (!logFileStream.is_open()) {
        logFilePath.clear();
        std::cerr << "[WARNING] Could not open log file: " << path.string() << std::endl;
        return;
    }

    logFilePath = path;
    logFileStream << "\n=== JasperEngine Editor " << JASPERENGINE_EDITOR_VERSION << " started at "
                  << timestamp() << " ===" << std::endl;
}

void Out::logMessage(LogType type, const std::string& message, const char* fallbackPrefix, std::ostream& fallbackStream) {
    // Before the hop to the main thread, so a worker's last message still reaches the file.
    writeToLogFile(type, message);
    addToLogBuffer(type, message);

    if (ConsoleWindow* window = Out::getConsoleWindow()) {
        EditorHost& host = getEditorHost();
        if (host.isMainThread()) {
            window->addLog(type, message);
            return;
        }

        host.enqueueMainThreadTask([type, message]() {
            if (ConsoleWindow* window = Out::getConsoleWindow()) {
                window->addLog(type, message);
            }
        });
    } else {
        fallbackStream << fallbackPrefix << message << std::endl;
    }
}

void Out::setConsoleWindow(ConsoleWindow* consoleWindow) {
    Out::consoleWindow = consoleWindow;
}

ConsoleWindow* Out::getConsoleWindow() {
    return consoleWindow;
}

std::vector<LogEntry> Out::getLogsSince(uint64_t afterId, size_t max, bool onlyProblems) {
    std::vector<LogEntry> result;
    std::lock_guard<std::mutex> lock(logBufferMutex);
    // ids increase along the buffer: skip to the first one after the cursor
    auto it = std::upper_bound(logBuffer.begin(), logBuffer.end(), afterId,
        [](uint64_t id, const LogEntry& entry) { return id < entry.id; });
    for (; it != logBuffer.end(); ++it) {
        if (onlyProblems && !isProblem(it->type)) continue;
        result.push_back(*it);
        if (max > 0 && result.size() >= max) break;
    }
    return result;
}

uint64_t Out::lastLogId() {
    std::lock_guard<std::mutex> lock(logBufferMutex);
    return lastLogBufferId;
}

void Out::clearConsole() {
    {
        std::lock_guard<std::mutex> lock(logBufferMutex);
        logBuffer.clear();
        logBufferBytes = 0;
    }
    // The window is only touched on the main thread; a message a worker logged just
    // before may still be on its way to it
    EditorHost& host = getEditorHost();
    if (host.isMainThread()) {
        if (ConsoleWindow* window = getConsoleWindow()) window->clear();
    } else {
        host.enqueueMainThreadTask([]() {
            if (ConsoleWindow* window = Out::getConsoleWindow()) window->clear();
        });
    }
}

const char* Out::logTypeName(LogType type) {
    switch (type) {
        case LogType::Warning: return "warning";
        case LogType::Error:   return "error";
        case LogType::Success: return "success";
        case LogType::Build:   return "build";
        default:               return "info";
    }
}

void Out::info(const std::string& message) {
    logMessage(LogType::Info, message, "[INFO] ", std::cout);
}

void Out::success(const std::string& message) {
    logMessage(LogType::Success, message, "[SUCCESS] ", std::cout);
}

void Out::warning(const std::string& message) {
    logMessage(LogType::Warning, message, "[WARNING] ", std::cout);
}

void Out::error(const std::string& message) {
    logMessage(LogType::Error, message, "[ERROR] ", std::cerr);
}

void Out::build(const std::string& message) {
    logMessage(LogType::Build, message, "[BUILD] ", std::cerr);
}
