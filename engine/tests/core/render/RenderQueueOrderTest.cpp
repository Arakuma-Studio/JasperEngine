// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The opaque queue's order (draw-perf spec 6.2): sort priority, then the draws that do not fade,
// then the distance band (one per doubling of the distance, so early depth keeps most of its use),
// then the render state (equal states run together and apply once: DrawPerfGpu), then the nearest,
// then the submission order: a total order, so the sort is deterministic.

#include "subsystem/RenderSystem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

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

namespace {

    RenderSystem::OpaqueRenderData entry(int priority, float squaredDistance, uint64_t state, uint32_t order, bool fading = false){
        RenderSystem::OpaqueRenderData data = {};
        data.sortPriority = priority;
        data.distanceToCamera = squaredDistance;
        data.order = order;
        data.fading = fading;
        data.band = RenderSystem::distanceBand(squaredDistance);
        data.stateKey = state;
        return data;
    }

    std::vector<uint32_t> sortedOrder(std::vector<RenderSystem::OpaqueRenderData> queue){
        std::sort(queue.begin(), queue.end(), RenderSystem::OpaqueRenderComparison());
        std::vector<uint32_t> order;
        for (const RenderSystem::OpaqueRenderData& data : queue)
            order.push_back(data.order);
        return order;
    }

}

TEST(RenderQueueOrder, PriorityThenBandThenStateThenDistance){
    const std::vector<uint32_t> order = sortedOrder({
        entry(0, 300.0f, 7, 0),         // band 4, state 7
        entry(0, 260.0f, 3, 1),         // band 4, state 3: before 7 in the band
        entry(0, 280.0f, 7, 2),         // band 4, state 7, nearer than 0
        entry(0, 20.0f, 9, 3),          // band 2: first of priority 0
        entry(-1, 900.0f, 9, 4),        // priority -1: first of all
        entry(0, 20.0f, 9, 5, true),    // fading: after the others of its priority
    });
    EXPECT_EQ(order, (std::vector<uint32_t>{4, 3, 1, 2, 0, 5}));
}

TEST(RenderQueueOrder, EqualKeysKeepSubmissionOrder){
    std::vector<RenderSystem::OpaqueRenderData> queue;
    for (uint32_t i = 0; i < 20; i++)
        queue.push_back(entry(0, 100.0f, 5, 19 - i));
    const std::vector<uint32_t> order = sortedOrder(queue);
    ASSERT_EQ(order.size(), 20u);
    for (uint32_t i = 0; i < 20; i++)
        EXPECT_EQ(order[i], i);
}
