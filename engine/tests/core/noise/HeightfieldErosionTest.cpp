// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T2 AC16-AC17: erosion's bits and behaviour.

#include "NoiseTestSupport.h"
#include "jobs/Primitives.h"
#include "noise/NoiseKernels.h"
#include "noise/NoiseNodes.h"
#include "terrain/HeightfieldErosion.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::terrain;
using namespace noisetest;

namespace {

    // Heights of a fractal, the ground erosion is for
    std::vector<float> fractalHeights(int size, float frequency = 4.0f, uint32_t seed = 3u) {
        noise::FractalParams params;
        params.basis.basis = noise::Basis::Perlin;
        params.basis.frequency = frequency;
        params.basis.seed = seed;
        params.octaves = 5;
        std::vector<float> heights(static_cast<size_t>(size) * size);
        for (int z = 0; z < size; z++) {
            for (int x = 0; x < size; x++) {
                const float px = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                const float pz = (static_cast<float>(z) + 0.5f) / static_cast<float>(size);
                heights[static_cast<size_t>(z) * size + x] = (noise::fbmPoint(params, px, 0.0f, pz) + 1.0f) * 0.5f;
            }
        }
        return heights;
    }

    ErosionRegion regionOf(int size, float worldSize = 1000.0f, float maxHeight = 400.0f) {
        ErosionRegion region;
        region.width = size;
        region.depth = size;
        region.maxHeight = maxHeight;
        region.metresPerTexel = static_cast<double>(worldSize) / static_cast<double>(size);
        return region;
    }

    double total(const std::vector<float>& values) {
        double sum = 0.0;
        for (float value : values) sum += value;
        return sum;
    }

    float percentile(std::vector<float> values, float p) {
        std::sort(values.begin(), values.end());
        return values[static_cast<size_t>(p * static_cast<float>(values.size() - 1))];
    }

    // Mean absolute curvature over a stencil of that many texels. The broad one is the terrain's shape,
    // which erosion wears down; at one texel a droplet's own cuts count.
    double curvature(const std::vector<float>& h, int size, int stencil) {
        double sum = 0.0;
        const size_t row = static_cast<size_t>(stencil) * static_cast<size_t>(size);
        for (int z = stencil; z + stencil < size; z++) {
            for (int x = stencil; x + stencil < size; x++) {
                const size_t i = static_cast<size_t>(z) * size + x;
                sum += std::fabs(h[i - stencil] + h[i + stencil] + h[i - row] + h[i + row] - 4.0f * h[i]);
            }
        }
        return sum / static_cast<double>((size - 2 * stencil) * (size - 2 * stencil));
    }

    float steepest(const std::vector<float>& h, int size, const ErosionRegion& region) {
        float top = 0.0f;
        for (int z = 0; z < size; z++) {
            for (int x = 0; x + 1 < size; x++) {
                const size_t i = static_cast<size_t>(z) * size + x;
                top = std::max(top, std::fabs(h[i + 1] - h[i]) * region.maxHeight / static_cast<float>(region.metresPerTexel));
            }
        }
        return top;
    }

}

TEST(HeightfieldErosion, SameBitsForAnyWorkerCount) {
    const int size = 257;
    const std::vector<float> start = fractalHeights(size);
    uint64_t expectedHydraulic = 0, expectedFlow = 0, expectedThermal = 0;
    HydraulicParams hydraulic;
    hydraulic.dropletsPerTexel = 0.5f;
    hydraulic.seed = 9u;
    ThermalParams thermal;
    for (int workers : {0, 1, 3, 7}) {
        WorkerScope scope(workers);
        std::vector<float> h = start, flow(start.size()), sediment(start.size());
        ASSERT_TRUE(HeightfieldErosion::hydraulic(hydraulic, regionOf(size), h.data(), nullptr, flow.data(), sediment.data()));
        std::vector<float> t = start;
        ASSERT_TRUE(HeightfieldErosion::thermal(thermal, regionOf(size), t.data()));
        const uint64_t hashH = noise::hashFloats(h.data(), h.size());
        const uint64_t hashF = noise::hashFloats(flow.data(), flow.size());
        const uint64_t hashT = noise::hashFloats(t.data(), t.size());
        if (workers == 0) {
            expectedHydraulic = hashH;
            expectedFlow = hashF;
            expectedThermal = hashT;
        }
        EXPECT_EQ(hashH, expectedHydraulic) << workers << " workers";
        EXPECT_EQ(hashF, expectedFlow) << workers << " workers";
        EXPECT_EQ(hashT, expectedThermal) << workers << " workers";
    }
}

TEST(HeightfieldErosion, GoldenHash) {
    // Pinned through the table on every target (NoiseGolden.AllCasesMatch, erosion/direct/*); here the
    // same parameters must be repeatable in one process and change with the seed
    const std::vector<float> start = fractalHeights(65);
    HydraulicParams params;
    params.seed = 5u;
    ErosionControl serial;
    serial.parallel = false;
    std::vector<float> a = start, b = start, c = start;
    HeightfieldErosion::hydraulic(params, regionOf(65), a.data(), nullptr, nullptr, nullptr, serial);
    HeightfieldErosion::hydraulic(params, regionOf(65), b.data(), nullptr, nullptr, nullptr, serial);
    params.seed = 6u;
    HeightfieldErosion::hydraulic(params, regionOf(65), c.data(), nullptr, nullptr, nullptr, serial);
    EXPECT_EQ(noise::hashFloats(a.data(), a.size()), noise::hashFloats(b.data(), b.size()));
    EXPECT_NE(noise::hashFloats(a.data(), a.size()), noise::hashFloats(c.data(), c.size()));
}

TEST(HeightfieldErosion, ConservesMaterial) {
    const int size = 129;
    const std::vector<float> start = fractalHeights(size);
    std::vector<float> h = start;
    HydraulicParams params;
    params.seed = 2u;
    uint64_t steps = 0;
    ASSERT_TRUE(HeightfieldErosion::hydraulic(params, regionOf(size), h.data(), nullptr, nullptr, nullptr, {}, &steps));
    EXPECT_GT(steps, 0u) << "a run reports its droplet steps (R26)";
    EXPECT_NEAR(total(h) / total(start), 1.0, 1e-3);
    EXPECT_NE(noise::hashFloats(h.data(), h.size()), noise::hashFloats(start.data(), start.size()));
}

TEST(HeightfieldErosion, CarvesChannels) {
    const int size = 257;
    const std::vector<float> start = fractalHeights(size);
    std::vector<float> h = start, flow(start.size());
    HydraulicParams params;
    params.seed = 2u;       // everything else as a person gets it
    ASSERT_TRUE(HeightfieldErosion::hydraulic(params, regionOf(size), h.data(), nullptr, flow.data(), nullptr));
    const float median = percentile(flow, 0.5f);
    const float p99 = percentile(flow, 0.99f);
    EXPECT_GT(p99, median * 5.0f) << "water gathers: p99 " << p99 << " median " << median;
    for (float value : flow) {
        ASSERT_GE(value, 0.0f);
        ASSERT_LE(value, 1.0f);
    }
    // AC17, at the defaults and at both scales: the broad shape wears down, and no texel-scale roughness appears
    EXPECT_LT(curvature(h, size, 4), curvature(start, size, 4));
    EXPECT_LT(curvature(h, size, 1), curvature(start, size, 1) * 1.1);
}

TEST(HeightfieldErosion, HardnessSlowsTheCut) {
    const int size = 129;
    const std::vector<float> start = fractalHeights(size);
    HydraulicParams params;
    params.seed = 2u;
    std::vector<float> soft = start, hard = start;
    const std::vector<float> hardness(start.size(), 0.9f);
    ASSERT_TRUE(HeightfieldErosion::hydraulic(params, regionOf(size), soft.data(), nullptr, nullptr, nullptr));
    ASSERT_TRUE(HeightfieldErosion::hydraulic(params, regionOf(size), hard.data(), hardness.data(), nullptr, nullptr));
    double softChange = 0.0, hardChange = 0.0;
    for (size_t i = 0; i < start.size(); i++) {
        softChange += std::fabs(soft[i] - start[i]);
        hardChange += std::fabs(hard[i] - start[i]);
    }
    EXPECT_LT(hardChange, softChange * 0.6);
}

// R26: any resolution erodes alike in world terms
TEST(HeightfieldErosion, ResolutionScales) {
    const auto meanChange = [](int size) {
        const std::vector<float> start = fractalHeights(size);
        HydraulicParams params;
        params.seed = 2u;
        std::vector<float> h = start;
        HeightfieldErosion::hydraulic(params, regionOf(size), h.data(), nullptr, nullptr, nullptr);
        double change = 0.0;
        for (size_t i = 0; i < start.size(); i++) change += std::fabs(h[i] - start[i]);
        return change / static_cast<double>(start.size());
    };
    const double coarse = meanChange(129);
    const double fine = meanChange(257);
    EXPECT_GT(coarse, 0.0);
    EXPECT_LT(std::max(coarse, fine) / std::min(coarse, fine), 2.5) << coarse << " at 129, " << fine << " at 257";
}

TEST(HeightfieldErosion, ThermalRespectsTalus) {
    const int size = 65;
    std::vector<float> h(static_cast<size_t>(size) * size);
    for (int z = 0; z < size; z++) {
        for (int x = 0; x < size; x++) {
            h[static_cast<size_t>(z) * size + x] = x < size / 2 ? 0.2f : 0.8f;
        }
    }
    const ErosionRegion region = regionOf(size, 640.0f, 200.0f);
    const float before = steepest(h, size, region);
    ThermalParams params;
    params.iterations = 64;
    params.talusAngle = 35.0f;
    params.rate = 1.0f;
    ASSERT_TRUE(HeightfieldErosion::thermal(params, region, h.data()));
    const float after = steepest(h, size, region);
    EXPECT_LT(after, before * 0.5f);
    // Above the talus it still moves; under it nothing does
    std::vector<float> gentle(h.size());
    for (int z = 0; z < size; z++) {
        for (int x = 0; x < size; x++) gentle[static_cast<size_t>(z) * size + x] = static_cast<float>(x) * 0.001f;
    }
    const std::vector<float> copy = gentle;
    ASSERT_TRUE(HeightfieldErosion::thermal(params, region, gentle.data()));
    EXPECT_EQ(noise::hashFloats(gentle.data(), gentle.size()), noise::hashFloats(copy.data(), copy.size()));
}

TEST(HeightfieldErosion, ThermalConserves) {
    const int size = 129;
    const std::vector<float> start = fractalHeights(size, 16.0f);
    std::vector<float> h = start;
    ThermalParams params;
    params.iterations = 32;
    params.talusAngle = 10.0f;
    ASSERT_TRUE(HeightfieldErosion::thermal(params, regionOf(size), h.data()));
    EXPECT_NE(noise::hashFloats(h.data(), h.size()), noise::hashFloats(start.data(), start.size()));
    EXPECT_NEAR(total(h) / total(start), 1.0, 1e-4);
}

TEST(HeightfieldErosion, ThermalAnyThreadOrder) {
    const int size = 129;
    const std::vector<float> start = fractalHeights(size, 16.0f);
    ThermalParams params;
    params.talusAngle = 10.0f;
    ErosionControl serial;
    serial.parallel = false;
    std::vector<float> a = start, b = start;
    ASSERT_TRUE(HeightfieldErosion::thermal(params, regionOf(size), a.data(), serial));
    ASSERT_TRUE(HeightfieldErosion::thermal(params, regionOf(size), b.data()));
    EXPECT_EQ(noise::hashFloats(a.data(), a.size()), noise::hashFloats(b.data(), b.size()));
}

TEST(HeightfieldErosion, CancelStopsWithinATile) {
    const int size = 257;
    std::vector<float> h = fractalHeights(size);
    jobs::CancelToken cancel;
    cancel.cancel();
    ErosionControl control;
    control.cancel = &cancel;
    HydraulicParams params;
    uint64_t steps = 0;
    EXPECT_FALSE(HeightfieldErosion::hydraulic(params, regionOf(size), h.data(), nullptr, nullptr, nullptr, control, &steps));
    EXPECT_EQ(steps, 0u) << "cancelled before the first tile";
    ThermalParams thermal;
    EXPECT_FALSE(HeightfieldErosion::thermal(thermal, regionOf(size), h.data(), control));
}

TEST(HeightfieldErosion, RefusesOverBudget) {
    GraphBuilder b;
    const std::string fbm = b.node("fbm");
    const std::string hydraulic = b.node("hydraulic_erosion", {{"droplets_per_texel", 4.0}, {"lifetime", 128}});
    b.edge(fbm, hydraulic, "height");
    b.output(hydraulic);
    noise::CompileOptions big;
    big.resolution = 4096;
    const noise::CompileResult refused = compiled(b.graph(), big);
    EXPECT_EQ(refused.graph, nullptr);
    EXPECT_TRUE(hasIssue(refused, "erosion-budget", hydraulic));
    noise::CompileOptions small;
    small.resolution = 257;
    EXPECT_NE(compiled(b.graph(), small).graph, nullptr);

    GraphBuilder c;
    const std::string thermal = c.node("thermal_erosion", {{"iterations", 64}});
    c.edge(c.node("fbm"), thermal, "height");
    c.output(thermal);
    big.resolution = 32768;
    EXPECT_TRUE(hasIssue(compiled(c.graph(), big), "erosion-budget", thermal));

    // Also through what validate_graph runs
    noise::registerNoiseKind();
    b.graph().metadata["noise"] = {{"resolution", 4096}};
    bool found = false;
    for (const graph::Issue& issue : graph::validate(b.graph())) found = found || issue.code == "erosion-budget";
    EXPECT_TRUE(found);
}
