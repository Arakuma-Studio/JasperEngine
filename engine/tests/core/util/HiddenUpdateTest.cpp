// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The update rate and clock of a hidden window (platform-lifecycle spec D2): the game updates
// without drawing at its own rate, at most 60 Hz, on a deadline schedule (FrameLimiter).

#include "util/FrameLimiter.h"
#include "util/PlatformRules.h"

#include <gtest/gtest.h>

using namespace jasperengine;

TEST(PlatformRules, HiddenUpdateFps){
    EXPECT_DOUBLE_EQ(PlatformRules::hiddenUpdateFps(-1, 0, 60.0), 60.0);
    EXPECT_DOUBLE_EQ(PlatformRules::hiddenUpdateFps(30, 0, 60.0), 30.0);
    EXPECT_DOUBLE_EQ(PlatformRules::hiddenUpdateFps(144, 0, 144.0), 60.0);
    // vSyncCount decides over targetFrameRate (Unity)
    EXPECT_DOUBLE_EQ(PlatformRules::hiddenUpdateFps(-1, 2, 60.0), 30.0);
    EXPECT_DOUBLE_EQ(PlatformRules::hiddenUpdateFps(120, 2, 60.0), 30.0);
    EXPECT_DOUBLE_EQ(PlatformRules::hiddenUpdateFps(-1, 1, 144.0), 60.0);
    // an unknown refresh rate counts as 60
    EXPECT_DOUBLE_EQ(PlatformRules::hiddenUpdateFps(-1, 2, 0.0), 30.0);
    EXPECT_DOUBLE_EQ(PlatformRules::hiddenUpdateFps(1, 0, 60.0), 1.0);
}

TEST(FrameLimiter, HiddenClockIsDueAtOnceAfterReset){
    FrameLimiter clock;
    EXPECT_DOUBLE_EQ(clock.secondsUntilDueAt(100.0), 0.0);
    EXPECT_TRUE(clock.isDueAt(100.0, 60.0, 0.0));
    EXPECT_FALSE(clock.isDueAt(100.001, 60.0, 0.0));
    clock.reset();
    EXPECT_TRUE(clock.isDueAt(100.002, 60.0, 0.0));
}

TEST(FrameLimiter, HiddenClockKeepsItsRate){
    FrameLimiter clock;
    int updates = 0;
    // one simulated second polled every millisecond
    for (int ms = 0; ms < 1000; ms++){
        if (clock.isDueAt(10.0 + ms / 1000.0, 60.0, 0.0)) updates++;
    }
    EXPECT_GE(updates, 59);
    EXPECT_LE(updates, 61);
}

TEST(FrameLimiter, HiddenClockMakesUpALateUpdate){
    FrameLimiter clock;
    const double period = 1.0 / 60.0;
    ASSERT_TRUE(clock.isDueAt(0.0, 60.0, 0.0));
    // woken half a period late: the late update runs, and the next one keeps the schedule (due at
    // 2 periods, not 2.5)
    EXPECT_TRUE(clock.isDueAt(1.5 * period, 60.0, 0.0));
    EXPECT_FALSE(clock.isDueAt(1.9 * period, 60.0, 0.0));
    EXPECT_TRUE(clock.isDueAt(2.0 * period, 60.0, 0.0));
}

TEST(FrameLimiter, HiddenClockRestartsAfterAStall){
    FrameLimiter clock;
    const double period = 1.0 / 60.0;
    ASSERT_TRUE(clock.isDueAt(0.0, 60.0, 0.0));
    // a stall of a second gives one update, not a burst of sixty
    EXPECT_TRUE(clock.isDueAt(1.0, 60.0, 0.0));
    EXPECT_FALSE(clock.isDueAt(1.0, 60.0, 0.0));
    EXPECT_NEAR(clock.secondsUntilDueAt(1.0), period, 1e-9);
}

TEST(FrameLimiter, SecondsUntilDueCountsDown){
    FrameLimiter clock;
    const double period = 1.0 / 30.0;
    ASSERT_TRUE(clock.isDueAt(5.0, 30.0, 0.0));
    EXPECT_NEAR(clock.secondsUntilDueAt(5.0), period, 1e-9);
    EXPECT_NEAR(clock.secondsUntilDueAt(5.0 + period / 2.0), period / 2.0, 1e-9);
    EXPECT_DOUBLE_EQ(clock.secondsUntilDueAt(5.0 + period), 0.0);
    EXPECT_DOUBLE_EQ(clock.secondsUntilDueAt(6.0), 0.0);
}
