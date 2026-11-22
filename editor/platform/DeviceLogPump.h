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
#include <deque>
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

    // An iOS Simulator app's two logs (R12): `log stream --style compact` (the unified log, with
    // the line's level and category) and the launch's stdout and stderr files, read on as they
    // grow. The engine writes each of its lines to both, so a line is passed on once: the unified
    // log's copy, as it comes; a console line waits up to `hold` seconds for its unified copy and
    // goes on alone only when none came (a printf past the engine's log, or no log stream).
    class SimulatorLogMerge {
    public:
        using Emit = std::function<void(LogLevel level, const std::string& text)>;

        // subsystem: the app's bundle id, which the engine's unified log lines carry
        SimulatorLogMerge(std::string subsystem, Emit emit, double hold = 3.0);

        // a line of `log stream --style compact`
        void unifiedLine(const std::string& line, double now = -1.0);
        // a line of the app's stdout (stderr: true for its stderr)
        void consoleLine(const std::string& line, bool stderrStream, double now = -1.0);
        // lines held back that are due (all: every one, at the run's end)
        void flush(double now = -1.0, bool all = false);

    private:
        struct Held {
            LogLevel level = LogLevel::Info;
            std::string text;   // as the console has it
            std::string key;    // the message, as the unified log's line gives it
            double at = 0.0;
        };
        std::string subsystem;
        Emit emit;
        double hold;
        std::mutex mutex;
        std::deque<Held> console;                           // console lines waiting for their copy
        std::deque<std::pair<std::string, double>> unified; // unified lines whose console copy did not come yet
    };

}
