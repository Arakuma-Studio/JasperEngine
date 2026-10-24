// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "LogRecord.h"

#include <atomic>
#include <cstdint>

namespace jasperengine {

    // Where records go. Inline sinks run on the thread that logs, under the logger's lock
    // (so they see records in id order); Async ones on the logger's drain thread, in
    // batches followed by flush(). Every sink also gets records of a Fatal call
    // synchronously, from the thread that logged it. A sink is only ever called by one
    // thread at a time.
    class JASPERENGINE_API LogSink {
    public:
        enum class Delivery { Inline, Async };

        virtual ~LogSink();

        virtual void write(const LogRecord& record) = 0;
        // After a batch, and when the logger flushes: whatever is buffered reaches the OS
        virtual void flush() {}
        virtual Delivery delivery() const { return Delivery::Async; }
        // A short name for tools and settings: "console", "file", "memory", "mcp" ...
        virtual const char* name() const = 0;

        // Initial filters; Logger::setSinkLevel changes the level later (and can give a
        // category a level of its own for this sink)
        std::atomic<LogLevel> minLevel{LogLevel::Trace};
        std::atomic<uint64_t> categoryMask{~0ull};
    };

}
