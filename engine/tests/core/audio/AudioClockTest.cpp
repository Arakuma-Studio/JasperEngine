// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "audio/AudioClock.h"

#include <cmath>
#include <cstdint>

using namespace jasperengine;

TEST(AudioClockTest, SixtyFpsAt48kMixes800AFrame) {
    AudioClock clock;
    clock.reset(48000);
    for (int frame = 0; frame < 3600; frame++) {
        ASSERT_EQ(clock.advance(1.0 / 60.0), 800u) << "frame " << frame;
    }
    EXPECT_EQ(clock.cursor(), 2880000u);
    EXPECT_DOUBLE_EQ(clock.seconds(), 60.0);
}

TEST(AudioClockTest, NoDriftAt144FpsAnd44k) {
    AudioClock clock;
    clock.reset(44100);
    uint64_t total = 0;
    for (int frame = 0; frame < 144 * 60; frame++) total += clock.advance(1.0 / 144.0);
    EXPECT_EQ(total, (uint64_t)std::llround(60.0 * 44100.0));
    EXPECT_EQ(clock.cursor(), total);
}

TEST(AudioClockTest, ZeroAndNegativeTimeMixNothing) {
    AudioClock clock;
    clock.reset(48000);
    EXPECT_EQ(clock.advance(0.0), 0u);
    EXPECT_EQ(clock.advance(-1.0), 0u);
    EXPECT_EQ(clock.advance(std::nan("")), 0u);
    EXPECT_EQ(clock.cursor(), 0u);
    EXPECT_EQ(clock.advance(0.5), 24000u);
    EXPECT_EQ(clock.advance(-0.5), 0u);
    EXPECT_EQ(clock.cursor(), 24000u);
}

TEST(AudioClockTest, ResetStartsOverAtTheNewRate) {
    AudioClock clock;
    clock.reset(48000);
    clock.advance(1.0);
    clock.reset(44100);
    EXPECT_EQ(clock.cursor(), 0u);
    EXPECT_EQ(clock.seconds(), 0.0);
    EXPECT_EQ(clock.advance(1.0), 44100u);
}
