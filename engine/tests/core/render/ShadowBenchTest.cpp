// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The CPU budgets of the shadows (spec S10 R26): the cascade fit and the cache keys of a
// camera's slots within 30 us, the caster registry's update of 10,000 casters with 1 % moving
// within 0.2 ms, and a frame with nothing changed reporting no change at all (so its slots
// draw nothing). The times are printed as [bench] lines; JASPER_TEST_PERF_SLACK scales them.

#include "render/shadow/CascadeFit.h"
#include "render/shadow/ShadowCache.h"
#include "render/shadow/ShadowCasterRegistry.h"
#include "support/AllocationCounter.h"

#include "math/Matrix4.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace jasperengine;

namespace {

    double medianOf(std::vector<double> values){
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    }

    ShadowBounds unitBox(float x, float z){
        ShadowBounds b;
        b.min = Vector3(x - 0.5f, 0.0f, z - 0.5f);
        b.max = Vector3(x + 0.5f, 1.0f, z + 0.5f);
        b.valid = true;
        return b;
    }

}

TEST(ShadowBench, CascadeFitAndKeysPerCamera){
    // a sun with 4 cascades and 5 more slots (spot lights): the work of one camera per update
    CascadeFitInput input;
    input.tanHalfY = 0.5773503f;
    input.aspect = 16.0f / 9.0f;
    input.nearClip = 0.1f;
    input.shadowDistance = 100.0f;
    input.lightDir = Vector3(-0.45f, -1.0f, -0.35f).normalized();
    input.cascades = 4;
    input.resolution = 2048;
    input.casterBounds = unitBox(0.0f, 0.0f);
    input.casterBounds.min = Vector3(-60.0f, 0.0f, -60.0f);
    input.casterBounds.max = Vector3(60.0f, 12.0f, 60.0f);

    std::array<ShadowSlotState, 9> states;
    const std::vector<ShadowCasterChange> none;
    CascadeFitResult previous;
    bool hasPrevious = false;
    uint64_t frame = 0;
    std::vector<double> samples;
    constexpr int frames = 2000;
    for (int f = 0; f < frames; f++){
        // a camera walking at 5 m/s, 60 frames a second, turning slowly
        const float x = 5.0f * (float)f / 60.0f;
        input.cameraView = Matrix4::lookAtMatrix(Vector3(x, 2.0f, 0.0f), Vector3(x + 0.2f * (float)f / frames, 1.5f, -10.0f), Vector3(0, 1, 0));
        const auto start = std::chrono::steady_clock::now();
        const CascadeFitResult fit = CascadeFit::fit(input, hasPrevious ? &previous : nullptr);
        for (size_t s = 0; s < states.size(); s++){
            ShadowSlotInput slot;
            slot.key.viewProjection = fit.viewProj[s % fit.cascades];
            slot.key.rect = ShadowRect{(int)(s % 3) * 2048, (int)(s / 3) * 2048, 2048, 2048};
            slot.key.casterMask = 1u;
            slot.key.lightRevision = 1;
            slot.key.atlasGeneration = 1;
            ShadowCache::evaluate(states[s], slot, none, ++frame);
        }
        samples.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
        previous = fit;
        hasPrevious = true;
    }
    const double median = medianOf(samples);
    std::printf("[bench] cascade fit (4 cascades) + 9 slot keys per camera: %.2f us median (budget 30)\n", median);
    if (testsupport::perfSlack() > 0.0)
        EXPECT_LE(median, 30.0 * testsupport::perfSlack());
}

TEST(ShadowBench, CasterRegistryTenThousandOnePercentMoving){
    constexpr uint32_t casters = 10000;
    std::vector<ShadowCasterState> states(casters);
    for (uint32_t i = 0; i < casters; i++){
        states[i].bounds = unitBox((float)(i % 100) * 2.0f, (float)(i / 100) * 2.0f);
        states[i].revision = 1;
        states[i].layerBit = 1u;
        states[i].isStatic = (i % 10) != 0;
    }

    ShadowCasterRegistry registry;
    // warm: every caster seen once (the first update reports them all as added)
    registry.beginUpdate();
    for (uint32_t i = 0; i < casters; i++)
        registry.update(i, states[i]);
    registry.endUpdate();
    ASSERT_EQ(registry.size(), casters);

    // nothing changed: no change reported, nothing allocated
    {
        testsupport::ThreadAllocationCount allocations;
        registry.beginUpdate();
        for (uint32_t i = 0; i < casters; i++)
            registry.update(i, states[i]);
        registry.endUpdate();
        EXPECT_TRUE(registry.getChanges().empty());
        if (testsupport::countsAllocations())
            EXPECT_EQ(allocations.count(), 0u);
    }

    std::vector<double> samples;
    for (int round = 0; round < 50; round++){
        // 1 % of the casters move this update
        for (uint32_t i = (uint32_t)round; i < casters; i += 100){
            states[i].bounds.min.y += 0.01f;
            states[i].bounds.max.y += 0.01f;
        }
        const auto start = std::chrono::steady_clock::now();
        registry.beginUpdate();
        for (uint32_t i = 0; i < casters; i++)
            registry.update(i, states[i]);
        registry.endUpdate();
        samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        EXPECT_EQ(registry.getChanges().size(), casters / 100);
    }
    const double median = medianOf(samples);
    std::printf("[bench] caster registry, 10,000 casters, 1 %% moving: %.3f ms median (budget 0.2)\n", median);
    if (testsupport::perfSlack() > 0.0)
        EXPECT_LE(median, 0.2 * testsupport::perfSlack());
}
