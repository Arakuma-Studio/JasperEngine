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

namespace {

    // `count` "flood" passes estimated at `estimateUs` each (1 ns/px) that take `actualUs` of the
    // fake clock: what a software rasterizer does to the desktop estimates (Deviation 54)
    TitleStepList slowPasses(int count, uint32_t estimateUs, uint32_t actualUs, double* clockUs) {
        TitleStepList list;
        for (int i = 0; i < count; i++) {
            TitleRenderStep step;
            step.kind = StepKind::Gpu;
            step.label = "flood";
            step.scissor = {0, 0, (int)estimateUs, 1000};
            step.run = [clockUs, actualUs](const TitleRectI&, std::string&) {
                *clockUs += actualUs;
                return true;
            };
            list.add(std::move(step));
        }
        return list;
    }

}

TEST_F(TitleRenderJobTest, MeasuredCostsHoldTheBudget) {
    // R45: steps estimated at 100 us that take 2000 us. Trusting the estimates, a 4 ms budget would
    // run all 40 of them (80 ms); measured, the call stops once the clock says the budget is spent
    double now = 0;
    TitleStepCosts costs([&now] { return now; });
    TitleRenderJob job(slowPasses(40, 100, 2000, &now));
    job.useCosts(&costs);
    EXPECT_EQ(job.nextStepCostUs(), 100u) << "nothing measured yet: the static estimate";
    StepReport report;
    const double start = now;
    EXPECT_FALSE(job.step(StepBudget{4000, 0}, &report));
    EXPECT_LE(now - start, 4000.0) << report.steps << " steps ran";
    EXPECT_EQ(report.steps, 2u);
    EXPECT_EQ(report.measuredUs, 4000u);
    EXPECT_EQ(report.longestUs, 2000u);
    EXPECT_EQ(report.longestLabel, "flood");
    EXPECT_EQ(job.nextStepCostUs(), 2000u) << "the next prediction is what the kind took";
    // a new job of the same kinds starts from what the model learned
    TitleRenderJob again(slowPasses(4, 100, 2000, &now));
    again.useCosts(&costs);
    EXPECT_EQ(again.nextStepCostUs(), 2000u);
    // a step predicted over what is left waits for the next call, unless it is the call's first
    EXPECT_FALSE(again.step(StepBudget{3000, 0}, &report));
    EXPECT_EQ(report.steps, 1u);
    EXPECT_FALSE(again.step(StepBudget{1000, 0}, &report));
    EXPECT_EQ(report.steps, 1u) << "always at least one step";
}

TEST_F(TitleRenderJobTest, CostModelLearnsPerKind) {
    TitleStepCosts costs([] { return 0.0; });
    TitleRenderStep flood;
    flood.label = "flood";
    flood.scissor = {0, 0, 100, 1000};      // 100 us estimated
    TitleRenderStep tile = flood;
    tile.label = "flood tile 2/4";
    EXPECT_EQ(TitleStepCosts::kindOf(tile), TitleStepCosts::kindOf(flood)) << "a split's tiles are its kind";
    TitleRenderStep outlines;
    outlines.kind = StepKind::Cpu;
    outlines.label = "flood";
    outlines.cpuUs = 50;
    EXPECT_NE(TitleStepCosts::kindOf(outlines), TitleStepCosts::kindOf(flood)) << "a CPU stage is another kind";
    EXPECT_EQ(costs.predictUs(flood), 100u);
    costs.observe(flood, 1000.0);
    EXPECT_EQ(costs.predictUs(flood), 1000u) << "the first measurement is what the kind knows";
    EXPECT_EQ(costs.predictUs(outlines), 500u) << "an unmeasured kind takes the machine's ratio";
    costs.observe(flood, 3000.0);
    EXPECT_EQ(costs.predictUs(flood), 2000u) << "a slower run moves it half way";
    costs.observe(flood, 200.0);
    EXPECT_EQ(costs.predictUs(flood), 1550u) << "a quicker one a quarter of the way";
    EXPECT_EQ(costs.kinds(), 1u);
    for (size_t i = 0; i < TitleStepCosts::MAX_KINDS + 10; i++) costs.observe("kind " + std::to_string(i), 10, 10.0);
    EXPECT_EQ(costs.kinds(), TitleStepCosts::MAX_KINDS) << "kinds are capped";
}

TEST_F(TitleRenderJobTest, GatedStepsAndPredictedSplits) {
    // a software GPU: a GPU step fences, and no GPU step starts until the GPU passed the fence (a
    // later call), so the main thread never waits for its drawing; CPU steps are not gated
    double now = 0;
    bool drawing = false;
    int fences = 0;
    TitleStepCosts costs([&now] { return now; });
    costs.setGpuGate([&] { return drawing; }, [&] {
        drawing = true;
        fences++;
    });
    TitleStepList list = slowPasses(2, 100, 100, &now);
    TitleRenderStep stage;
    stage.kind = StepKind::Cpu;
    stage.label = "outlines";
    stage.cpuUs = 10;
    list.add(stage);
    list.add(slowPasses(1, 100, 100, &now).steps().front());
    TitleRenderJob job(std::move(list));
    job.useCosts(&costs);
    StepReport report;
    EXPECT_FALSE(job.step(StepBudget{100000, 0}, &report));
    EXPECT_EQ(report.steps, 1u) << "the second pass waits for the first one's drawing";
    EXPECT_TRUE(job.waiting());
    EXPECT_EQ(fences, 1);
    EXPECT_FALSE(job.step(StepBudget{100000, 0}, &report));
    EXPECT_EQ(report.steps, 0u) << "still drawing";
    drawing = false;
    EXPECT_FALSE(job.step(StepBudget{100000, 0}, &report));
    EXPECT_EQ(report.steps, 2u) << "the pass, then the CPU stage while the GPU draws";
    drawing = false;
    EXPECT_TRUE(job.step(StepBudget{100000, 0}, &report));
    EXPECT_EQ(fences, 3);
    costs.setGpuGate({}, {});
    // a 2048 x 1000 px pass estimated at 2048 us, predicted at 20 x that: cut by the prediction
    std::vector<TitleRectI> ran;
    TitleRenderJob big(passes(1, 2048, 1000, &ran));
    big.useCosts(&costs);
    TitleRenderStep probe = passes(1, 2048, 1000, nullptr).steps().front();
    costs.observe(probe, 20.0 * TitleRenderJob::estimateUs(probe));
    big.splitOversizedSteps(4000);
    EXPECT_GT(big.passesTotal(), 1u) << "split where the static estimate alone would not";
    TitleRenderStep tile = probe;
    tile.label = probe.label + " tile 1/8 tile 2/2";
    EXPECT_EQ(TitleStepCosts::kindOf(tile), TitleStepCosts::kindOf(probe)) << "a tile split again keeps its kind";
}
