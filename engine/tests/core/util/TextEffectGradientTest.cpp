// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "util/TextEffects.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

using namespace jasperengine;

TEST(TextEffectGradient, EmptyAndSingleStopHaveConstantColors){
    EXPECT_EQ(TextEffects::evaluateGradient({}, 0.5f), Vector4(1, 1, 1, 1));
    const std::vector<TextEffectGradientStop> gradient = {{Vector4(0.2f, 0.3f, 0.4f, 0.5f), 0.7f}};
    for (float t : {-1.0f, 0.5f, 2.0f}){
        EXPECT_EQ(TextEffects::evaluateGradient(gradient, t), gradient.front().color);
    }
}

TEST(TextEffectGradient, DuplicatePositionsKeepStableStopOrder){
    const std::vector<TextEffectGradientStop> gradient = {
        {Vector4(1, 0, 0, 1), 0.0f}, {Vector4(0, 1, 0, 1), 0.5f},
        {Vector4(0, 0, 1, 1), 0.5f}, {Vector4(1, 1, 1, 1), 1.0f}};
    EXPECT_EQ(TextEffects::evaluateGradient(gradient, 0.5f), gradient[1].color);
    EXPECT_EQ(TextEffects::evaluateGradient(gradient, 0.75f), Vector4(0.5f, 0.5f, 1, 1));
}

TEST(TextEffectGradient, UnsortedStopsSampleWithoutChangingTheirOrder){
    const std::vector<TextEffectGradientStop> gradient = {
        {Vector4(1, 1, 1, 1), 1.0f}, {Vector4(0, 0, 1, 1), 0.5f},
        {Vector4(1, 0, 0, 1), 0.0f}, {Vector4(0, 1, 0, 1), 0.5f}};
    const auto original = gradient;
    EXPECT_EQ(TextEffects::evaluateGradient(gradient, 0.5f), gradient[1].color);
    EXPECT_EQ(TextEffects::evaluateGradient(gradient, 0.75f), Vector4(0.5f, 1, 0.5f, 1));
    EXPECT_EQ(gradient, original);
}

TEST(TextEffectGradient, SortedSamplingAllocatesNothing){
    const std::vector<TextEffectGradientStop> gradient = {
        {Vector4(1, 0, 0, 1), 0.0f}, {Vector4(0, 1, 0, 1), 0.5f},
        {Vector4(0, 0, 1, 1), 0.5f}, {Vector4(1, 1, 1, 1), 1.0f}};
    Vector4 last;
    testsupport::ThreadAllocationCount allocations;
    for (int i = 0; i < 64; i++){
        last = TextEffects::evaluateGradient(gradient, static_cast<float>(i) / 63.0f);
    }
    const uint64_t count = allocations.count();
    if (testsupport::countsAllocations()){
        EXPECT_EQ(count, 0u);
    }
    EXPECT_EQ(last, gradient.back().color);
}
