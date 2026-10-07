// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "window/ConsoleWindow.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <memory>
#include <vector>

namespace jasperengine::editor {
    // One message as Out logged it (see Out::getLogsSince)
    struct LogEntry {
        uint64_t id = 0;            // increasing from 1, never reused (not the Console window's ids)
        LogType type = LogType::Info;
        std::string message;
        double timeSeconds = 0.0;   // wall clock, seconds since the Unix epoch
    };

    class Out {
    private:
        static ConsoleWindow* consoleWindow;
        static void logMessage(LogType type, const std::string& message, const char* fallbackPrefix, std::ostream& fallbackStream);

        template<typename... Args>
        static std::string formatMessage(const char* fmt, Args... args) {
            if (!fmt) return std::string();

            int size = snprintf(nullptr, 0, fmt, args...);
            if (size <= 0) return std::string();

            std::string result;
            result.resize(size + 1);

            snprintf(&result[0], size + 1, fmt, args...);

            result.resize(size);
            return result;
        }

    public:
        static void setConsoleWindow(ConsoleWindow* consoleWindow);
        static ConsoleWindow* getConsoleWindow();

        // Mirrors every message to a file, from any thread. The Console window dies with
        // the process, this is what is left after a crash.
        static void setLogFile(const std::filesystem::path& path);

        // Structured reads of everything logged through Out (and so Log::*, Lua print and
        // script errors), from any thread, with or without a Console window (CLI, headless).
        // Out keeps the newest MAX_LOG_ENTRIES messages itself, captured when they are
        // logged, before a worker thread's message hops to the main thread.
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

        // Basic logging methods
        static void info(const std::string& message);
        static void success(const std::string& message);
        static void warning(const std::string& message);
        static void error(const std::string& message);
        static void build(const std::string& message);

        // Overloads for const char* without formatting
        static void info(const char* message) { info(std::string(message)); }
        static void success(const char* message) { success(std::string(message)); }
        static void warning(const char* message) { warning(std::string(message)); }
        static void error(const char* message) { error(std::string(message)); }
        static void build(const char* message) { build(std::string(message)); }

        // Formatted logging methods with variable arguments
        template<typename... Args>
        static void info(const char* fmt, Args... args) {
            info(formatMessage(fmt, args...));
        }

        template<typename... Args>
        static void success(const char* fmt, Args... args) {
            success(formatMessage(fmt, args...));
        }

        template<typename... Args>
        static void warning(const char* fmt, Args... args) {
            warning(formatMessage(fmt, args...));
        }

        template<typename... Args>
        static void error(const char* fmt, Args... args) {
            error(formatMessage(fmt, args...));
        }

        template<typename... Args>
        static void build(const char* fmt, Args... args) {
            build(formatMessage(fmt, args...));
        }

    };
}
