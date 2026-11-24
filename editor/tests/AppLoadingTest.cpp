// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// App::reportLoadingProgress: the loading UI a long project load pumps between its steps.

#include "App.h"
#include "Backend.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(AppLoading, ASlowProgressFrameLeavesTheLoadWorkTime) {
    App& app = Backend::getApp();
    // the test binary's App never finishes a startup load
    ASSERT_TRUE(app.isProjectLoading());
    using Clock = std::chrono::steady_clock;
    // a frame slower than the 33 ms between progress frames (a software-rendered editor)
    std::vector<Clock::time_point> starts, ends;
    app.setLoadingPump([&](bool) {
        starts.push_back(Clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ends.push_back(Clock::now());
    });
    struct PumpReset {
        App& app;
        ~PumpReset() { app.setLoadingPump(nullptr); }
    } reset{app};
    // a load that reports progress as often as it can
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(30);
    while (starts.size() < 4 && Clock::now() < deadline) app.reportLoadingProgress();
    ASSERT_EQ(starts.size(), 4u) << "the load reports progress until the hang guard";
    // the interval runs from the end of a frame: one taking longer than it never pumps back to back
    for (size_t i = 1; i < starts.size(); i++) {
        EXPECT_GE(starts[i] - ends[i - 1], std::chrono::milliseconds(33)) << "frame " << i << " followed its predecessor without load work between";
    }
}
