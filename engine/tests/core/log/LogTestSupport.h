// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPER_TEST_LOG_SUPPORT_H
#define JASPER_TEST_LOG_SUPPORT_H

#include "TestSystem.h"
#include "log/Backtrace.h"
#include "log/LogSinks.h"
#include "log/Logger.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine { namespace test {

    // What a RecordingSink keeps of a record
    struct Recorded {
        uint64_t id;
        LogLevel level;
        std::string category;
        uint32_t flags;
        std::string text;
        std::string file;
        uint32_t line;
        uint32_t frameCount;
        std::vector<std::string> frames;   // symbolized, when symbolize
    };

    // Collects the records it gets (Async by default, like most sinks). Thread-safe reads.
    class RecordingSink : public LogSink {
    public:
        explicit RecordingSink(Delivery mode = Delivery::Async, std::string sinkName = "recording")
            : mode(mode), sinkName(std::move(sinkName)) {}

        void write(const LogRecord& record) override {
            Recorded copy{record.id, record.level, record.category->name, record.flags, std::string(record.text),
                          std::string(record.file), record.line, record.frameCount, {}};
            if (symbolize) {
                for (uint32_t i = 0; i < record.frameCount; i++) {
                    std::string frame;
                    Backtrace::describe(record.frames[i], frame);
                    copy.frames.push_back(frame);
                }
            }
            if (onWrite) onWrite(record);
            std::lock_guard<std::mutex> lock(mutex);
            records.push_back(std::move(copy));
        }
        Delivery delivery() const override { return mode; }
        const char* name() const override { return sinkName.c_str(); }

        std::vector<Recorded> taken() const {
            std::lock_guard<std::mutex> lock(mutex);
            return records;
        }
        std::vector<Recorded> containing(const std::string& text) const {
            std::vector<Recorded> found;
            for (const Recorded& r : taken()) {
                if (r.text.find(text) != std::string::npos) found.push_back(r);
            }
            return found;
        }

        bool symbolize = false;
        std::function<void(const LogRecord&)> onWrite;

    private:
        Delivery mode;
        std::string sinkName;
        mutable std::mutex mutex;
        std::vector<Recorded> records;
    };

    // Adds a sink for the scope of a test
    template<typename Sink>
    class ScopedSink {
    public:
        explicit ScopedSink(std::unique_ptr<Sink> owned): sink(owned.get()), id(Logger::addSink(std::move(owned))) {}
        ScopedSink(const ScopedSink&) = delete;
        ScopedSink& operator=(const ScopedSink&) = delete;
        ~ScopedSink() { Logger::removeSink(id); }
        Sink* operator->() const { return sink; }
        Sink& operator*() const { return *sink; }
        Logger::SinkId sinkId() const { return id; }

    private:
        Sink* sink;
        Logger::SinkId id;
    };

    inline ScopedSink<RecordingSink> recordingSink(LogSink::Delivery mode = LogSink::Delivery::Async) {
        return ScopedSink<RecordingSink>(std::make_unique<RecordingSink>(mode));
    }

    // A logger of its own for the test (queue size, no sinks but TestSystem's), and the test
    // binary's logger back afterwards
    class FreshLogger {
    public:
        explicit FreshLogger(size_t queueBytes = 1024 * 1024, bool async = true) {
            Logger::shutdown();
            Logger::Config config;
            config.queueBytes = queueBytes;
            config.asyncDrain = async;
            Logger::init(config);
        }
        ~FreshLogger() {
            Logger::shutdown();
            Logger::Config config;
            Logger::init(config);
            TestSystem::get().installLogSinks();
        }
    };

} }

#endif
