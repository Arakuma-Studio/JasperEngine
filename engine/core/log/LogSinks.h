// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Logger.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine {

    class System;

    // ------------------------------------------------------------ text helpers

    // "(ERROR): " ... the game console's prefix ("" for Info)
    JASPERENGINE_API const char* logConsolePrefix(LogLevel level);
    // Whether text output names the category: not for the ones whose output predates
    // categories (engine, editor, lua, build), so those lines read as before
    JASPERENGINE_API bool logShowsCategory(const LogCategory& category);
    // The five-letter label of editor.log lines: "INFO ", "WARN ", "ERROR", "OK   ", "BUILD",
    // "DEBUG", "TRACE", "FATAL" (build output is BUILD whatever its level)
    JASPERENGINE_API const char* logFileLabel(const LogRecord& record);
    // "YYYY-MM-DD HH:MM:SS", local time
    JASPERENGINE_API void logFormatTimestamp(double wallTime, char out[20]);

    // ------------------------------------------------------------ sinks

    // stdout / stderr (file descriptors 1 and 2, whatever they are redirected to at the time).
    // Game:   "(ERROR): msg" ("msg" for Info), Warn and above to stderr; Async, per batch.
    // Editor: "[ERROR] msg" (the editor's CLI and headless output), Error and above and
    //         build output to stderr; Inline, a line at a time, so it stays in order with what
    //         the CLI prints itself (JSON on stdout, reports).
    class JASPERENGINE_API ConsoleSink : public LogSink {
    public:
        enum class Style { Game, Editor };

        explicit ConsoleSink(Style style);
        void write(const LogRecord& record) override;
        void flush() override;
        Delivery delivery() const override { return style == Style::Editor ? Delivery::Inline : Delivery::Async; }
        const char* name() const override { return "console"; }

    private:
        Style style;
        std::string pending;
        bool pendingToError = false;
    };

    // A log file that rotates: past maxBytes the file becomes path.1 (path.1 becomes path.2
    // ... up to maxFiles kept), at open and while running. A failed rename turns rotation
    // off for good and the file keeps growing (nothing is lost, nothing retried per write).
    // Lines: "YYYY-MM-DD HH:MM:SS [ERROR] msg", then " [category]" (logShowsCategory) and
    // " (file:line)" (showSource), then the symbolized frames of a backtrace.
    class JASPERENGINE_API RotatingFileSink : public LogSink {
    public:
        struct Options {
            std::string path;                    // UTF-8
            size_t maxBytes = 2 * 1024 * 1024;
            int maxFiles = 3;                    // rotated files kept: path.1 ... path.maxFiles
            std::string banner;                  // written after opening ("" for none)
            bool showSource = true;
            // test seam: renames a file, true on success (default: the OS rename)
            std::function<bool(const std::string& from, const std::string& to)> rename;
        };

        explicit RotatingFileSink(Options options);
        ~RotatingFileSink() override;
        void write(const LogRecord& record) override;
        void flush() override;
        const char* name() const override { return "file"; }

        bool isOpen() const;
        const std::string& path() const { return options.path; }
        bool rotationDisabled() const { return rotationOff; }
        void setShowSource(bool show) { showSource.store(show, std::memory_order_relaxed); }

    private:
        Options options;
        std::atomic<bool> showSource{true};
        intptr_t handle = -1;
        size_t fileBytes = 0;
        bool rotationOff = false;
        std::string pending;
        std::string scratch;
        int64_t stampSecond = -1;
        char stamp[20] = {0};

        bool openFile();
        void closeFile();
        void rotate();
        void writeOut(const char* data, size_t size);
    };

    // The newest records in memory, for the editor's Console and the MCP read_console /
    // wait_for_log tools (games optionally). Inline, so a read right after a log call sees
    // it. One byte arena, bounded by bytes and by entries; no allocation per record.
    class JASPERENGINE_API MemorySink : public LogSink {
    public:
        struct Entry {
            uint64_t id = 0;
            LogLevel level = LogLevel::Info;
            const LogCategory* category = nullptr;
            uint32_t flags = 0;
            double wallTime = 0.0;
            uint64_t frame = 0;
            std::string threadName;
            std::string file;
            std::string function;
            uint32_t line = 0;
            std::string text;
        };
        // What a filter sees before an entry is copied out (views valid during the call)
        struct View {
            uint64_t id;
            LogLevel level;
            const LogCategory* category;
            uint32_t flags;
            std::string_view file;
            std::string_view text;
        };

        MemorySink(size_t maxBytes, size_t maxEntries);
        void write(const LogRecord& record) override;
        Delivery delivery() const override { return Delivery::Inline; }
        const char* name() const override { return "memory"; }

        // Entries with id > afterId, oldest first; at most max (0: no limit) of those the
        // filter keeps
        std::vector<Entry> since(uint64_t afterId, size_t max = 0, const std::function<bool(const View&)>& filter = {}) const;
        // The newest entry's id (0 when empty)
        uint64_t lastId() const;
        size_t size() const;
        // Drops every entry; ids keep counting (they are the logger's)
        void clear();

    private:
        struct Slot {
            uint64_t id;
            double wallTime;
            uint64_t frame;
            size_t offset;       // in the arena: text, file, function, thread name
            size_t skipped;      // arena bytes left unused before it (the end, when it wrapped)
            uint32_t textLength;
            uint32_t line;
            uint32_t flags;
            uint16_t fileLength;
            uint16_t functionLength;
            uint8_t threadNameLength;
            uint8_t level;
            uint8_t category;
        };

        mutable std::mutex mutex;
        std::vector<char> arena;
        size_t arenaHead = 0;    // the oldest entry's bytes
        size_t arenaTail = 0;    // where the next goes
        size_t arenaUsed = 0;
        std::vector<Slot> slots;  // a ring of maxEntries
        size_t first = 0;
        size_t count = 0;

        void dropOldest();
        Entry toEntry(const Slot& slot) const;
    };

    // Calls a function per record (the MCP log stream, tests)
    class JASPERENGINE_API CallbackSink : public LogSink {
    public:
        using Callback = std::function<void(const LogRecord&)>;

        CallbackSink(std::string sinkName, Callback callback, Delivery delivery = Delivery::Async);
        void write(const LogRecord& record) override;
        Delivery delivery() const override { return mode; }
        const char* name() const override { return sinkName.c_str(); }

    private:
        std::string sinkName;
        Callback callback;
        Delivery mode;
    };

    // Forwards every record to System::platformLog, for a System that overrides it to see
    // the game's log (install it from the override of System::installLogSinks)
    class JASPERENGINE_API PlatformSink : public LogSink {
    public:
        explicit PlatformSink(System& system);
        void write(const LogRecord& record) override;
        const char* name() const override { return "platform"; }

    private:
        System& system;
        std::string scratch;
    };

#if defined(__ANDROID__)
    // logcat, tag "JasperEngine", priority by level: W JasperEngine: [render] ...
    class JASPERENGINE_API AndroidLogSink : public LogSink {
    public:
        void write(const LogRecord& record) override;
        const char* name() const override { return "logcat"; }

    private:
        std::string scratch;
    };
#endif

#if defined(__EMSCRIPTEN__)
    // The browser's console, warnings and errors flagged so devtools colour them. Inline:
    // the web delivers synchronously.
    class JASPERENGINE_API EmscriptenConsoleSink : public LogSink {
    public:
        void write(const LogRecord& record) override;
        Delivery delivery() const override { return Delivery::Inline; }
        const char* name() const override { return "browser"; }

    private:
        std::string scratch;
    };
#endif

#if defined(_WIN32)
    // The debugger's output window (OutputDebugString), while a debugger is attached
    class JASPERENGINE_API DebuggerSink : public LogSink {
    public:
        void write(const LogRecord& record) override;
        const char* name() const override { return "debugger"; }

    private:
        std::string scratch;
        std::wstring wideScratch;
    };
#endif

#if defined(__APPLE__)
    // The unified log (os_log): subsystem the bundle id, one os_log category per category
    class JASPERENGINE_API AppleLogSink : public LogSink {
    public:
        AppleLogSink();
        void write(const LogRecord& record) override;
        const char* name() const override { return "oslog"; }

    private:
        std::string subsystem;
        void* logs[LogCategory::MAX_CATEGORIES] = {nullptr};
        std::string scratch;
    };
#endif

    // What a game's log goes to by default (System::installLogSinks): the console on
    // desktop (plus the debugger on Windows and os_log on Apple), logcat on Android, the
    // browser console on the web; with fileLog also <userdata>/logs/player.log on desktop
    struct PlatformLogOptions {
        bool console = true;
        bool fileLog = false;
        std::string userDataPath;
        size_t fileBytes = 1024 * 1024;
        int fileCount = 2;
    };
    JASPERENGINE_API void installPlatformLogSinks(const PlatformLogOptions& options);

}
