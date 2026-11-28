// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A device run's log on its way to the Console (docs/specs/build-and-run-on-device.md R12, AC12):
// lines are handed on in batches at most once a frame, the main thread logs a batch in one go, and
// above the rate limit lines are dropped with one line saying how many.

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"

#include "Out.h"
#include "platform/DeviceLogPump.h"
#include "platform/DeviceRunner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace DeviceLogPumpTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::platform;

TEST(DeviceLogPump, BatchesLinesOncePerFrame) {
    // the editor's store (main() starts it in the editor)
    if (!Out::memorySinkId()) Out::startLogging();
    size_t lines = 0;
    std::vector<double> batchMs;
    // the main thread's part: a batch into the log (category device)
    DeviceLogPump::Options options;
    options.maxLinesPerSecond = 0;   // this test is about batching: every line arrives
    DeviceLogPump pump([&](std::vector<DeviceLogLine>&& batch) {
        const auto start = std::chrono::steady_clock::now();
        logDeviceLines(batch);
        batchMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        lines += batch.size();
    }, options);
    const uint64_t before = Out::lastLogId();
    // 10,000 lines in one second, each push followed by the pump's tick
    for (int i = 0; i < 10000; i++) {
        const double now = 100.0 + i / 10000.0;
        pump.push(LogLevel::Info, "[Pixel 7] line " + std::to_string(i), now);
        pump.tick(now);
    }
    // the end of the stream hands the rest on (stop() does this)
    pump.tick(101.0, true);
    EXPECT_EQ(lines, 10000u);
    EXPECT_LE(pump.batches(), 60u) << "at most one batch a frame (1/60 s)";
    EXPECT_GE(pump.batches(), 30u);

    // they reach the log, category device
    const std::vector<editor::LogEntry> logged = Out::getLogsSince(before);
    const size_t device = static_cast<size_t>(std::count_if(logged.begin(), logged.end(), [](const editor::LogEntry& e) {
        return e.category == DEVICE_LOG_CATEGORY && e.message.rfind("[Pixel 7] line ", 0) == 0;
    }));
    EXPECT_EQ(device, 10000u);

    // main-thread time per batch: <= 1 ms a frame at 2,000 lines/s (34 lines a batch); here a
    // batch holds about 170 lines, so the budget scales with it
    double worst = 0.0, total = 0.0;
    for (double ms : batchMs) {
        worst = std::max(worst, ms);
        total += ms;
    }
    const double perLine = total / 10000.0;
    std::printf("[bench] DeviceLogPump: %zu batches, worst %.3f ms, %.4f ms a line (budget 1 ms a frame at 2000 lines/s: %.3f ms)\n",
                batchMs.size(), worst, perLine, perLine * 2000.0 / 60.0);
    if (const double slack = editor::test::perfSlack(); slack > 0.0) {
        EXPECT_LT(perLine * 2000.0 / 60.0, 1.0 * slack);
    }
}

TEST(DeviceLogPump, DropsAboveTheRateAndSaysHowMany) {
    std::vector<DeviceLogLine> got;
    DeviceLogPump pump([&](std::vector<DeviceLogLine>&& batch) { got.insert(got.end(), batch.begin(), batch.end()); });
    // 7,000 lines in the first second: 5,000 pass, 2,000 are dropped
    for (int i = 0; i < 7000; i++) pump.push(LogLevel::Info, "line " + std::to_string(i), 10.0 + i / 7000.0);
    // the next second's first line brings the count of the dropped ones
    pump.push(LogLevel::Info, "next", 11.0);
    pump.tick(11.0, true);
    EXPECT_EQ(pump.dropped(), 2000u);
    ASSERT_EQ(got.size(), 5002u);
    EXPECT_EQ(got[4999].text, "line 4999");
    EXPECT_EQ(got[5000].text, "2000 log lines dropped (over 5000 lines/s)");
    EXPECT_EQ(got[5000].level, LogLevel::Warn);
    EXPECT_EQ(got[5001].text, "next");

    // a burst nothing follows still says it at the end
    got.clear();
    DeviceLogPump burst([&](std::vector<DeviceLogLine>&& batch) { got.insert(got.end(), batch.begin(), batch.end()); });
    for (int i = 0; i < 5100; i++) burst.push(LogLevel::Info, "x", 20.0);
    burst.stop();
    ASSERT_FALSE(got.empty());
    EXPECT_EQ(got.back().text, "100 log lines dropped (over 5000 lines/s)");
}

TEST(DeviceLogPump, ItsThreadHandsLinesOnWithoutTicks) {
    std::mutex mutex;
    std::vector<DeviceLogLine> got;
    DeviceLogPump pump([&](std::vector<DeviceLogLine>&& batch) {
        std::lock_guard<std::mutex> lock(mutex);
        got.insert(got.end(), batch.begin(), batch.end());
    });
    pump.start();
    pump.push(LogLevel::Warn, "one");
    pump.push(LogLevel::Error, "two");
    // the state, not the time: until the pump's thread handed both on
    for (int i = 0; i < 500; i++) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (got.size() == 2) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    pump.stop();
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[1].level, LogLevel::Error);
}

// The iOS Simulator gives a run's log twice: the unified log (`log stream --style compact`) and the
// app's stdout and stderr, where the engine's console sink writes the same lines. Each reaches the
// Console once, as the unified log has it: its level and its category (R12).
TEST(DeviceLogPump, SimulatorLinesArriveOnceWithTheirLevelAndCategory) {
    std::vector<std::string> got;
    const auto name = [](LogLevel level) {
        return level == LogLevel::Warn ? "W" : level == LogLevel::Error ? "E" : level == LogLevel::Debug ? "D" : level == LogLevel::Info ? "I" : "?";
    };
    SimulatorLogMerge merge("com.jasperengine.packaging", [&](LogLevel level, const std::string& text) { got.push_back(name(level) + std::string(" ") + text); });
    const std::string at = "2026-11-22 01:13:11.116 ";
    const std::string game = " UnifiedLighting[16994:656a6] ";
    merge.unifiedLine("Filtering the log data using \"process == \"UnifiedLighting\"\"", 10.0);
    merge.unifiedLine("Timestamp               Ty Process[PID:TID]", 10.0);
    // the unified log first (the console file is read every 0.5 s)
    merge.unifiedLine(at + "I " + game + "[com.jasperengine.packaging:engine] GPU quirks: none", 10.1);
    merge.unifiedLine(at + "Df" + game + "[com.jasperengine.packaging:engine] Shader 'tinted' has the legacy shadow interface", 10.1);
    merge.unifiedLine(at + "I " + game + "[com.jasperengine.packaging:lua] [UL] frame 100 (1.66 s): 63 fps", 10.2);
    merge.unifiedLine(at + "Db" + game + "[com.jasperengine.packaging:lua] tick", 10.2);
    merge.unifiedLine(at + "I " + game + "[com.jasperengine.packaging:lua] tick", 10.2);
    merge.unifiedLine(at + "Df" + game + "[com.apple.UIKit:Application] Deactivation reason added: 10", 10.2);
    merge.consoleLine("GPU quirks: none", false, 10.5);
    merge.consoleLine("(WARN): Shader 'tinted' has the legacy shadow interface", true, 10.5);
    merge.consoleLine("[UL] frame 100 (1.66 s): 63 fps", false, 10.5);
    merge.consoleLine("(DEBUG): tick", false, 10.5);
    merge.consoleLine("tick", false, 10.5);
    // the console first: a category other than engine and lua is in its text
    merge.consoleLine("[UL] frame 200 (3.33 s): 59 fps", false, 11.0);
    merge.consoleLine("[render] 31 meshes", false, 11.0);
    merge.unifiedLine(at + "I " + game + "[com.jasperengine.packaging:lua] [UL] frame 200 (3.33 s): 59 fps", 11.4);
    merge.unifiedLine(at + "I " + game + "[com.jasperengine.packaging:render] 31 meshes", 11.4);
    // only the console has it (a printf past the engine's log): passed on once its wait is over
    merge.consoleLine("native printf", false, 12.0);
    merge.flush(12.5);
    EXPECT_EQ(std::count(got.begin(), got.end(), "I native printf"), 0) << "held while the unified log may still bring it";
    merge.flush(30.0);
    merge.flush(30.0, true);
    const std::vector<std::string> want = {
        "I [com.jasperengine.packaging:engine] GPU quirks: none",
        "W [com.jasperengine.packaging:engine] Shader 'tinted' has the legacy shadow interface",
        "I [com.jasperengine.packaging:lua] [UL] frame 100 (1.66 s): 63 fps",
        "D [com.jasperengine.packaging:lua] tick",
        "I [com.jasperengine.packaging:lua] tick",
        "I [com.apple.UIKit:Application] Deactivation reason added: 10",
        "I [com.jasperengine.packaging:lua] [UL] frame 200 (3.33 s): 59 fps",
        "I [com.jasperengine.packaging:render] 31 meshes",
        "I native printf",
    };
    EXPECT_EQ(got, want);
}

}  // namespace DeviceLogPumpTest_cpp
