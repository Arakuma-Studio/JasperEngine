// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "titleart/render/TitleRenderJob.h"

#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    // a job of `count` GPU passes over `width` x `height`, each recording the scissors it ran with
    TitleStepList passes(int count, int width, int height, std::vector<TitleRectI>* ran, float weight = 1.0f) {
        TitleStepList list;
        for (int i = 0; i < count; i++) {
            TitleRenderStep step;
            step.kind = StepKind::Gpu;
            step.label = "pass " + std::to_string(i);
            step.layerId = i % 2 ? "0000000b" : "0000000a";
            step.scissor = {0, 0, width, height};
            step.costWeight = weight;
            step.splittable = true;
            step.shader = ShaderKey(7, (uint64_t)(i % 3), 0);
            step.run = [ran](const TitleRectI& scissor, std::string&) {
                if (ran) ran->push_back(scissor);
                return true;
            };
            list.add(std::move(step));
        }
        return list;
    }

    class TitleRenderJobTest : public ::testing::Test {};

}

TEST_F(TitleRenderJobTest, StepsRunWholeWithinTheBudget) {
    std::vector<TitleRectI> ran;
    // 1000 x 1000 px at 1 ns/px: 1000 us per pass
    TitleRenderJob job(passes(6, 1000, 1000, &ran));
    EXPECT_EQ(job.passesTotal(), 6u);
    EXPECT_EQ(job.nextStepCostUs(), 1000u);
    StepReport report;
    EXPECT_FALSE(job.step(StepBudget{2500, 0}, &report));
    EXPECT_EQ(report.steps, 2u) << "two passes fit 2.5 ms";
    EXPECT_EQ(report.estimatedUs, 2000u);
    EXPECT_FALSE(job.step(StepBudget{10, 0}, &report));
    EXPECT_EQ(report.steps, 1u) << "always at least one step";
    EXPECT_FALSE(job.step(StepBudget{0, 2}, &report));
    EXPECT_EQ(report.passes, 2u) << "maxPasses";
    EXPECT_TRUE(job.step(StepBudget{}, &report));
    EXPECT_TRUE(job.done());
    EXPECT_EQ(job.passesDone(), job.passesTotal());
    EXPECT_EQ(job.nextStepCostUs(), 0u);
    EXPECT_EQ(ran.size(), 6u);
}

TEST_F(TitleRenderJobTest, ProgressRisesToOne) {
    TitleRenderJob job(passes(5, 300, 300, nullptr));
    float last = job.progress();
    EXPECT_EQ(last, 0.0f);
    while (!job.done()) {
        job.step(StepBudget{1, 0});
        EXPECT_GE(job.progress(), last);
        last = job.progress();
    }
    EXPECT_EQ(job.progress(), 1.0f);
}

TEST_F(TitleRenderJobTest, SplitTilesCoverTheScissorOnce) {
    std::vector<TitleRectI> ran;
    TitleRenderJob job(passes(1, 1500, 700, &ran, 2.0f));
    // 1500 x 700 x 2 ns = 2100 us
    EXPECT_EQ(job.nextStepCostUs(), 2100u);
    job.splitOversizedSteps(600);
    EXPECT_GT(job.passesTotal(), 1u);
    EXPECT_LE(job.nextStepCostUs(), 600u + 1);
    job.step(StepBudget{});
    ASSERT_EQ(ran.size(), job.passesTotal());
    long area = 0;
    for (const TitleRectI& tile : ran) {
        EXPECT_GE(tile.width, TITLE_MIN_TILE);
        EXPECT_GE(tile.height, TITLE_MIN_TILE);
        area += (long)tile.width * tile.height;
        for (const TitleRectI& other : ran) {
            if (&other == &tile) continue;
            const bool apart = tile.x + tile.width <= other.x || other.x + other.width <= tile.x ||
                               tile.y + tile.height <= other.y || other.y + other.height <= tile.y;
            EXPECT_TRUE(apart) << "tiles do not overlap";
        }
    }
    EXPECT_EQ(area, 1500L * 700L) << "the tiles cover the scissor";
    // a step smaller than two tiles is never split
    std::vector<TitleRectI> small;
    TitleRenderJob tiny(passes(1, 300, 300, &small, 100.0f));
    tiny.splitOversizedSteps(1);
    EXPECT_EQ(tiny.passesTotal(), 1u);
}

TEST_F(TitleRenderJobTest, ShaderKeysAndNodeCosts) {
    TitleRenderJob job(passes(6, 100, 100, nullptr));
    const std::vector<ShaderKey> keys = job.shaderKeys();
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0], ShaderKey(7, 0, 0));
    EXPECT_EQ(keys[1], ShaderKey(7, 1, 0));
    job.step(StepBudget{});
    const std::vector<NodeCost> costs = job.nodeCosts();
    ASSERT_EQ(costs.size(), 2u);
    EXPECT_EQ(costs[0].layerId, "0000000a");
    EXPECT_EQ(costs[0].passes, 3u);
    EXPECT_EQ(costs[0].estimatedUs, 30u);
}

TEST_F(TitleRenderJobTest, AFailedStepStopsTheJob) {
    TitleStepList list = passes(2, 10, 10, nullptr);
    TitleRenderStep bad;
    bad.kind = StepKind::Cpu;
    bad.label = "outlines";
    bad.cpuUs = 50;
    bad.run = [](const TitleRectI&, std::string& error) {
        error = "asset_missing: fonts/gone.ttf";
        return false;
    };
    list.add(bad);
    list.add(passes(1, 10, 10, nullptr).steps().front());
    TitleRenderJob job(std::move(list));
    EXPECT_FALSE(job.step(StepBudget{}));
    EXPECT_TRUE(job.failed());
    EXPECT_FALSE(job.done());
    EXPECT_EQ(job.error(), "asset_missing: fonts/gone.ttf");
    EXPECT_EQ(job.passesDone(), 2u);
    EXPECT_EQ(job.nextStepCostUs(), 0u);
    EXPECT_FALSE(job.step(StepBudget{})) << "nothing runs after a failure";
}
