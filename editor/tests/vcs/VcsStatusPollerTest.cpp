// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// VcsStatusPoller (docs/specs/S29-version-control.md R5, AC4), the cases of OpenEngine's
// VCSStatusPollerTests (Tests/Editor/VCSStatusPollerTests.cpp, MIT, Copyright (c) 2026 Gamecrafters
// Guild Limited and contributors; NOTICE.md) ported. The interval is an hour, so every poll after
// the first one comes from a wake.

#include "vcs/VcsStatusPoller.h"

#include "LogCapture.h"
#include "Out.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace jasperengine::editor;
using namespace jasperengine::editor::vcs;

namespace {

    constexpr int kNeverSeconds = 3600;
    constexpr std::chrono::seconds kPollTimeout{5};
    constexpr std::chrono::milliseconds kSettleTime{1500};

    bool waitForPolls(const std::atomic<int>& polls, int target) {
        const auto deadline = std::chrono::steady_clock::now() + kPollTimeout;
        while (polls.load() < target) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

}

TEST(VcsStatusPoller, PollsOnceAtStart) {
    std::atomic<int> polls{0};
    VcsStatusPoller poller;
    poller.setIntervalSeconds(kNeverSeconds);
    poller.start("je-vcs-test", [&polls] { ++polls; });
    EXPECT_TRUE(waitForPolls(polls, 1));
    EXPECT_FALSE(poller.isStopping());
    poller.stop();
}

TEST(VcsStatusPoller, WakePollsBeforeTheInterval) {
    std::atomic<int> polls{0};
    VcsStatusPoller poller;
    poller.setIntervalSeconds(kNeverSeconds);
    poller.start("je-vcs-test", [&polls] { ++polls; });
    ASSERT_TRUE(waitForPolls(polls, 1));
    poller.wake();
    EXPECT_TRUE(waitForPolls(polls, 2));
    poller.stop();
}

TEST(VcsStatusPoller, BurstOfWakesCollapsesIntoAtMostTwoPolls) {
    std::atomic<int> polls{0};
    VcsStatusPoller poller;
    poller.setIntervalSeconds(kNeverSeconds);
    poller.start("je-vcs-test", [&polls] { ++polls; });
    ASSERT_TRUE(waitForPolls(polls, 1));
    // AC4: 20 wakes within 100 ms (each at its own time: a host whose sleeps overrun would
    // otherwise spread them past the cooldown)
    const auto burst = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) {
        poller.wake();
        std::this_thread::sleep_until(burst + std::chrono::milliseconds(5 * (i + 1)));
    }
    std::this_thread::sleep_for(kSettleTime);
    // the immediate poll, then at most one more after the cooldown
    const int afterStart = polls.load() - 1;
    EXPECT_GE(afterStart, 1);
    EXPECT_LE(afterStart, 2);
    poller.stop();
}

TEST(VcsStatusPoller, StopJoinsAndIgnoresLaterWakes) {
    std::atomic<int> polls{0};
    VcsStatusPoller poller;
    poller.setIntervalSeconds(kNeverSeconds);
    poller.start("je-vcs-test", [&polls] { ++polls; });
    ASSERT_TRUE(waitForPolls(polls, 1));
    poller.stop();
    EXPECT_TRUE(poller.isStopping());
    EXPECT_FALSE(poller.isRunning());
    const int pollsAtStop = polls.load();
    poller.wake();
    std::this_thread::sleep_for(kSettleTime);
    EXPECT_EQ(polls.load(), pollsAtStop);
}

TEST(VcsStatusPoller, StopDuringAPollJoins) {
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    VcsStatusPoller poller;
    poller.setIntervalSeconds(kNeverSeconds);
    poller.start("je-vcs-test", [&] {
        ++started;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ++finished;
    });
    ASSERT_TRUE(waitForPolls(started, 1));
    // the poll in progress finishes before stop() returns, and none starts after it
    poller.stop();
    EXPECT_EQ(finished.load(), started.load());
    EXPECT_EQ(started.load(), 1);
    EXPECT_FALSE(poller.isRunning());
}

TEST(VcsStatusPoller, ThrowingPollIsLoggedAndPollingContinues) {
    jasperengine::test::LogCapture log;
    std::atomic<int> polls{0};
    VcsStatusPoller poller;
    poller.setIntervalSeconds(kNeverSeconds);
    poller.start("je-vcs-test", [&polls] {
        if (++polls == 1) throw std::runtime_error("status command failed");
    });
    ASSERT_TRUE(waitForPolls(polls, 1));
    poller.wake();
    EXPECT_TRUE(waitForPolls(polls, 2));
    poller.stop();
    EXPECT_EQ(log.count(D_LOG_WARN, "je-vcs-test: status poll failed: status command failed"), 1u);
}

TEST(VcsStatusPoller, NonPositiveIntervalRestoresTheDefault) {
    VcsStatusPoller poller;
    poller.setIntervalSeconds(0);
    EXPECT_EQ(poller.getIntervalSeconds(), VcsStatusPoller::kDefaultIntervalSeconds);
    poller.setIntervalSeconds(30);
    EXPECT_EQ(poller.getIntervalSeconds(), 30);
}
