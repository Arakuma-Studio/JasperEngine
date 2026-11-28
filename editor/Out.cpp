// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Out.h"
#include "util/BuildDiagnostics.h"
#include "util/SafeWrite.h"
#include "EditorHost.h"
#include "EditorVersion.h"
#include "log/LogSinks.h"
#include "util/FileUtils.h"
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::editor;

ConsoleWindow* Out::consoleWindow = nullptr;

namespace je_out {}

namespace je_out {

// Several sessions fit, so a crash log survives the restarts before it is sent.
constexpr size_t kMaxLogFileSize = 2 * 1024 * 1024;
constexpr int kLogFiles = 3;
// Bounded by count and by size: build output can make single messages large.
constexpr size_t kMaxLogBufferBytes = 16 * 1024 * 1024;
constexpr size_t kEditorQueueBytes = 4 * 1024 * 1024;

std::mutex sinkMutex;
MemorySink* memoryStore = nullptr;              // owned by the Logger while memorySink is set
Logger::SinkId memorySink = 0;
Logger::SinkId consoleSink = 0;
Logger::SinkId fileSink = 0;
RotatingFileSink* fileStore = nullptr;          // owned by the Logger while fileSink is set
std::filesystem::path logFilePath;
bool fileShowsSource = true;
std::string levelSpec;
LogLevel fileLevel = LogLevel::Info;

bool outIsProblem(LogType type) {
    return type == LogType::Warning || type == LogType::Error || type == LogType::Build;
}

std::string outFormat(const char* fmt, va_list args) {
    if (!fmt) return std::string();
    char stackBuffer[1024];
    va_list sizing;
    va_copy(sizing, args);
    const int size = std::vsnprintf(stackBuffer, sizeof(stackBuffer), fmt, sizing);
    va_end(sizing);
    if (size <= 0) return std::string();
    if (static_cast<size_t>(size) < sizeof(stackBuffer)) return std::string(stackBuffer, static_cast<size_t>(size));
    std::string result(static_cast<size_t>(size) + 1, '\0');
    std::vsnprintf(&result[0], result.size(), fmt, args);
    result.resize(static_cast<size_t>(size));
    return result;
}

// rateKey: the call's format string; a pass-through "%s" groups by text instead
void logEditor(LogLevel level, const std::string& message, uint32_t flags = 0, const char* rateKey = nullptr) {
    if (rateKey && rateKey[0] == '%' && rateKey[1] == 's' && rateKey[2] == '\0') rateKey = nullptr;
    Logger::write(level, logCategoryEditor(), message, nullptr, flags, rateKey);
}

void logBuildLine(const std::string& message) {
    // compiler output in bursts is the point of a build: never rate limited
    Logger::write(Out::buildLineLevel(message), logCategoryBuild(), message, nullptr,
                  LogFlagNoRateLimit | LogFlagNoBacktrace);
}

}

void Out::startLogging() {
    Logger::Config config;
    config.queueBytes = je_out::kEditorQueueBytes;
    config.backtraceOnError = true;
    Logger::init(config);
    Logger::setThreadName("main");
    std::lock_guard<std::mutex> lock(je_out::sinkMutex);
    if (!je_out::memorySink) {
        auto sink = std::make_unique<MemorySink>(je_out::kMaxLogBufferBytes, MAX_LOG_ENTRIES);
        je_out::memoryStore = sink.get();
        je_out::memorySink = Logger::addSink(std::move(sink));
    }
    if (!je_out::consoleSink && !consoleWindow) {
        je_out::consoleSink = Logger::addSink(std::make_unique<ConsoleSink>(ConsoleSink::Style::Editor));
    }
}

void Out::stopLogging() {
    {
        std::lock_guard<std::mutex> lock(je_out::sinkMutex);
        je_out::memoryStore = nullptr;
        je_out::fileStore = nullptr;
        je_out::memorySink = je_out::consoleSink = je_out::fileSink = 0;
        je_out::logFilePath.clear();
    }
    Logger::shutdown();
}

bool Out::setLevelOverride(const std::string& spec, std::string* error) {
    if (!Logger::applyLevelSpec(spec, error)) return false;
    je_out::levelSpec = spec;
    return true;
}

void Out::applyLevelOverride() {
    if (!je_out::levelSpec.empty()) Logger::applyLevelSpec(je_out::levelSpec);
}

const std::string& Out::levelOverride() {
    return je_out::levelSpec;
}

void Out::setLogFile(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(je_out::sinkMutex);

    // Startup settles the config directory twice, the banner is worth writing once.
    if (je_out::fileSink && je_out::logFilePath == path) {
        return;
    }
    if (je_out::fileSink) {
        Logger::removeSink(je_out::fileSink);
        je_out::fileSink = 0;
        je_out::fileStore = nullptr;
    }

    char stamp[20];
    logFormatTimestamp(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(), stamp);
    RotatingFileSink::Options options;
    options.path = FileUtils::pathToUtf8(path);
    options.maxBytes = je_out::kMaxLogFileSize;
    options.maxFiles = je_out::kLogFiles;
    options.showSource = je_out::fileShowsSource;
    options.banner = std::string("\n=== JasperEngine Editor ") + JASPERENGINE_EDITOR_VERSION + " started at " + stamp + " ===\n";
    auto sink = std::make_unique<RotatingFileSink>(std::move(options));
    if (!sink->isOpen()) {
        je_out::logFilePath.clear();
        std::cerr << "[WARNING] Could not open log file: " << path.string() << std::endl;
        return;
    }
    // Info and above by default, as before (Preferences > Logging)
    sink->minLevel.store(je_out::fileLevel, std::memory_order_relaxed);
    je_out::fileStore = sink.get();
    je_out::fileSink = Logger::addSink(std::move(sink));
    je_out::logFilePath = path;
}

Logger::SinkId Out::fileSinkId() {
    std::lock_guard<std::mutex> lock(je_out::sinkMutex);
    return je_out::fileSink;
}

Logger::SinkId Out::memorySinkId() {
    std::lock_guard<std::mutex> lock(je_out::sinkMutex);
    return je_out::memorySink;
}

Logger::SinkId Out::consoleSinkId() {
    std::lock_guard<std::mutex> lock(je_out::sinkMutex);
    return je_out::consoleSink;
}

void Out::setFileShowsSource(bool show) {
    std::lock_guard<std::mutex> lock(je_out::sinkMutex);
    je_out::fileShowsSource = show;
    if (je_out::fileStore) je_out::fileStore->setShowSource(show);
}

void Out::setFileLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(je_out::sinkMutex);
    je_out::fileLevel = level;
    if (je_out::fileSink) Logger::setSinkLevel(je_out::fileSink, level);
}

void Out::setConsoleWindow(ConsoleWindow* consoleWindow) {
    Out::consoleWindow = consoleWindow;
    // with a Console window the terminal stays quiet, as before; without one it gets everything
    std::lock_guard<std::mutex> lock(je_out::sinkMutex);
    if (consoleWindow && je_out::consoleSink) {
        Logger::removeSink(je_out::consoleSink);
        je_out::consoleSink = 0;
    } else if (!consoleWindow && !je_out::consoleSink && Logger::isInitialized()) {
        je_out::consoleSink = Logger::addSink(std::make_unique<ConsoleSink>(ConsoleSink::Style::Editor));
    }
}

ConsoleWindow* Out::getConsoleWindow() {
    return consoleWindow;
}

LogType Out::typeOf(LogLevel level, uint32_t flags, const LogCategory& category) {
    if (&category == &logCategoryBuild()) return LogType::Build;
    if (level >= LogLevel::Error && level != LogLevel::Off) return LogType::Error;
    if (level == LogLevel::Warn) return LogType::Warning;
    if (flags & LogFlagSuccess) return LogType::Success;
    return LogType::Info;
}

LogLevel Out::buildLineLevel(const std::string& line) {
    // one matcher for the Console, the exporter and MCP's build tools
    return BuildDiagnostics::lineLevel(line);
}

std::vector<LogEntry> Out::getLogsSince(uint64_t afterId, size_t max, bool onlyProblems) {
    std::vector<LogEntry> result;
    std::lock_guard<std::mutex> lock(je_out::sinkMutex);
    if (!je_out::memoryStore) return result;
    std::function<bool(const MemorySink::View&)> filter;
    if (onlyProblems) {
        filter = [](const MemorySink::View& view) {
            return je_out::outIsProblem(typeOf(view.level, view.flags, *view.category));
        };
    }
    for (MemorySink::Entry& stored : je_out::memoryStore->since(afterId, max, filter)) {
        LogEntry entry;
        entry.id = stored.id;
        entry.level = stored.level;
        entry.type = typeOf(stored.level, stored.flags, *stored.category);
        entry.category = stored.category->name;
        entry.flags = stored.flags;
        entry.message = std::move(stored.text);
        entry.timeSeconds = stored.wallTime;
        entry.frame = stored.frame;
        entry.threadName = std::move(stored.threadName);
        entry.file = std::move(stored.file);
        entry.line = stored.line;
        result.push_back(std::move(entry));
    }
    return result;
}

uint64_t Out::lastLogId() {
    return Logger::lastId();
}

void Out::clearConsole() {
    {
        std::lock_guard<std::mutex> lock(je_out::sinkMutex);
        if (je_out::memoryStore) je_out::memoryStore->clear();
    }
    // The window is only touched on the main thread
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
    je_out::logEditor(LogLevel::Info, message);
}

void Out::success(const std::string& message) {
    je_out::logEditor(LogLevel::Info, message, LogFlagSuccess);
}

void Out::warning(const std::string& message) {
    je_out::logEditor(LogLevel::Warn, message);
}

void Out::error(const std::string& message) {
    je_out::logEditor(LogLevel::Error, message);
}

void Out::build(const std::string& message) {
    je_out::logBuildLine(message);
}

void Out::info(const char* fmt, ...) {
    if (!Logger::shouldLog(LogLevel::Info, logCategoryEditor())) return;
    va_list args;
    va_start(args, fmt);
    const std::string message = je_out::outFormat(fmt, args);
    va_end(args);
    je_out::logEditor(LogLevel::Info, message, 0, fmt);
}

void Out::success(const char* fmt, ...) {
    if (!Logger::shouldLog(LogLevel::Info, logCategoryEditor())) return;
    va_list args;
    va_start(args, fmt);
    const std::string message = je_out::outFormat(fmt, args);
    va_end(args);
    je_out::logEditor(LogLevel::Info, message, LogFlagSuccess, fmt);
}

void Out::warning(const char* fmt, ...) {
    if (!Logger::shouldLog(LogLevel::Warn, logCategoryEditor())) return;
    va_list args;
    va_start(args, fmt);
    const std::string message = je_out::outFormat(fmt, args);
    va_end(args);
    je_out::logEditor(LogLevel::Warn, message, 0, fmt);
}

void Out::error(const char* fmt, ...) {
    if (!Logger::shouldLog(LogLevel::Error, logCategoryEditor())) return;
    va_list args;
    va_start(args, fmt);
    const std::string message = je_out::outFormat(fmt, args);
    va_end(args);
    je_out::logEditor(LogLevel::Error, message, 0, fmt);
}

void Out::build(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const std::string message = je_out::outFormat(fmt, args);
    va_end(args);
    je_out::logBuildLine(message);
}

namespace {
    // the safe writers' warnings (a restore an undo could not do, a scoped write refused) go to the console
    struct SafeWriteWarnings {
        SafeWriteWarnings() {
            jasperengine::editor::safewrite::setWarningSink([](const std::string& message) { jasperengine::editor::Out::warning(message); });
        }
    } safeWriteWarnings;
}
