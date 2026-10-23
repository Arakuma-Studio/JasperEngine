// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The shadow slot planner (spec S10 R15, AC16): packing per-light resolutions without overlap,
// slots by importance, sticky slots with the challenge margin and cooldown, and the memory
// budget (static layers dropped first, then resolutions halved).

#include "render/shadow/ShadowSlotPlanner.h"
#include "render/shadow/ShadowTiers.h"

#include <gtest/gtest.h>

#include <vector>

using namespace jasperengine;

namespace {

    ShadowLightRequest light(uint32_t id, ShadowLightKind kind, float importance, unsigned int resolution, bool wantsStatic = false){
        ShadowLightRequest r;
        r.id = id;
        r.kind = kind;
        r.importance = importance;
        r.resolution = resolution;
        r.slots = (kind == ShadowLightKind::DIRECTIONAL) ? 4u : (kind == ShadowLightKind::POINT ? 6u : 1u);
        r.wantsStatic = wantsStatic;
        return r;
    }

    bool overlap(const ShadowRect& a, const ShadowRect& b){
        return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
    }

    void expectPacked(const ShadowAtlasLayout& layout){
        for (size_t i = 0; i < layout.rects.size(); i++){
            const ShadowRect& a = layout.rects[i];
            if (a.empty())
                continue;
            EXPECT_GE(a.x, 0);
            EXPECT_GE(a.y, 0);
            EXPECT_LE(a.x + a.width, (int)layout.width);
            EXPECT_LE(a.y + a.height, (int)layout.height);
            for (size_t j = i + 1; j < layout.rects.size(); j++){
                if (!layout.rects[j].empty())
                    EXPECT_FALSE(overlap(a, layout.rects[j])) << i << " and " << j;
            }
        }
    }

}

TEST(ShadowSlotPlanner, PacksMixedResolutionsTightly){
    // 2 x 1024 + 4 x 512: three 1024 squares of area, 2048 x 1536 without waste
    ShadowAtlasLayout layout;
    ASSERT_TRUE(ShadowSlotPlanner::pack({1024, 1024, 512, 512, 512, 512, 0, 0, 0}, 16384, layout));
    expectPacked(layout);
    EXPECT_EQ((uint64_t)layout.width * layout.height, 3ull * 1024 * 1024);
    EXPECT_TRUE(layout.rects[6].empty());

    // one size alone
    ASSERT_TRUE(ShadowSlotPlanner::pack({0, 2048}, 16384, layout));
    EXPECT_EQ(layout.width, 2048u);
    EXPECT_EQ(layout.height, 2048u);
    EXPECT_EQ(layout.rects[1], (ShadowRect{0, 0, 2048, 2048}));

    // too large for the device
    EXPECT_FALSE(ShadowSlotPlanner::pack({8192}, 4096, layout));

    // many small ones stay within the area of their sum plus a row
    std::vector<unsigned int> sizes(24, 256);
    ASSERT_TRUE(ShadowSlotPlanner::pack(sizes, 16384, layout));
    expectPacked(layout);
    EXPECT_EQ((uint64_t)layout.width * layout.height, 24ull * 256 * 256);
}

TEST(ShadowSlotPlanner, PowerOfTwoResolutions){
    EXPECT_EQ(ShadowSlotPlanner::powerOfTwoResolution(1000, 256, 4096), 512u);
    EXPECT_EQ(ShadowSlotPlanner::powerOfTwoResolution(1024, 256, 4096), 1024u);
    EXPECT_EQ(ShadowSlotPlanner::powerOfTwoResolution(8192, 256, 2048), 2048u);
    EXPECT_EQ(ShadowSlotPlanner::powerOfTwoResolution(16, 256, 2048), 256u);
}

TEST(ShadowSlotPlanner, GivesSlotsByImportanceSunFirst){
    ShadowSlotPlanner planner;
    ShadowSlotPlanner::Limits limits;
    std::vector<ShadowLightRequest> requests;
    for (uint32_t i = 0; i < 8; i++){
        requests.push_back(light(100 + i, ShadowLightKind::SPOT, 1.0f + (float)i, 512));
    }
    requests.push_back(light(1, ShadowLightKind::DIRECTIONAL, 0.1f, 2048));
    const ShadowPlan& plan = planner.plan(requests, limits, 0);
    // the sun takes 4 slots, the 5 most important spots the other 5
    EXPECT_EQ(plan.lights[8].firstSlot, 0);
    int assigned = 0;
    for (int i = 0; i < 8; i++){
        if (plan.lights[i].firstSlot >= 0){
            assigned++;
            EXPECT_GE(i, 3) << "a less important spot got a slot";
        }
    }
    EXPECT_EQ(assigned, 5);
    expectPacked(plan.projective);
}

TEST(ShadowSlotPlanner, SlotsAreStickyWithMarginAndCooldown){
    ShadowSlotPlanner planner;
    ShadowSlotPlanner::Limits limits;
    limits.projectiveSlots = 1;
    std::vector<ShadowLightRequest> requests = {light(1, ShadowLightKind::SPOT, 1.0f, 512)};
    EXPECT_EQ(planner.plan(requests, limits, 0).lights[0].firstSlot, 0);

    // a challenger 20 % stronger does not take it, a 30 % stronger one only after the cooldown
    requests.push_back(light(2, ShadowLightKind::SPOT, 1.2f, 512));
    EXPECT_EQ(planner.plan(requests, limits, 1).lights[0].firstSlot, 0);
    requests[1].importance = 1.3f;
    EXPECT_EQ(planner.plan(requests, limits, 10).lights[0].firstSlot, 0);
    const ShadowPlan& plan = planner.plan(requests, limits, 31);
    EXPECT_EQ(plan.lights[0].firstSlot, -1);
    EXPECT_EQ(plan.lights[1].firstSlot, 0);
    // and keeps it while the old holder is back to slightly stronger
    requests[0].importance = 1.5f;
    EXPECT_EQ(planner.plan(requests, limits, 32).lights[1].firstSlot, 0);
}

TEST(ShadowSlotPlanner, PointLightsTakeAlignedFaceGroups){
    ShadowSlotPlanner planner;
    ShadowSlotPlanner::Limits limits;
    std::vector<ShadowLightRequest> requests;
    for (uint32_t i = 0; i < 5; i++){
        requests.push_back(light(10 + i, ShadowLightKind::POINT, 1.0f + (float)i, 256));
    }
    const ShadowPlan& plan = planner.plan(requests, limits, 0);
    int assigned = 0;
    for (const ShadowLightAssignment& a : plan.lights){
        if (a.firstSlot >= 0){
            EXPECT_EQ(a.firstSlot % 6, 0);
            assigned++;
        }
    }
    EXPECT_EQ(assigned, 4);
    EXPECT_EQ(plan.lights[0].firstSlot, -1);
    expectPacked(plan.point);
}

TEST(ShadowSlotPlanner, MemoryBudgetDropsStaticLayersThenHalves){
    // the bench scene of the spec at each tier: 1 sun, 4 spots, 2 point lights
    for (int t = 0; t < 4; t++){
        const ShadowTier tier = static_cast<ShadowTier>(t);
        const ShadowTierSettings& settings = ShadowTiers::getSettings(tier);
        ShadowSlotPlanner planner;
        ShadowSlotPlanner::Limits limits;
        limits.maxResolution = settings.slotResolution;
        limits.memoryBytes = settings.memoryBytes;
        std::vector<ShadowLightRequest> requests;
        ShadowLightRequest sun = light(1, ShadowLightKind::DIRECTIONAL, 3.0f, settings.slotResolution, true);
        sun.slots = settings.maxCascades;
        requests.push_back(sun);
        for (uint32_t i = 0; i < 4; i++)
            requests.push_back(light(10 + i, ShadowLightKind::SPOT, 1.0f + (float)i, settings.spotResolution, true));
        for (uint32_t i = 0; i < 2; i++)
            requests.push_back(light(20 + i, ShadowLightKind::POINT, 1.0f + (float)i, settings.pointResolution, true));
        const ShadowPlan& plan = planner.plan(requests, limits, 0);
        EXPECT_LE(plan.memoryBytes, settings.memoryBytes) << ShadowTiers::getName(tier);
        for (const ShadowLightAssignment& a : plan.lights){
            EXPECT_GE(a.firstSlot, 0) << ShadowTiers::getName(tier);
        }
        expectPacked(plan.projective);
        expectPacked(plan.point);
        expectPacked(plan.projectiveStatic);
        expectPacked(plan.pointStatic);
        // the sun keeps the tier's resolution: what gives way is less important
        EXPECT_EQ(plan.lights[0].resolution[0], settings.slotResolution) << ShadowTiers::getName(tier);
    }

    // a budget below the atlases halves the least important first
    ShadowSlotPlanner planner;
    ShadowSlotPlanner::Limits limits;
    limits.memoryBytes = 2ull * 1024 * 1024 * 4 - 1;
    std::vector<ShadowLightRequest> requests = {
        light(1, ShadowLightKind::SPOT, 2.0f, 1024, true),
        light(2, ShadowLightKind::SPOT, 1.0f, 1024, true)};
    const ShadowPlan& plan = planner.plan(requests, limits, 0);
    EXPECT_LE(plan.memoryBytes, limits.memoryBytes);
    EXPECT_EQ(plan.lights[0].resolution[0], 1024u);
    EXPECT_LT(plan.lights[1].resolution[0], 1024u);
    EXPECT_TRUE(plan.reducedResolution);
}
