// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A device run's log on its way to the Console (docs/specs/build-and-run-on-device.md R12, the log
// streaming budget): the reader thread pushes lines, which are handed on in batches at most once a
// frame (1/60 s) so the main thread logs them in one go; above the rate limit (5,000 lines/s)
// lines are dropped and one line a second says how many.

#include "log/LogCategory.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine::editor::platform {

    struct DeviceLogLine {
        LogLevel level = LogLevel::Info;
        std::string text;
    };

    class DeviceLogPump {
    public:
        using Sink = std::function<void(std::vector<DeviceLogLine>&& batch)>;

        struct Options {
            double interval = 1.0 / 60.0;   // seconds between batches
            size_t maxLinesPerSecond = 5000; // 0: no limit
        };

        explicit DeviceLogPump(Sink sink);
        DeviceLogPump(Sink sink, Options options);
        ~DeviceLogPump();
        DeviceLogPump(const DeviceLogPump&) = delete;
        DeviceLogPump& operator=(const DeviceLogPump&) = delete;

        // from any thread; `now` in seconds (steady clock when negative)
        void push(LogLevel level, std::string text, double now = -1.0);
        // hands the pending lines on when a batch is due (or always with force); the pump's own
        // thread calls it, a test with its own clock
        void tick(double now = -1.0, bool force = false);
        // a thread that ticks every interval until stop(), which hands the rest on
        void start();
        void stop();

        uint64_t dropped() const { return droppedTotal.load(); }
        uint64_t batches() const { return batchCount.load(); }

        static double steadySeconds();

    private:
        Sink sink;
        Options options;
        std::mutex mutex;
        std::vector<DeviceLogLine> pending;
        double lastBatch = -1.0;
        double windowStart = -1.0;
        size_t windowLines = 0;
        uint64_t windowDropped = 0;
        std::atomic<uint64_t> droppedTotal{0};
        std::atomic<uint64_t> batchCount{0};
        std::thread thread;
        std::condition_variable wake;
        bool running = false;
    };

}
