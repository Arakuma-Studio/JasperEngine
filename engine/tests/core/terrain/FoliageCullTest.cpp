// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T5 AC8, AC10, AC13 (bands), AC18 (caps), AC43 (near fade): what a view keeps

#include "terrain/FoliageCull.h"
#include "terrain/FoliageScatter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jasperengine::terrain;

namespace {

    // a box frustum: x in [-b, b], y in [-b, b], z in [-b, b]
    FoliageFrustum boxFrustum(float b) {
        FoliageFrustum frustum;
        const float planes[6][4] = {
            {1, 0, 0, b}, {-1, 0, 0, b}, {0, 1, 0, b}, {0, -1, 0, b}, {0, 0, 1, b}, {0, 0, -1, b}
        };
        std::copy(&planes[0][0], &planes[0][0] + 24, &frustum.planes[0][0]);
        return frustum;
    }

    FoliageInstance instanceAt(float x, float y, float z, uint8_t rank = 0) {
        FoliageInstance instance{};
        instance.position[0] = x;
        instance.position[1] = y;
        instance.position[2] = z;
        instance.scale = 1.0f;
        instance.rotation[3] = 32767;
        instance.heightRatio = 128;
        instance.densityRank = rank;
        return instance;
    }

    std::vector<FoliageInstance> grid(float extent, float step) {
        std::vector<FoliageInstance> out;
        uint8_t rank = 0;
        for (float z = -extent; z <= extent; z += step) {
            for (float x = -extent; x <= extent; x += step) {
                out.push_back(instanceAt(x, 0.0f, z, rank++));
            }
        }
        return out;
    }

    size_t cull(const std::vector<FoliageInstance>& in, const FoliageDetailCull& cull, const FoliageFrustum& frustum, FoliageCellVisibility visibility,
                std::vector<FoliageInstance>& out, std::vector<uint8_t>& lods, uint64_t* tests = nullptr) {
        out.resize(in.size());
        lods.resize(in.size());
        const size_t kept = cullFoliageCell(in.data(), in.size(), cull, frustum, visibility, out.data(), lods.data(), tests);
        out.resize(kept);
        lods.resize(kept);
        return kept;
    }

}

TEST(FoliageCull, KeepsExactlyTheInstancesInTheFrustum) {
    const std::vector<FoliageInstance> in = grid(10.0f, 1.0f);
    FoliageDetailCull params;
    params.limitDistance = false;
    params.meshRadius = 0.0f;
    const FoliageFrustum frustum = boxFrustum(5.0f);
    std::vector<FoliageInstance> out;
    std::vector<uint8_t> lods;
    cull(in, params, frustum, FoliageCellVisibility::Intersecting, out, lods);
    size_t expected = 0;
    for (const FoliageInstance& instance : in) {
        if (std::abs(instance.position[0]) <= 5.0f && std::abs(instance.position[2]) <= 5.0f) expected++;
    }
    EXPECT_EQ(out.size(), expected);
    for (const FoliageInstance& instance : out) {
        EXPECT_LE(std::abs(instance.position[0]), 5.0f);
        EXPECT_LE(std::abs(instance.position[2]), 5.0f);
    }

    // the sphere counts: a radius lets the next row in
    params.meshRadius = 1.0f;
    cull(in, params, frustum, FoliageCellVisibility::Intersecting, out, lods);
    size_t wider = 0;
    for (const FoliageInstance& instance : in) {
        if (std::abs(instance.position[0]) <= 6.0f && std::abs(instance.position[2]) <= 6.0f) wider++;
    }
    EXPECT_EQ(out.size(), wider);
}

TEST(FoliageCull, InsideCellsSkipPlaneTests) {
    const std::vector<FoliageInstance> in = grid(2.0f, 1.0f);
    FoliageDetailCull params;
    params.limitDistance = false;
    const FoliageFrustum frustum = boxFrustum(100.0f);
    const float boxMin[3] = {-2.0f, 0.0f, -2.0f};
    const float boxMax[3] = {2.0f, 0.0f, 2.0f};
    EXPECT_EQ(classifyFoliageBox(frustum, boxMin, boxMax, 1.0f), FoliageCellVisibility::Inside);

    const float crossing[3] = {99.5f, 0.0f, 0.0f};
    const float crossingEnd[3] = {101.0f, 0.0f, 1.0f};
    EXPECT_EQ(classifyFoliageBox(frustum, crossing, crossingEnd, 1.0f), FoliageCellVisibility::Intersecting);
    const float gone[3] = {120.0f, 0.0f, 0.0f};
    const float goneEnd[3] = {130.0f, 0.0f, 1.0f};
    EXPECT_EQ(classifyFoliageBox(frustum, gone, goneEnd, 1.0f), FoliageCellVisibility::Outside);

    std::vector<FoliageInstance> out;
    std::vector<uint8_t> lods;
    uint64_t tests = 0;
    cull(in, params, frustum, FoliageCellVisibility::Inside, out, lods, &tests);
    EXPECT_EQ(tests, 0u);
    EXPECT_EQ(out.size(), in.size());
    cull(in, params, frustum, FoliageCellVisibility::Intersecting, out, lods, &tests);
    EXPECT_GE(tests, in.size() * 6);
    EXPECT_EQ(out.size(), in.size());
}

TEST(FoliageCull, LodBucketsMatchSelectMeshLod) {
    FoliageLodTable table;
    table.enabled = true;
    table.numLods = 3;
    table.error[1] = 0.01f;
    table.error[2] = 0.05f;
    table.threshold = 1.0f;
    table.perUnit = 0.002f;
    table.constant = 0.0f;
    table.bias = 1.0f;

    // RenderSystem::selectMeshLod's rule, on the same numbers
    const auto expected = [&](float distance, float scale) {
        const float allowed = table.threshold * (table.perUnit * std::max(distance, 0.0f) + table.constant) / std::max(table.bias, 0.001f);
        unsigned int level = 0;
        for (unsigned int l = 1; l < table.numLods; l++) {
            if (table.error[l] * scale > allowed) break;
            level = l;
        }
        return level;
    };

    for (float distance = 0.0f; distance < 100.0f; distance += 0.7f) {
        EXPECT_EQ(selectFoliageLod(table, distance, 1.0f), expected(distance, 1.0f));
        EXPECT_EQ(selectFoliageLod(table, distance, 3.0f), expected(distance, 3.0f));
    }

    FoliageDetailCull params;
    params.limitDistance = false;
    params.lod = table;
    std::vector<FoliageInstance> in;
    for (int i = 0; i < 64; i++) in.push_back(instanceAt(static_cast<float>(i), 0.0f, 0.0f));
    std::vector<FoliageInstance> out;
    std::vector<uint8_t> lods;
    cull(in, params, boxFrustum(1000.0f), FoliageCellVisibility::Inside, out, lods);
    ASSERT_EQ(out.size(), in.size());
    for (size_t i = 0; i < out.size(); i++) {
        EXPECT_EQ(lods[i], expected(static_cast<float>(i), 1.0f));
    }
    EXPECT_EQ(lods.front(), 0);
    EXPECT_EQ(lods.back(), 2);
}

TEST(FoliageCull, DensityIsAStableSubset) {
    std::vector<FoliageInstance> in;
    for (int rank = 0; rank < 256; rank++) in.push_back(instanceAt(0.0f, 0.0f, static_cast<float>(rank) * 0.01f, static_cast<uint8_t>(rank)));
    FoliageDetailCull params;
    params.limitDistance = false;
    const FoliageFrustum frustum = boxFrustum(100.0f);

    std::vector<FoliageInstance> previous;
    std::vector<uint8_t> lods;
    for (float density = 0.0f; density <= 1.0f; density += 0.05f) {
        params.density = density;
        std::vector<FoliageInstance> kept;
        cull(in, params, frustum, FoliageCellVisibility::Inside, kept, lods);
        // what a lower density kept, a higher one keeps too: changing it shows a stable subset
        for (const FoliageInstance& instance : previous) {
            const bool stays = std::any_of(kept.begin(), kept.end(), [&](const FoliageInstance& other) { return other.densityRank == instance.densityRank; });
            EXPECT_TRUE(stays);
        }
        EXPECT_NEAR(static_cast<float>(kept.size()) / 256.0f, density, 1.0f / 256.0f + 1e-5f);
        previous = kept;
    }

    params.density = 1.0f;
    std::vector<FoliageInstance> all;
    cull(in, params, frustum, FoliageCellVisibility::Inside, all, lods);
    EXPECT_EQ(all.size(), in.size()) << "at 1, all draw";
}

TEST(FoliageCull, FalloffFollowsTheCurve) {
    FoliageDetailCull params;
    params.falloffStart = 50.0f;
    params.falloffEnd = 100.0f;
    params.farDensity = 0.3f;
    EXPECT_FLOAT_EQ(foliageDensityFalloff(params, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(foliageDensityFalloff(params, 50.0f), 1.0f);
    EXPECT_NEAR(foliageDensityFalloff(params, 75.0f), 0.65f, 1e-5f);
    EXPECT_FLOAT_EQ(foliageDensityFalloff(params, 100.0f), 0.3f);
    EXPECT_FLOAT_EQ(foliageDensityFalloff(params, 500.0f), 0.3f);

    // no effect with the defaults
    FoliageDetailCull off;
    EXPECT_FLOAT_EQ(foliageDensityFalloff(off, 80.0f), 1.0f);
}

TEST(FoliageCull, FadeMatchesTheOldShaderRange) {
    // mesh.vert's: clamp((end - d) / (end - start)), on XZ, start at 75 % of the distance
    FoliageDetailCull params;
    params.drawDistance = 100.0f;
    params.fadeStart = 100.0f * FOLIAGE_FADE_START;
    for (float d = 0.0f; d <= 120.0f; d += 1.3f) {
        const float expected = std::clamp((params.drawDistance - d) / (params.drawDistance - params.fadeStart), 0.0f, 1.0f);
        EXPECT_FLOAT_EQ(foliageFadeScale(params, d), expected);
    }

    std::vector<FoliageInstance> in = {instanceAt(90.0f, 50.0f, 0.0f), instanceAt(10.0f, 0.0f, 0.0f), instanceAt(101.0f, 0.0f, 0.0f)};
    std::vector<FoliageInstance> out;
    std::vector<uint8_t> lods;
    cull(in, params, boxFrustum(1000.0f), FoliageCellVisibility::Inside, out, lods);
    ASSERT_EQ(out.size(), 2u);
    // height stays out of the distance, as the shader's XZ
    EXPECT_FLOAT_EQ(out[0].scale, 0.4f);
    EXPECT_FLOAT_EQ(out[1].scale, 1.0f);
}

TEST(FoliageCull, MinDrawDistanceFadesAndSkips) {
    FoliageDetailCull params;
    params.drawDistance = 300.0f;
    params.fadeStart = 225.0f;
    params.minDistance = 40.0f;
    params.nearFadeStart = 40.0f * FOLIAGE_FADE_START;
    EXPECT_FLOAT_EQ(foliageFadeScale(params, 20.0f), 0.0f);
    EXPECT_FLOAT_EQ(foliageFadeScale(params, 35.0f), 0.5f);
    EXPECT_FLOAT_EQ(foliageFadeScale(params, 40.0f), 1.0f);

    // a cell whose farthest corner is nearer than the start is skipped
    const float nearMin[3] = {0.0f, 0.0f, 0.0f};
    const float nearMax[3] = {8.0f, 0.0f, 8.0f};
    EXPECT_FALSE(foliageCellInDistance(params, nearMin, nearMax));
    const float farMin[3] = {32.0f, 0.0f, 0.0f};
    const float farMax[3] = {40.0f, 0.0f, 8.0f};
    EXPECT_TRUE(foliageCellInDistance(params, farMin, farMax));
    const float pastMin[3] = {320.0f, 0.0f, 0.0f};
    const float pastMax[3] = {328.0f, 0.0f, 8.0f};
    EXPECT_FALSE(foliageCellInDistance(params, pastMin, pastMax));
}

TEST(FoliageCull, ScaleZeroRemovesFoliage) {
    // Foliage Distance 0: every effective distance is 0, and nothing passes
    FoliageDetailCull params;
    params.drawDistance = 0.0f;
    params.fadeStart = 0.0f;
    std::vector<FoliageInstance> in = {instanceAt(0.5f, 0.0f, 0.0f), instanceAt(30.0f, 0.0f, 0.0f)};
    std::vector<FoliageInstance> out;
    std::vector<uint8_t> lods;
    cull(in, params, boxFrustum(1000.0f), FoliageCellVisibility::Inside, out, lods);
    EXPECT_TRUE(out.empty());
    const float boxMin[3] = {0.0f, 0.0f, 0.0f};
    const float boxMax[3] = {8.0f, 0.0f, 8.0f};
    EXPECT_FALSE(foliageCellInDistance(params, boxMin, boxMax));
}

TEST(FoliageTrees, CellLodBandsFromDistance) {
    FoliageTreeBandSettings settings;
    settings.treeDistance = 500.0f;
    settings.billboardStart = 80.0f;
    settings.fadeLength = 5.0f;
    settings.hasImpostor = true;
    settings.explicitLods = 2;
    settings.lodDistanceCount = 2;
    settings.lodDistances[0] = 20.0f;
    settings.lodDistances[1] = 50.0f;

    FoliageTreeBands near = selectFoliageTreeBands(settings, 0.0f, 10.0f, true);
    EXPECT_TRUE(near.mesh);
    EXPECT_EQ(near.meshLod, 0u);
    EXPECT_FALSE(near.impostor);

    FoliageTreeBands middle = selectFoliageTreeBands(settings, 30.0f, 45.0f, true);
    EXPECT_TRUE(middle.mesh);
    EXPECT_EQ(middle.meshLod, 1u);
    EXPECT_FALSE(middle.impostor);

    FoliageTreeBands fade = selectFoliageTreeBands(settings, 70.0f, 90.0f, true);
    EXPECT_TRUE(fade.mesh);
    EXPECT_EQ(fade.meshLod, 2u);
    EXPECT_TRUE(fade.impostor) << "in the fade band a cell draws both";

    FoliageTreeBands far = selectFoliageTreeBands(settings, 200.0f, 230.0f, true);
    EXPECT_FALSE(far.mesh);
    EXPECT_TRUE(far.impostor);

    FoliageTreeBands gone = selectFoliageTreeBands(settings, 600.0f, 630.0f, true);
    EXPECT_FALSE(gone.visible);
}

TEST(FoliageTrees, MaxMeshTreesFallsBackToImpostors) {
    FoliageTreeBandSettings settings;
    settings.hasImpostor = true;
    FoliageTreeBands capped = selectFoliageTreeBands(settings, 5.0f, 20.0f, false);
    EXPECT_FALSE(capped.mesh);
    EXPECT_TRUE(capped.impostor);

    // no impostor: past the cap there is nothing cheaper
    settings.hasImpostor = false;
    FoliageTreeBands none = selectFoliageTreeBands(settings, 5.0f, 20.0f, false);
    EXPECT_FALSE(none.visible);
}

TEST(FoliageTrees, NoImpostorKeepsTheMeshToTreeDistance) {
    FoliageTreeBandSettings settings;
    settings.treeDistance = 300.0f;
    settings.billboardStart = 80.0f;
    settings.fadeLength = 10.0f;
    EXPECT_TRUE(selectFoliageTreeBands(settings, 200.0f, 220.0f, true).mesh);
    EXPECT_FALSE(selectFoliageTreeBands(settings, 310.0f, 330.0f, true).visible);
    EXPECT_FLOAT_EQ(foliageMeshFade(settings, 295.0f), 0.5f);
    EXPECT_FLOAT_EQ(foliageMeshFade(settings, 200.0f), 1.0f);
    EXPECT_FLOAT_EQ(foliageImpostorFade(settings, 200.0f), 0.0f);
}

TEST(FoliageTrees, FadeFactorsAreComplementaryInTheBand) {
    FoliageTreeBandSettings settings;
    settings.treeDistance = 1000.0f;
    settings.billboardStart = 80.0f;
    settings.fadeLength = 5.0f;
    settings.hasImpostor = true;
    for (float d = 70.0f; d < 90.0f; d += 0.5f) {
        EXPECT_NEAR(foliageMeshFade(settings, d) + foliageImpostorFade(settings, d), 1.0f, 1e-5f) << d;
    }
    EXPECT_FLOAT_EQ(foliageMeshFade(settings, 74.0f), 1.0f);
    EXPECT_FLOAT_EQ(foliageMeshFade(settings, 80.0f), 0.0f);
    EXPECT_FLOAT_EQ(foliageImpostorFade(settings, 997.5f), 0.5f);
}

TEST(FoliageSettings, CapsDropTheFarthestFirst) {
    std::vector<FoliageCapCandidate> candidates;
    for (uint32_t i = 0; i < 10; i++) {
        FoliageCapCandidate candidate;
        candidate.distance = static_cast<float>(10 - i);    // the last is the nearest
        candidate.order = i;
        candidate.instances = 100;
        candidate.draws = 2;
        candidates.push_back(candidate);
    }
    std::vector<FoliageCapCandidate> byInstances = candidates;
    EXPECT_TRUE(applyFoliageCaps(byInstances, 350, 1000));
    for (uint32_t i = 0; i < 10; i++) {
        EXPECT_EQ(byInstances[i].kept, i >= 7) << i;
    }

    std::vector<FoliageCapCandidate> byDraws = candidates;
    EXPECT_TRUE(applyFoliageCaps(byDraws, 100000, 4, 0));
    EXPECT_TRUE(byDraws[9].kept);
    EXPECT_TRUE(byDraws[8].kept);
    EXPECT_FALSE(byDraws[7].kept);

    std::vector<FoliageCapCandidate> all = candidates;
    EXPECT_FALSE(applyFoliageCaps(all, 1000, 1000));
    for (const FoliageCapCandidate& candidate : all) EXPECT_TRUE(candidate.kept);

    // deterministic: ties fall to the canonical order
    std::vector<FoliageCapCandidate> ties = candidates;
    for (FoliageCapCandidate& candidate : ties) candidate.distance = 5.0f;
    applyFoliageCaps(ties, 300, 1000);
    for (uint32_t i = 0; i < 10; i++) EXPECT_EQ(ties[i].kept, i < 3);
}

TEST(FoliageSettings, TierTableMatchesTheSpec) {
    const FoliageTierLimits low = foliageTierLimits(FoliageTier::Low);
    EXPECT_FLOAT_EQ(low.detailDensity, 0.5f);
    EXPECT_FLOAT_EQ(low.fadeLength, 0.5f);
    EXPECT_EQ(low.maxDetailInstances, 30000u);
    EXPECT_EQ(low.maxMeshTreesCap, 60u);
    EXPECT_EQ(low.maxVisibleTrees, 8000u);
    EXPECT_EQ(low.maxDraws, 150u);
    EXPECT_FALSE(low.detailShadows);
    EXPECT_FALSE(low.impostorBlend);

    const FoliageTierLimits medium = foliageTierLimits(FoliageTier::Medium);
    EXPECT_FLOAT_EQ(medium.detailDensity, 0.75f);
    EXPECT_EQ(medium.maxDetailInstances, 120000u);
    EXPECT_EQ(medium.maxMeshTreesCap, 200u);
    EXPECT_EQ(medium.maxDraws, 400u);

    const FoliageTierLimits high = foliageTierLimits(FoliageTier::High);
    EXPECT_FLOAT_EQ(high.detailDensity, 1.0f);
    EXPECT_EQ(high.maxDetailInstances, 400000u);
    EXPECT_EQ(high.maxMeshTreesCap, 500u);
    EXPECT_EQ(high.maxVisibleTrees, 30000u);
    EXPECT_EQ(high.maxDraws, 1200u);
    EXPECT_TRUE(high.impostorBlend);
}
