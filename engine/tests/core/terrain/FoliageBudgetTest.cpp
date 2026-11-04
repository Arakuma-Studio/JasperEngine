// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T5 AC31: R40's counts on the benchmark scene, every frame of R44's path, on each tier. Counts, not
// device time: they do not depend on the GPU.

#include "../bench/FoliageBenchScene.h"
#include "Engine.h"

#include <gtest/gtest.h>

using namespace foliagebench;

namespace {

    struct Caps {
        uint64_t details;
        uint64_t trees;
        uint64_t meshTrees;
    };

    void walk(FoliageQuality quality, const Caps& caps) {
        Engine::setFoliageQuality(quality);
        {
            Scene1 scene(std::string("t5-budget-") + std::to_string(static_cast<int>(quality)));
            scene.settle();
            FoliageViewList list;
            for (int i = 0; i < FRAMES; i += 10) {
                scene.setPose(i);
                Plane planes[6];
                const FoliageViewRequest request = scene.view(planes);
                scene.frame(FRAME_SECONDS * 10);
                scene.foliage->buildView(list, request);
                EXPECT_LE(list.detailVisible, caps.details) << "frame " << i;
                EXPECT_LE(list.treesVisible, caps.trees) << "frame " << i;
                EXPECT_LE(list.meshTrees, caps.meshTrees) << "frame " << i;
                scene.foliage->waitForJobs();
            }
            EXPECT_GT(list.detailVisible, 0u);
            EXPECT_GT(list.treesVisible, 0u);
        }
        Engine::resetFoliageQuality();
    }

}

TEST(FoliageBudget, BenchSceneWithinCountsHigh) {
    walk(FoliageQuality::HIGH, {400000, 30000, 500});
}

TEST(FoliageBudget, BenchSceneWithinCountsLow) {
    walk(FoliageQuality::LOW, {30000, 8000, 60});
}
