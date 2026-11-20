// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DeviceLogPump.h"

#include "thread/ThreadUtil.h"

#include <chrono>

using namespace jasperengine;
using namespace jasperengine::editor::platform;

DeviceLogPump::DeviceLogPump(Sink sink): DeviceLogPump(std::move(sink), Options()) {}

DeviceLogPump::DeviceLogPump(Sink sink, Options options): sink(std::move(sink)), options(options) {}

DeviceLogPump::~DeviceLogPump() {
    stop();
}

double DeviceLogPump::steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void DeviceLogPump::push(LogLevel level, std::string text, double now) {
    if (now < 0.0) now = steadySeconds();
    std::lock_guard<std::mutex> lock(mutex);
    if (windowStart < 0.0 || now - windowStart >= 1.0) {
        if (windowDropped > 0) {
            pending.push_back({LogLevel::Warn, std::to_string(windowDropped) + " log lines dropped (over " +
                                                   std::to_string(options.maxLinesPerSecond) + " lines/s)"});
        }
        windowStart = now;
        windowLines = 0;
        windowDropped = 0;
    }
    if (options.maxLinesPerSecond > 0 && windowLines >= options.maxLinesPerSecond) {
        windowDropped++;
        droppedTotal++;
        return;
    }
    windowLines++;
    pending.push_back({level, std::move(text)});
    if (lastBatch < 0.0) lastBatch = now;
}

void DeviceLogPump::tick(double now, bool force) {
    if (now < 0.0) now = steadySeconds();
    std::vector<DeviceLogLine> batch;
    {
        std::lock_guard<std::mutex> lock(mutex);
        // the drop count of a window nothing followed
        if (force && windowDropped > 0) {
            pending.push_back({LogLevel::Warn, std::to_string(windowDropped) + " log lines dropped (over " +
                                                   std::to_string(options.maxLinesPerSecond) + " lines/s)"});
            windowDropped = 0;
        }
        if (pending.empty()) return;
        if (!force && now - lastBatch < options.interval) return;
        batch.swap(pending);
        lastBatch = now;
    }
    batchCount++;
    if (sink) sink(std::move(batch));
}

void DeviceLogPump::start() {
    std::lock_guard<std::mutex> lock(mutex);
    if (running) return;
    running = true;
    thread = jasperengine::thread::spawnNamed("je-device-log", [this]() {
        std::unique_lock<std::mutex> lock(mutex);
        while (running) {
            wake.wait_for(lock, std::chrono::duration<double>(options.interval));
            if (!running) break;
            lock.unlock();
            tick();
            lock.lock();
        }
    });
}

void DeviceLogPump::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        running = false;
    }
    wake.notify_all();
    if (thread.joinable()) thread.join();
    tick(-1.0, true);
}
