// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The opaque queue's distance bands (draw-perf spec 6.2): it sorts by sort priority, the draws
// that do not fade, the band (one per doubling of the distance, so early depth keeps most of its
// use), the render state (equal states run together and apply once: DrawPerfGpu), the nearest,
// then the submission order.

#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace jasperengine;

TEST(RenderQueueOrder, DistanceBandsDouble){
    EXPECT_EQ(RenderSystem::distanceBand(0.0f), 0);
    EXPECT_EQ(RenderSystem::distanceBand(1.0f), 0);
    EXPECT_EQ(RenderSystem::distanceBand(3.9f), 0);     // under 2 m
    EXPECT_EQ(RenderSystem::distanceBand(4.0f), 1);     // 2 m
    EXPECT_EQ(RenderSystem::distanceBand(15.9f), 1);
    EXPECT_EQ(RenderSystem::distanceBand(16.0f), 2);    // 4 m
    EXPECT_EQ(RenderSystem::distanceBand(256.0f), 4);   // 16 m
    EXPECT_EQ(RenderSystem::distanceBand(1e30f), 49);
    EXPECT_EQ(RenderSystem::distanceBand(std::numeric_limits<float>::infinity()), 63);
    EXPECT_EQ(RenderSystem::distanceBand(std::nanf("")), 0);
    EXPECT_EQ(RenderSystem::distanceBand(-1.0f), 0);
}
