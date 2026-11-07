// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The order a pass's opaque queue draws its batches in (spec S55 R2, AC2): the queue keeps its
// order (sort priority, then nearest first: draw-perf owns the sort), and the draws of one batch
// key come together at the place of the first within each priority, so a batch is a run of the
// queue. A priority run never mixes with another, and a draw that batches with nothing stays
// where it is. The transparent queue is never grouped (InstancingGoldenGpu draws overlapping
// transparent planes with GPU Instancing on and off: the same image).

#include "render/VisibleList.h"

#include <gtest/gtest.h>

#include <vector>

using namespace jasperengine;

namespace {
    std::vector<uint32_t> group(const std::vector<int>& priorities, const std::vector<uint64_t>& keys){
        std::vector<uint32_t> order;
        VisibleList::groupBatches(priorities.data(), keys.data(), keys.size(), order);
        return order;
    }
}

TEST(DrawSort, PriorityRunsKeepTheirPlace){
    // equal keys in two priorities stay in their own (stencil writers before their readers)
    const std::vector<uint32_t> order = group({-1, -1, 0, 0, 0, 1}, {5, 6, 5, 6, 5, 5});
    EXPECT_EQ(order, (std::vector<uint32_t>{0, 1, 2, 4, 3, 5}));
}

TEST(DrawSort, UnbatchedDrawsStayInPlace){
    // key 0 batches with nothing: two of them never group
    const std::vector<uint32_t> order = group({0, 0, 0, 0}, {0, 7, 0, 7});
    EXPECT_EQ(order, (std::vector<uint32_t>{0, 1, 3, 2}));
}

TEST(DrawSort, KeepsTheQueueOrderWithinABatch){
    // a batch's draws keep their front-to-back order, and the queue is a permutation
    std::vector<int> priorities(100, 0);
    std::vector<uint64_t> keys(100);
    for (size_t i = 0; i < keys.size(); i++)
        keys[i] = 1 + i % 4;
    const std::vector<uint32_t> order = group(priorities, keys);
    ASSERT_EQ(order.size(), keys.size());
    std::vector<bool> seen(order.size(), false);
    for (size_t i = 0; i < order.size(); i++){
        ASSERT_LT(order[i], order.size());
        EXPECT_FALSE(seen[order[i]]);
        seen[order[i]] = true;
        if (i > 0 && keys[order[i]] == keys[order[i - 1]]){
            EXPECT_LT(order[i - 1], order[i]);
        }
    }
    // four runs, in the order of their first draws
    for (size_t i = 0; i < order.size(); i++)
        EXPECT_EQ(keys[order[i]], 1 + i / 25);
}

TEST(DrawSort, MixKeyDependsOnEveryValue){
    const uint32_t a = VisibleList::mixKey(VisibleList::mixKey(0, 1), 2);
    const uint32_t b = VisibleList::mixKey(VisibleList::mixKey(0, 2), 1);
    const uint32_t c = VisibleList::mixKey(VisibleList::mixKey(0, 1), 3);
    EXPECT_NE(a, b);
    EXPECT_NE(a, c);
    EXPECT_EQ(a, VisibleList::mixKey(VisibleList::mixKey(0, 1), 2));
}
