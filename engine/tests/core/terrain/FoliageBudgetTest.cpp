// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T5 AC31: R40's counts on the benchmark scene along R44's path, on each tier, through whole frames.
// Counts, not device time: they do not depend on the GPU.

#include "../bench/FoliageBenchScene.h"
#include "Engine.h"
#include "subsystem/RenderSystem.h"

#include "GpuEnvironment.h"

#include <gtest/gtest.h>

using namespace foliagebench;
using namespace jasperengine::test;

namespace {

    struct Caps {
        uint64_t details;
        uint64_t trees;
        uint64_t meshTrees;
        uint32_t draws;
    };

    void walk(FoliageQuality quality, const Caps& caps) {
        Engine::setFoliageQuality(quality);
        {
            Scene1 bench(std::string("t5-budget-") + std::to_string(static_cast<int>(quality)));
            bench.addImpostors();
            bench.camera->setFarClip(1200.0f);
            bench.camera->activate();
            Engine::setScene(bench.scene.get());
            bench.setPose(0);
            Scene* current = bench.scene.get();
            ASSERT_TRUE(GpuEnvironment::runFrames([current]() {
                return !current->getSystem<FoliageSystem>()->hasPendingUpdates() && current->getSystem<RenderSystem>()->isAllLoaded();
            }, 2000));

            uint64_t seenDetails = 0;
            uint64_t seenTrees = 0;
            for (int i = 0; i < FRAMES; i += 60) {
                bench.setPose(i);
                GpuEnvironment::drawFrames(1);
                const FoliageStats& stats = bench.foliage->getStats(bench.entity);
                EXPECT_LE(stats.detailVisible, caps.details) << "frame " << i;
                EXPECT_LE(stats.treesVisible, caps.trees) << "frame " << i;
                EXPECT_LE(stats.meshTrees, caps.meshTrees) << "frame " << i;
                EXPECT_LE(stats.draws, caps.draws) << "frame " << i;
                seenDetails = std::max(seenDetails, stats.detailVisible);
                seenTrees = std::max(seenTrees, stats.treesVisible);
            }
            std::printf("[budget] tier %d: %llu details, %llu trees at most\n", static_cast<int>(quality),
                        static_cast<unsigned long long>(seenDetails), static_cast<unsigned long long>(seenTrees));
            EXPECT_GT(seenDetails, 0u);
            EXPECT_GT(seenTrees, 0u);
            Engine::removeScene(bench.scene.get());
        }
        Engine::resetFoliageQuality();
    }

}

TEST(FoliageBudget, BenchSceneWithinCountsHigh) {
    JASPER_REQUIRE_GPU();
    walk(FoliageQuality::HIGH, {400000, 30000, 500, 1200});
}

TEST(FoliageBudget, BenchSceneWithinCountsLow) {
    JASPER_REQUIRE_GPU();
    walk(FoliageQuality::LOW, {30000, 8000, 60, 150});
}
