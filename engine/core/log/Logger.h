// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Export.h"
#include "LogCategory.h"
#include "LogFormatCheck.h"
#include "LogRecord.h"
#include "LogSink.h"

#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// JE_LOG_TRACE / JE_LOG_DEBUG sites below this level are compiled out (0 Trace, 1 Debug,
// 2 Info): Debug in debug builds, Info in release builds
#ifndef JE_LOG_MIN_COMPILED_LEVEL
    #ifdef NDEBUG
        #define JE_LOG_MIN_COMPILED_LEVEL 2
    #else
        #define JE_LOG_MIN_COMPILED_LEVEL 1
    #endif
#endif

// JE_LOG_WARN(logCategoryRender(), "%d draws dropped", n): formats and logs with the call's
// file, line and function only when a sink takes the level for the category; otherwise the
// arguments are not even evaluated.
#define JE_LOG_AT(level, category, ...) \
    do { \
        ::jasperengine::LogCategory& je_log_category_ = (category); \
        if (::jasperengine::Logger::shouldLog(level, je_log_category_)) { \
            const ::jasperengine::LogSource je_log_source_{__FILE__, __func__, static_cast<uint32_t>(__LINE__)}; \
            ::jasperengine::Logger::writef(level, je_log_category_, &je_log_source_, __VA_ARGS__); \
        } \
    } while (0)

// a compiled-out site: still type-checked (format included), never run
#define JE_LOG_DISCARD(level, category, ...) \
    do { \
        if (false) ::jasperengine::Logger::writef(level, (category), nullptr, __VA_ARGS__); \
    } while (0)

#if JE_LOG_MIN_COMPILED_LEVEL <= 0
    #define JE_LOG_TRACE(category, ...) JE_LOG_AT(::jasperengine::LogLevel::Trace, category, __VA_ARGS__)
#else
    #define JE_LOG_TRACE(category, ...) JE_LOG_DISCARD(::jasperengine::LogLevel::Trace, category, __VA_ARGS__)
#endif
#if JE_LOG_MIN_COMPILED_LEVEL <= 1
    #define JE_LOG_DEBUG(category, ...) JE_LOG_AT(::jasperengine::LogLevel::Debug, category, __VA_ARGS__)
#else
    #define JE_LOG_DEBUG(category, ...) JE_LOG_DISCARD(::jasperengine::LogLevel::Debug, category, __VA_ARGS__)
#endif
#define JE_LOG_INFO(category, ...) JE_LOG_AT(::jasperengine::LogLevel::Info, category, __VA_ARGS__)
#define JE_LOG_WARN(category, ...) JE_LOG_AT(::jasperengine::LogLevel::Warn, category, __VA_ARGS__)
#define JE_LOG_ERROR(category, ...) JE_LOG_AT(::jasperengine::LogLevel::Error, category, __VA_ARGS__)
// Logs synchronously to every sink and flushes; the caller decides whether to abort
#define JE_LOG_FATAL(category, ...) JE_LOG_AT(::jasperengine::LogLevel::Fatal, category, __VA_ARGS__)

// JE_LOG_ONCE(Warn, logCategoryRender(), "..."): this call site logs the first time only
#define JE_LOG_ONCE(level, category, ...) \
    do { \
        static ::std::atomic<bool> je_log_once_{false}; \
        if (!je_log_once_.load(::std::memory_order_relaxed) && !je_log_once_.exchange(true)) { \
            JE_LOG_AT(::jasperengine::LogLevel::level, category, __VA_ARGS__); \
        } \
    } while (0)

// JE_LOG_EVERY(5.0, Warn, logCategoryAudio(), "..."): this call site logs at most once per
// period (seconds)
#define JE_LOG_EVERY(seconds, level, category, ...) \
    do { \
        static ::std::atomic<int64_t> je_log_next_{0}; \
        if (::jasperengine::Logger::passEvery(je_log_next_, (seconds))) { \
            JE_LOG_AT(::jasperengine::LogLevel::level, category, __VA_ARGS__); \
        } \
    } while (0)

// A category of one's own: JE_LOG_CATEGORY(logCategoryNet, "net", ::jasperengine::LogLevel::Info)
// defines logCategoryNet(), registered with the engine on first use (the same name always
// gives the same category, from any library)
#define JE_LOG_CATEGORY(Id, name, defaultLevel) \
    inline ::jasperengine::LogCategory& Id() { \
        static ::jasperengine::LogCategory& je_log_category_ = ::jasperengine::Logger::registerCategory(name, defaultLevel); \
        return je_log_category_; \
    }

namespace jasperengine {

    // The process-wide logger (engine/core/log). Records are formatted on the thread that
    // logs; Inline sinks get them right there, Async sinks from a drain thread ("je-log")
    // through a bounded byte queue. Before init() (and after shutdown()) records go
    // synchronously to stderr. Web builds deliver every sink inline. Every function is
    // thread-safe; all state lives in the engine library.
    class JASPERENGINE_API Logger {
    public:
        struct Config {
            LogLevel defaultLevel = LogLevel::Info;   // of every category not set otherwise
            size_t queueBytes = 1024 * 1024;           // editor 4 MB, mobile and web 256 KB
            bool asyncDrain = true;                   // false: every sink inline (no thread)
            bool backtraceOnError = false;            // Error and Fatal from C++ capture frames
        };

        using SinkId = uint32_t;

        struct SinkInfo {
            SinkId id = 0;
            std::string name;
            LogSink::Delivery delivery = LogSink::Delivery::Async;
            LogLevel level = LogLevel::Trace;
            // categories this sink filters at a level of their own
            std::vector<std::pair<const LogCategory*, LogLevel>> categoryLevels;
        };

        struct Stats {
            uint64_t records = 0;             // logged (ids handed out)
            uint64_t dropped = 0;             // lost to a full queue
            uint64_t suppressed = 0;          // held back by the rate limit
            uint64_t backtracesOmitted = 0;   // past the 10 per second cap
            size_t queueCapacity = 0;
            size_t queueUsed = 0;
            size_t queueHighWater = 0;
            bool async = false;
        };

        static void init(const Config& config);
        static bool isInitialized();
        // Drains the queue, flushes and removes every sink, stops the drain thread. Later
        // records go to stderr again.
        static void shutdown();

        static bool shouldLog(LogLevel level, const LogCategory& category) noexcept {
            return static_cast<uint8_t>(level) >= category.effective.load(std::memory_order_relaxed);
        }

        static void writef(LogLevel level, const LogCategory& category, const LogSource* source,
                           JE_FORMAT_STRING const char* format, ...) JE_PRINTF_FORMAT(4, 5);
        static void vwritef(LogLevel level, const LogCategory& category, const LogSource* source,
                            const char* format, va_list args) JE_PRINTF_FORMAT(4, 0);
        // Logs text as it is (no formatting); flags: LogFlags. rateKey groups records for the
        // rate limit (a call site's format string); 0 groups by the text's first 64 bytes.
        static void write(LogLevel level, const LogCategory& category, std::string_view text,
                          const LogSource* source = nullptr, uint32_t flags = 0, const void* rateKey = nullptr);

        static SinkId addSink(std::unique_ptr<LogSink> sink);
        // The sink stops getting records; it is destroyed once a batch in progress is done
        static void removeSink(SinkId id);
        // The first sink of that name, 0 if none
        static SinkId findSink(std::string_view name);
        // With a category: that category's level for this sink only (Off mutes it there);
        // clearSinkCategoryLevel goes back to the sink's level
        static void setSinkLevel(SinkId id, LogLevel level, const LogCategory* category = nullptr);
        static void clearSinkCategoryLevel(SinkId id, const LogCategory& category);
        static LogLevel sinkLevel(SinkId id, const LogCategory* category = nullptr);
        static std::vector<SinkInfo> sinkInfo();

        // A category's minimum level: records below it are never formatted
        static void setCategoryLevel(LogCategory& category, LogLevel level);
        static LogLevel categoryLevel(const LogCategory& category);
        // Every category, and those registered later
        static void setDefaultLevel(LogLevel level);
        static LogLevel defaultLevel();
        // Registers (or finds) a category by name; at most LogCategory::MAX_CATEGORIES, past
        // that the engine category is returned (and a warning logged once)
        static LogCategory& registerCategory(std::string_view name, LogLevel defaultLevel);
        static LogCategory* findCategory(std::string_view name);
        static size_t categoryCount();
        static LogCategory& categoryAt(size_t index);
        // "warn" or "info,render=debug,physics=error": the first item without "=" is every
        // category's level (setDefaultLevel). Unknown categories are registered. False (and
        // nothing changed) on a malformed spec, with the reason in error.
        static bool applyLevelSpec(std::string_view spec, std::string* error = nullptr);

        static void setBacktraceOnError(bool enabled);
        static bool backtraceOnError();

        // Blocks until every record logged before it reached the sinks, then flushes them.
        // A no-op on the drain thread.
        static void flush();
        // For crash handlers (signals, SEH filters, std::terminate): waits until what was
        // queued at entry is written, using only atomics and sleeps. False on the drain
        // thread, before init or when the deadline passes; unwritten (optional) gets how
        // many records were still queued.
        // The rate limit's summaries of what it held back, now rather than when their second is
        // over, and every window starts anew. Then as flush(): a summary logged after this says
        // what was logged after it (a test's log capture).
        static void flushSummaries();
        static bool flushForCrash(std::chrono::milliseconds timeout, uint64_t* unwritten = nullptr) noexcept;

        // The engine frame records are stamped with (Engine::systemDraw)
        static void setFrame(uint64_t frame);
        // The calling thread's name in records (default: the OS name of the thread)
        static void setThreadName(const char* name);

        static Stats stats();
        // The newest record's id (0 before any): a cursor for "what was logged after this"
        static uint64_t lastId();

        // JE_LOG_EVERY's per-site check: true once per period
        static bool passEvery(std::atomic<int64_t>& next, double seconds);
    };

}
