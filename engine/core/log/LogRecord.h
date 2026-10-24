// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "LogCategory.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace jasperengine {

    // Where a JE_LOG_* call is (copied into the record: file up to 160 bytes, function up to 64)
    struct LogSource {
        const char* file;
        const char* function;
        uint32_t line;
    };

    enum LogFlags : uint32_t {
        LogFlagSuccess = 1u << 0,      // Out::success: shown as a success (Info level)
        LogFlagNoBacktrace = 1u << 1,  // never captures frames (Lua: the traceback is in the text)
        LogFlagLua = 1u << 2,          // logged by Lua (print, Log.*, a Lua error)
        LogFlagInternal = 1u << 3,     // the logger's own notices (drops, suppression): never rate limited
        LogFlagTruncated = 1u << 4,    // the text was cut at MAX_TEXT_BYTES
        LogFlagNoRateLimit = 1u << 5,  // never held back by the rate limit (build output)
    };

    // One log record as a sink sees it. Every view is valid during LogSink::write only.
    struct LogRecord {
        static constexpr size_t MAX_TEXT_BYTES = 64 * 1024;
        static constexpr size_t MAX_FILE_BYTES = 160;
        static constexpr size_t MAX_FUNCTION_BYTES = 64;
        static constexpr size_t MAX_THREAD_NAME_BYTES = 32;
        static constexpr size_t MAX_FRAMES = 32;

        uint64_t id = 0;               // process-wide, increasing from 1, assigned when logged
        LogLevel level = LogLevel::Info;
        const LogCategory* category = nullptr;
        uint32_t flags = 0;
        double wallTime = 0.0;         // seconds since the Unix epoch
        uint64_t frame = 0;            // engine frame being run
        uint64_t threadId = 0;
        std::string_view threadName;
        std::string_view file;         // empty without a source
        std::string_view function;
        uint32_t line = 0;
        std::string_view text;
        const void* const* frames = nullptr;   // raw return addresses (Error and Fatal from C++)
        uint32_t frameCount = 0;

        bool hasSource() const { return !file.empty(); }
    };

}
