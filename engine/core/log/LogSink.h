// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "LogRecord.h"

#include <atomic>
#include <cstdint>

namespace jasperengine {

    // Where records go. Inline sinks run on the thread that logs, under the logger's lock
    // (so they see records in id order; afterWrite() comes after the lock); Async ones on the logger's drain thread, in
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

        // Inline sinks only (ignored for Async ones), read once by Logger::addSink.
        // A resident sink stays through Logger::shutdown() and also gets the records logged
        // before init() and after shutdown() (on the thread that logs, next to the stderr
        // fallback), so it sees every record of the process: the Log buffer (Log.h).
        virtual bool resident() const { return false; }
        // True: afterWrite() is called on the thread that logged once the logger's lock is
        // released, after write() got one or more records there. What must not run under the
        // logger's lock goes there (the Log buffer's listeners). It may log; it must be
        // thread-safe (threads that log call it at once).
        virtual bool wantsAfterWrite() const { return false; }
        virtual void afterWrite() {}

        // Initial filters; Logger::setSinkLevel changes the level later (and can give a
        // category a level of its own for this sink)
        std::atomic<LogLevel> minLevel{LogLevel::Trace};
        std::atomic<uint64_t> categoryMask{~0ull};
    };

}
