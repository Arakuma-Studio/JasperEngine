// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The opaque queue's 64-bit state sort key (spec S43 R2, AC2): sort priority first, then the
// draws fading between detail levels after the others, then pipeline, material and mesh, then
// front to back within them. The transparent queue keeps its back-to-front order: it is never
// sorted by these keys (InstancingGoldenGpu draws overlapping transparent planes with GPU
// Instancing on and off).

#include "render/VisibleList.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace jasperengine;

namespace {
    uint64_t key(int priority, bool fading, uint32_t pipeline, uint32_t material, uint32_t mesh, float distance){
        return VisibleList::opaqueKey(priority, fading, pipeline, material, mesh, distance, 1000.0f);
    }
}

TEST(DrawSort, PriorityComesFirst){
    // a lower priority draws first whatever its state or distance (stencil writers before readers)
    EXPECT_LT(key(-1, true, 0x7FFF, 0xFFFF, 0xFFF, 999.0f), key(0, false, 0, 0, 0, 0.0f));
    EXPECT_LT(key(0, true, 0x7FFF, 0xFFFF, 0xFFF, 999.0f), key(1, false, 0, 0, 0, 0.0f));
    // clamped to -128..127, order kept at the ends
    EXPECT_LT(key(-1000, false, 0, 0, 0, 0.0f), key(-127, false, 0, 0, 0, 0.0f));
    EXPECT_EQ(key(-1000, false, 0, 0, 0, 0.0f), key(-128, false, 0, 0, 0, 0.0f));
    EXPECT_EQ(key(1000, false, 0, 0, 0, 0.0f), key(127, false, 0, 0, 0, 0.0f));
}

TEST(DrawSort, FadingDrawsAfterTheOthers){
    // (S42: a dither pipeline of their own, without early depth)
    EXPECT_LT(key(0, false, 0x7FFF, 0xFFFF, 0xFFF, 999.0f), key(0, true, 0, 0, 0, 0.0f));
}

TEST(DrawSort, StateBeforeDistance){
    // pipeline, then material, then mesh: each wins over everything after it
    EXPECT_LT(key(0, false, 1, 0xFFFF, 0xFFF, 999.0f), key(0, false, 2, 0, 0, 0.0f));
    EXPECT_LT(key(0, false, 1, 1, 0xFFF, 999.0f), key(0, false, 1, 2, 0, 0.0f));
    EXPECT_LT(key(0, false, 1, 1, 1, 999.0f), key(0, false, 1, 1, 2, 0.0f));
}

TEST(DrawSort, FrontToBackWithinState){
    float previous = -1.0f;
    uint64_t previousKey = 0;
    for (float distance : {0.0f, 0.5f, 1.0f, 2.0f, 10.0f, 100.0f, 999.0f, 1000.0f}){
        const uint64_t k = key(0, false, 3, 4, 5, distance);
        if (previous >= 0.0f){
            EXPECT_LE(previousKey, k) << distance;
        }
        previous = distance;
        previousKey = k;
    }
    // far apart distances keep apart; past the far clip they share the last bucket
    EXPECT_LT(key(0, false, 3, 4, 5, 1.0f), key(0, false, 3, 4, 5, 50.0f));
    EXPECT_EQ(key(0, false, 3, 4, 5, 1000.0f), key(0, false, 3, 4, 5, 5000.0f));
}

TEST(DrawSort, SortsDrawsOfOneStateTogether){
    // draws of two materials interleaved by distance come out grouped, each group front to back
    struct Draw { uint32_t material; float distance; uint64_t key; };
    std::vector<Draw> draws;
    for (int i = 0; i < 8; i++){
        Draw draw{(uint32_t)(i % 2), (float)(8 - i)};
        draw.key = key(0, false, 7, draw.material, 9, draw.distance);
        draws.push_back(draw);
    }
    std::sort(draws.begin(), draws.end(), [](const Draw& a, const Draw& b){ return a.key < b.key; });
    for (size_t i = 0; i < draws.size(); i++){
        EXPECT_EQ(draws[i].material, i < 4 ? 0u : 1u);
        if (i % 4 != 0){
            EXPECT_LE(draws[i - 1].distance, draws[i].distance);
        }
    }
}

TEST(DrawSort, MixKeyDependsOnEveryValue){
    const uint32_t a = VisibleList::mixKey(VisibleList::mixKey(0, 1), 2);
    const uint32_t b = VisibleList::mixKey(VisibleList::mixKey(0, 2), 1);
    const uint32_t c = VisibleList::mixKey(VisibleList::mixKey(0, 1), 3);
    EXPECT_NE(a, b);
    EXPECT_NE(a, c);
    EXPECT_EQ(a, VisibleList::mixKey(VisibleList::mixKey(0, 1), 2));
}
