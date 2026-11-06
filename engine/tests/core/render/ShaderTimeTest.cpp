// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The shader clock (docs/specs/S22-gameplay.md R29, AC13): latched once a frame at the top of
// Engine::systemDraw (latchShaderTime), so every fixed step of a frame, its draw and the CPU's
// waves agree; with a fixed frame delta it advances exactly that much a frame. Pinned
// (Engine::setPinnedShaderTime, the golden image tests) it stays put.

#include "Engine.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

using namespace jasperengine;

namespace {

    class ShaderTimeTest : public ::testing::Test {
    protected:
        void TearDown() override{
            Engine::setPinnedShaderTime(-1.0);
            Engine::setFixedDeltatime(0.0);
            Engine::latchShaderTime();
        }
    };

}

TEST_F(ShaderTimeTest, LatchedOncePerFrame){
    Engine::setFixedDeltatime(0.0);
    Engine::latchShaderTime();
    const float latched = Engine::getShaderTime();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    // the wall clock moved on, the frame's shader time did not
    EXPECT_EQ(Engine::getShaderTime(), latched);
    EXPECT_GT((float)Engine::getSystemTime(), latched + 0.02f);

    Engine::latchShaderTime();
    EXPECT_GT(Engine::getShaderTime(), latched + 0.02f);
    EXPECT_NEAR(Engine::getShaderTime(), (float)Engine::getSystemTime(), 0.01f);
}

TEST_F(ShaderTimeTest, FixedDeltaAdvancesExactly){
    Engine::setFixedDeltatime(0.0);
    Engine::latchShaderTime();
    // fixed mode starts from where the clock is
    Engine::setFixedDeltatime(1.0 / 60.0);
    Engine::latchShaderTime();
    const double start = Engine::getShaderTimeSeconds();
    for (int frame = 0; frame < 60; frame++){
        Engine::latchShaderTime();
    }
    EXPECT_NEAR(Engine::getShaderTimeSeconds() - start, 1.0, 1e-9);

    // whole frames of the delta from where fixed mode began: no rounding piles up (reproducible captures)
    std::vector<double> first;
    Engine::setFixedDeltatime(0.0);
    Engine::latchShaderTime();
    Engine::setFixedDeltatime(0.5);
    Engine::latchShaderTime();
    const double base = Engine::getShaderTimeSeconds();
    for (int frame = 0; frame < 4; frame++){
        Engine::latchShaderTime();
        first.push_back(Engine::getShaderTimeSeconds() - base);
    }
    ASSERT_EQ(first.size(), 4u);
    for (size_t i = 0; i < first.size(); i++){
        EXPECT_NEAR(first[i], 0.5 * (double)(i + 1), 1e-12);
    }

    // back to the wall clock
    Engine::setFixedDeltatime(0.0);
    Engine::latchShaderTime();
    EXPECT_NEAR(Engine::getShaderTime(), (float)Engine::getSystemTime(), 0.01f);
}

TEST_F(ShaderTimeTest, PinnedHoldsUntilUnpinned){
    Engine::setFixedDeltatime(1.0 / 60.0);
    Engine::setPinnedShaderTime(12.5);
    EXPECT_EQ(Engine::getShaderTimeSeconds(), 12.5);
    for (int frame = 0; frame < 10; frame++){
        Engine::latchShaderTime();
        EXPECT_EQ(Engine::getShaderTimeSeconds(), 12.5);
    }

    // unpinned, a fixed delta goes on from the pinned time
    Engine::setPinnedShaderTime(-1.0);
    Engine::latchShaderTime();
    EXPECT_EQ(Engine::getShaderTimeSeconds(), 12.5);
    Engine::latchShaderTime();
    EXPECT_NEAR(Engine::getShaderTimeSeconds(), 12.5 + 1.0 / 60.0, 1e-12);

    // and the wall clock without one
    Engine::setFixedDeltatime(0.0);
    Engine::latchShaderTime();
    EXPECT_NEAR(Engine::getShaderTime(), (float)Engine::getSystemTime(), 0.01f);
}
