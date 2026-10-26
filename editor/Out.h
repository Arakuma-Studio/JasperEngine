// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "window/ConsoleWindow.h"
#include "log/Logger.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine {
    class MemorySink;
}

namespace jasperengine::editor {
    // One message as the editor's log keeps it (see Out::getLogsSince)
    struct LogEntry {
        uint64_t id = 0;            // the logger's record id: increasing from 1, never reused
        LogType type = LogType::Info;   // from level, flags and category (Out::typeOf)
        LogLevel level = LogLevel::Info;
        std::string category;       // "editor", "engine", "lua", "build", "render" ...
        uint32_t flags = 0;         // LogFlags
        std::string message;
        double timeSeconds = 0.0;   // wall clock, seconds since the Unix epoch
        uint64_t frame = 0;         // engine frame
        std::string threadName;
        std::string file;           // the call site (JE_LOG_* macros, Lua chunk), empty if none
        uint32_t line = 0;
    };

    // The editor's log, a façade over the engine's Logger (engine/core/log): Out::* log in
    // the "editor" category (build output in "build"), and the Console and the MCP tools
    // read the in-memory store everything goes to (Log::*, Lua print, script errors too).
    class Out {
    private:
        static ConsoleWindow* consoleWindow;

    public:
        // Starts the logger with the editor's sinks: the in-memory store (Console, MCP) and,
        // until a Console window exists, stdout/stderr ("[ERROR] msg"). First thing in main.
        static void startLogging();
        // Drains and flushes everything (normal exit)
        static void stopLogging();

        // --log-level / JASPER_LOG_LEVEL: applied now, and again after the logging
        // preferences (applyLevelOverride), which it overrides. False on a malformed spec.
        static bool setLevelOverride(const std::string& spec, std::string* error);
        static void applyLevelOverride();
        static const std::string& levelOverride();

        static void setConsoleWindow(ConsoleWindow* consoleWindow);
        static ConsoleWindow* getConsoleWindow();

        // Mirrors every message to a rotating file (2 MB x 3): what is left after a crash
        static void setLogFile(const std::filesystem::path& path);
        static Logger::SinkId fileSinkId();
        static Logger::SinkId memorySinkId();
        static Logger::SinkId consoleSinkId();
        // The file line's " (file:line)" suffix, and the file's level (default Info)
        static void setFileShowsSource(bool show);
        static void setFileLevel(LogLevel level);

        // Structured reads of everything logged, from any thread, with or without a Console
        // window (CLI, headless). The newest MAX_LOG_ENTRIES messages (16 MB) are kept.
        static constexpr size_t MAX_LOG_ENTRIES = 20000;
        // The messages with id > afterId, oldest first; with max > 0 only the first max of
        // them (page on with the last id returned). onlyProblems keeps Warning, Error and
        // Build entries. A caller polls with the last id it saw (a cursor); 0 reads from
        // the oldest kept.
        static std::vector<LogEntry> getLogsSince(uint64_t afterId, size_t max = 0, bool onlyProblems = false);
        // The newest message's id, 0 before any. Take it before an action to read only what
        // the action logs.
        static uint64_t lastLogId();
        // Empties the kept messages and the Console window (its Clear button). Ids keep
        // counting, so cursors stay valid.
        static void clearConsole();
        // "info", "success", "warning", "error", "build"
        static const char* logTypeName(LogType type);
        // The Console's type of a record: build output is Build, then Error (and Fatal),
        // Warning, Success; everything else (Trace and Debug too) is Info
        static LogType typeOf(LogLevel level, uint32_t flags, const LogCategory& category);
        // A build output line's level: Error or Warn for compiler, linker and CMake
        // diagnostics, Info otherwise
        static LogLevel buildLineLevel(const std::string& line);

        static void info(const std::string& message);
        static void success(const std::string& message);
        static void warning(const std::string& message);
        static void error(const std::string& message);
        static void build(const std::string& message);

        // printf-style (format checked; a std::string is logged as it is)
        static void info(JE_FORMAT_STRING const char* fmt, ...) JE_PRINTF_FORMAT(1, 2);
        static void success(JE_FORMAT_STRING const char* fmt, ...) JE_PRINTF_FORMAT(1, 2);
        static void warning(JE_FORMAT_STRING const char* fmt, ...) JE_PRINTF_FORMAT(1, 2);
        static void error(JE_FORMAT_STRING const char* fmt, ...) JE_PRINTF_FORMAT(1, 2);
        static void build(JE_FORMAT_STRING const char* fmt, ...) JE_PRINTF_FORMAT(1, 2);
    };
}
