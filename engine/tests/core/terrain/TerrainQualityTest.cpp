// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T3 R1-R3, R8, R14: the tier table, its resolution, the layer selection, the cost formula and the
// macro normal bake (AC3, AC9, AC18's CPU half).

#include "terrain/TerrainLayers.h"
#include "terrain/TerrainMacroNormal.h"
#include "terrain/TerrainMaterialCost.h"
#include "terrain/TerrainQuality.h"
#include "component/TerrainComponent.h"
#include "subsystem/RenderSystem.h"
#include "texture/MaterialPbr.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace jasperengine::terrain;

TEST(TerrainQuality, Table) {
    EXPECT_EQ(terrainTierInfo(TerrainQuality::HIGH).layersPerPixel, 0);
    EXPECT_EQ(terrainTierInfo(TerrainQuality::HIGH).limitedLayersPerPixel, 4);
    EXPECT_EQ(terrainTierInfo(TerrainQuality::MEDIUM).layersPerPixel, 3);
    EXPECT_EQ(terrainTierInfo(TerrainQuality::LOW).layersPerPixel, 2);
    EXPECT_EQ(terrainTierInfo(TerrainQuality::MEDIUM).readsPerMap, 2);
    EXPECT_EQ(terrainTierInfo(TerrainQuality::LOW).readsPerMap, 1);
    EXPECT_FALSE(terrainTierInfo(TerrainQuality::LOW).surfaceMaps);
    EXPECT_EQ(terrainTierInfo(TerrainQuality::LOW).detailArraySize, 1024);
    TerrainQuality parsed = TerrainQuality::HIGH;
    EXPECT_TRUE(terrainQualityFromName("low", parsed));
    EXPECT_EQ(parsed, TerrainQuality::LOW);
    EXPECT_FALSE(terrainQualityFromName("ultra", parsed));
    EXPECT_STREQ(terrainQualityName(TerrainQuality::MEDIUM), "medium");
}

// R53 with the cap: a terrain with no more layers than High's limited count keeps them all, so it takes no
// TERRAIN_LAYER_LIMIT (the same image, and llvmpipe's GL path dies in that variant without layers)
TEST(TerrainQuality, LayerLimitOnlyWhenItCuts) {
    jasperengine::MeshShaderEnvironment environment;
    environment.terrainForced = true;
    environment.terrainForcedTier = TerrainQuality::HIGH;
    TerrainVariantInputs inputs;
    inputs.limitLayers = true;
    const uint64_t limitBit = jasperengine::MaterialPbr::getDefineBit("TERRAIN_LAYER_LIMIT");
    const int cap = terrainTierInfo(TerrainQuality::HIGH).limitedLayersPerPixel;
    for (int layers = 0; layers <= 12; layers++) {
        inputs.layerCount = layers;
        for (bool surface : {false, true}) {
            const bool limited = (jasperengine::RenderSystem::getTerrainDefineBits(environment, inputs, surface, false, true) & limitBit) != 0;
            EXPECT_EQ(limited, layers > cap) << layers << " layers, surface " << surface;
        }
    }
    inputs.limitLayers = false;
    inputs.layerCount = 9;
    EXPECT_EQ(jasperengine::RenderSystem::getTerrainDefineBits(environment, inputs, true, false, true) & limitBit, 0u) << "off is off";
}

// A terrain with no layer draws its base material alone at every tier and with every feature: no define,
// and the narrow path (the tiers and features act on layers, and llvmpipe dies in those variants without one)
TEST(TerrainQuality, NoLayersDrawTheBaseAlone) {
    TerrainVariantInputs inputs;
    inputs.limitLayers = true;
    inputs.macro = true;
    inputs.resample = true;
    inputs.heightBlend = true;
    inputs.macroNormal = true;
    for (TerrainQuality tier : {TerrainQuality::HIGH, TerrainQuality::MEDIUM, TerrainQuality::LOW}) {
        jasperengine::MeshShaderEnvironment environment;
        environment.terrainForced = true;
        environment.terrainForcedTier = tier;
        inputs.layerCount = 0;
        EXPECT_EQ(jasperengine::RenderSystem::getTerrainDefineBits(environment, inputs, true, false, true), 0u) << terrainQualityName(tier);
        environment.terrainDebugView = static_cast<int>(TerrainDebugView::HOLES);
        EXPECT_EQ(jasperengine::RenderSystem::getTerrainDefineBits(environment, inputs, true, false, true), 0u) << terrainQualityName(tier) << ", a Terrain view";
        environment.terrainDebugView = 0;
        inputs.layerCount = 1;
        EXPECT_NE(jasperengine::RenderSystem::getTerrainDefineBits(environment, inputs, true, false, true), 0u) << terrainQualityName(tier) << ", one layer";
    }
    jasperengine::TerrainComponent terrain;
    terrain.macroMode = jasperengine::TerrainMacroMode::NOISE;
    EXPECT_FALSE(jasperengine::hasTerrainSurfaceLayers(terrain)) << "no layer, Macro Variation on";
    terrain.surfaceLayers.resize(1);
    EXPECT_TRUE(jasperengine::hasTerrainSurfaceLayers(terrain)) << "a layer and Macro Variation";
}

TEST(TerrainQuality, Resolution) {
    TerrainQualityRequest request;
    request.project = TerrainQuality::MEDIUM;
    EXPECT_EQ(resolveTerrainQuality(request).quality, TerrainQuality::MEDIUM);
    EXPECT_EQ(resolveTerrainQuality(request).source, TerrainQualitySource::PROJECT);

    request.hasComponent = true;
    request.component = TerrainQuality::LOW;
    EXPECT_EQ(resolveTerrainQuality(request).quality, TerrainQuality::LOW);
    EXPECT_EQ(resolveTerrainQuality(request).source, TerrainQualitySource::COMPONENT);

    request.hasOverride = true;
    request.globalOverride = TerrainQuality::HIGH;
    EXPECT_EQ(resolveTerrainQuality(request).quality, TerrainQuality::HIGH);
    EXPECT_EQ(resolveTerrainQuality(request).source, TerrainQualitySource::GLOBAL_OVERRIDE);

    request.hasPreview = true;
    request.preview = TerrainQuality::MEDIUM;
    EXPECT_EQ(resolveTerrainQuality(request).quality, TerrainQuality::MEDIUM);
    EXPECT_EQ(resolveTerrainQuality(request).source, TerrainQualitySource::EDITOR_PREVIEW);
}

TEST(TerrainQuality, ClampsToShipped) {
    const uint32_t lowOnly = terrainQualityBit(TerrainQuality::LOW);
    const uint32_t highAndLow = terrainQualityBit(TerrainQuality::HIGH) | terrainQualityBit(TerrainQuality::LOW);
    EXPECT_EQ(clampTerrainQuality(TerrainQuality::HIGH, lowOnly), TerrainQuality::LOW);
    EXPECT_EQ(clampTerrainQuality(TerrainQuality::MEDIUM, highAndLow), TerrainQuality::HIGH) << "higher first";
    EXPECT_EQ(clampTerrainQuality(TerrainQuality::LOW, highAndLow), TerrainQuality::LOW);

    TerrainQualityRequest request;
    request.project = TerrainQuality::HIGH;
    request.shippedMask = lowOnly;
    const TerrainQualityResolved resolved = resolveTerrainQuality(request);
    EXPECT_EQ(resolved.quality, TerrainQuality::LOW);
    EXPECT_TRUE(resolved.clamped);

    TerrainQualitySettings::reset();
    EXPECT_TRUE(TerrainQualitySettings::takeClampWarning()) << "one warning";
    EXPECT_FALSE(TerrainQualitySettings::takeClampWarning());
    TerrainQualitySettings::reset();
}

TEST(TerrainQuality, SettingsBumpTheRevisionOnChangeOnly) {
    TerrainQualitySettings::reset();
    const uint64_t start = TerrainQualitySettings::getRevision();
    TerrainQualitySettings::setPlatformTier(3, TerrainQuality::LOW);
    EXPECT_EQ(TerrainQualitySettings::getRevision(), start + 1);
    TerrainQualitySettings::setPlatformTier(3, TerrainQuality::LOW);
    EXPECT_EQ(TerrainQualitySettings::getRevision(), start + 1);
    EXPECT_EQ(TerrainQualitySettings::getPlatformTier(3), TerrainQuality::LOW);
    EXPECT_EQ(TerrainQualitySettings::getPlatformTier(0), TerrainQuality::HIGH) << "absent means High";
    TerrainQualitySettings::setGlobalOverride(TerrainQuality::MEDIUM);
    TerrainQuality value = TerrainQuality::HIGH;
    EXPECT_TRUE(TerrainQualitySettings::getGlobalOverride(value));
    EXPECT_EQ(value, TerrainQuality::MEDIUM);
    TerrainQualitySettings::clearGlobalOverride();
    EXPECT_FALSE(TerrainQualitySettings::getGlobalOverride(value));
    TerrainQualitySettings::reset();
}

TEST(TerrainLayerSelect, KeepsTheStrongestAndConservesTheSum) {
    const float weights[6] = {0.1f, 0.3f, 0.05f, 0.25f, 0.2f, 0.1f};
    const LayerSelection selection = selectLayers(weights, 6, 2);
    ASSERT_EQ(selection.count, 2);
    EXPECT_EQ(selection.layer[0], 1);
    EXPECT_EQ(selection.layer[1], 3);
    const float sum = selection.weight[0] + selection.weight[1];
    EXPECT_NEAR(sum, 1.0f, 1e-5f);
    EXPECT_NEAR(selection.baseWeight, 0.0f, 1e-5f);
    // the third, 0.2, was subtracted before the scale
    EXPECT_NEAR(selection.weight[0] / selection.weight[1], 0.1f / 0.05f, 1e-4f);

    // fewer layers than the cap: as read, with the base's share
    const float few[2] = {0.25f, 0.5f};
    const LayerSelection all = selectLayers(few, 2, 3);
    ASSERT_EQ(all.count, 2);
    EXPECT_FLOAT_EQ(all.weight[0], 0.5f);
    EXPECT_FLOAT_EQ(all.weight[1], 0.25f);
    EXPECT_NEAR(all.baseWeight, 0.25f, 1e-6f);

    // a tie of more than the cap uses the kept weights as read
    const float tie[4] = {0.25f, 0.25f, 0.25f, 0.25f};
    const LayerSelection tied = selectLayers(tie, 4, 2);
    ASSERT_EQ(tied.count, 2);
    EXPECT_EQ(tied.layer[0], 0);
    EXPECT_EQ(tied.layer[1], 1);
    EXPECT_FLOAT_EQ(tied.weight[0], 0.25f);
}

// AC3: across each swap the kept shares change by at most twice the input step, and the sum and the
// base weight are conserved, so nothing pops when a layer enters or leaves the cap
TEST(TerrainLayerSelect, Continuity) {
    std::mt19937 random(17);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    constexpr float STEP = 1e-3f;
    constexpr int LAYERS = 9;
    for (int trial = 0; trial < 10000; trial++) {
        const int keep = trial % 2 == 0 ? 2 : 3;
        float weights[LAYERS];
        float sum = 0.0f;
        for (float& weight : weights) {
            weight = unit(random);
            sum += weight;
        }
        const float scale = unit(random) / sum;
        for (float& weight : weights) weight *= scale;
        // the two layers around the cap meet at a common value
        const LayerSelection before = selectLayers(weights, LAYERS, keep);
        ASSERT_EQ(before.count, keep);
        std::vector<int> order(LAYERS);
        for (int i = 0; i < LAYERS; i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return weights[a] > weights[b]; });
        const int in = order[keep - 1];
        const int out = order[keep];
        const float mid = 0.5f * (weights[in] + weights[out]);
        float a[LAYERS];
        float b[LAYERS];
        std::copy(weights, weights + LAYERS, a);
        std::copy(weights, weights + LAYERS, b);
        a[in] = mid + STEP * 0.5f;
        a[out] = mid - STEP * 0.5f;
        b[in] = mid - STEP * 0.5f;
        b[out] = mid + STEP * 0.5f;
        const LayerSelection sa = selectLayers(a, LAYERS, keep);
        const LayerSelection sb = selectLayers(b, LAYERS, keep);

        float shareA[LAYERS] = {};
        float shareB[LAYERS] = {};
        float sumA = 0.0f;
        float sumB = 0.0f;
        for (int i = 0; i < sa.count; i++) {
            shareA[sa.layer[i]] = sa.weight[i];
            sumA += sa.weight[i];
        }
        for (int i = 0; i < sb.count; i++) {
            shareB[sb.layer[i]] = sb.weight[i];
            sumB += sb.weight[i];
        }
        // the scale of the rule is (sum of all) / (sum of the kept above the next): the bound is two input
        // steps times it, for a pair of layers near that swap
        const float capped = sumA > 0.0f ? [&]() {
            float value = 0.0f;
            float sorted[LAYERS];
            std::copy(a, a + LAYERS, sorted);
            std::sort(sorted, sorted + LAYERS, std::greater<float>());
            for (int i = 0; i < keep; i++) value += sorted[i] - sorted[keep];
            return value;
        }() : 1.0f;
        const float gain = capped > 1e-6f ? sumA / capped : 1.0f;
        for (int layer = 0; layer < LAYERS; layer++) {
            EXPECT_LE(std::fabs(shareA[layer] - shareB[layer]), 2.0f * STEP * gain + 1e-4f) << "trial " << trial << " layer " << layer;
        }
        float all = 0.0f;
        for (float weight : a) all += weight;
        EXPECT_NEAR(sumA, all, 1e-4f);
        EXPECT_NEAR(sa.baseWeight, std::max(0.0f, 1.0f - all), 1e-5f);
    }
}

TEST(TerrainLayers, ChannelMapping) {
    for (int layer = 0; layer < TERRAIN_LAYERS_IN_RGB; layer++) {
        const LayerChannel channel = terrainLayerChannel(layer);
        EXPECT_EQ(channel.map, layer / 3);
        EXPECT_EQ(channel.channel, layer % 3);
        EXPECT_FALSE(channel.inverted);
    }
    for (int m = 0; m < 3; m++) {
        const LayerChannel channel = terrainLayerChannel(9 + m);
        EXPECT_EQ(channel.map, m);
        EXPECT_EQ(channel.channel, 3);
        EXPECT_TRUE(channel.inverted);
    }
    EXPECT_EQ(terrainBlendMapCount(3), 1);
    EXPECT_EQ(terrainBlendMapCount(4), 2);
    EXPECT_EQ(terrainBlendMapCount(6), 2);
    EXPECT_EQ(terrainBlendMapCount(9), 3);
    EXPECT_EQ(terrainBlendMapCount(10), 3) << "a layer in an alpha reads all three maps";
}

namespace {

    TerrainMaterialInputs pbrLayers(int count) {
        TerrainMaterialInputs inputs;
        inputs.layerCount = count;
        for (int i = 0; i < count; i++) {
            inputs.layers[i].pbr = true;
            inputs.layers[i].normalMap = true;
            inputs.layers[i].surfaceMap = true;
        }
        return inputs;
    }

}

// R8's table
TEST(TerrainMaterialCost, Budgets) {
    TerrainMaterialInputs nine = pbrLayers(9);
    EXPECT_EQ(materialCost(nine, TerrainQuality::HIGH).flat, 9);
    EXPECT_EQ(materialCost(nine, TerrainQuality::HIGH).worst, 111);
    EXPECT_EQ(materialCost(nine, TerrainQuality::MEDIUM).flat, 9);
    EXPECT_EQ(materialCost(nine, TerrainQuality::MEDIUM).slope, 15);
    EXPECT_EQ(materialCost(nine, TerrainQuality::MEDIUM).worst, 21);
    EXPECT_EQ(materialCost(nine, TerrainQuality::LOW).worst, 6);
    nine.limitLayersPerPixel = true;
    EXPECT_EQ(materialCost(nine, TerrainQuality::HIGH).worst, 51);
    EXPECT_EQ(materialCost(nine, TerrainQuality::HIGH).flat, 9);
    nine.limitLayersPerPixel = false;
    nine.holes = true;
    EXPECT_EQ(materialCost(nine, TerrainQuality::MEDIUM).worst, 22);
    EXPECT_EQ(materialCost(nine, TerrainQuality::LOW).worst, 7);
    nine.macroNormal = true;
    EXPECT_EQ(materialCost(nine, TerrainQuality::MEDIUM).worst, 23);
    EXPECT_EQ(materialCost(nine, TerrainQuality::LOW).worst, 7) << "not at Low";

    // hex and stochastic stay High's: 138 and 246
    TerrainMaterialInputs hexes = pbrLayers(9);
    for (TerrainLayerCostInput& layer : hexes.layers) layer.hex = true;
    EXPECT_EQ(materialCost(hexes, TerrainQuality::HIGH).worst, 138);
    EXPECT_EQ(materialCost(hexes, TerrainQuality::MEDIUM).worst, 21) << "drawn plain";
    TerrainMaterialInputs stochastic = pbrLayers(9);
    for (TerrainLayerCostInput& layer : stochastic.layers) layer.stochastic = true;
    EXPECT_EQ(materialCost(stochastic, TerrainQuality::HIGH).worst, 246);

    // trimming: a terrain of up to 6 layers has 6 and 5, one of up to 3 layers has 5 and 4
    TerrainMaterialInputs six = pbrLayers(6);
    EXPECT_EQ(materialCost(six, TerrainQuality::LOW).worst, 5);
    six.holes = true;
    EXPECT_EQ(materialCost(six, TerrainQuality::LOW).worst, 6);
    TerrainMaterialInputs three = pbrLayers(3);
    EXPECT_EQ(materialCost(three, TerrainQuality::LOW).worst, 4);
    three.holes = true;
    EXPECT_EQ(materialCost(three, TerrainQuality::LOW).worst, 5);
    EXPECT_EQ(materialCost(three, TerrainQuality::HIGH).blendMapsRead, 3) << "High keeps three";

    // Flat Only never reads a side plane
    TerrainMaterialInputs flat = pbrLayers(2);
    for (TerrainLayerCostInput& layer : flat.layers) layer.flatOnly = true;
    EXPECT_LT(materialCost(flat, TerrainQuality::HIGH).worst, materialCost(pbrLayers(2), TerrainQuality::HIGH).worst);
}

// D4's table
TEST(TerrainMaterialCost, Slots) {
    TerrainMaterialInputs high = pbrLayers(9);
    high.holes = true;
    EXPECT_EQ(materialCost(high, TerrainQuality::HIGH).textureSlots, 16) << "post colour, holes";
    high.postColor = false;
    high.lightingSlots = 7;
    EXPECT_EQ(materialCost(high, TerrainQuality::HIGH).textureSlots, 16) << "no post colour, holes";

    TerrainMaterialInputs medium = pbrLayers(6);
    medium.holes = true;
    EXPECT_EQ(materialCost(medium, TerrainQuality::MEDIUM).textureSlots, 15);
    TerrainMaterialInputs low = pbrLayers(3);
    low.holes = true;
    EXPECT_EQ(materialCost(low, TerrainQuality::LOW).textureSlots, 14);
}

// AC18: within 1/127 of the height's central difference, on a slope
TEST(TerrainMacroNormal, MatchesCentralDifferences) {
    constexpr int SIZE = 65;
    std::vector<uint8_t> pixels(static_cast<size_t>(SIZE) * SIZE * 2);
    const auto heightOf = [](float u, float v) {
        return 0.5f + 0.25f * std::sin(u * 6.2831853f) * std::cos(v * 6.2831853f);
    };
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            const float h = heightOf((static_cast<float>(x) + 0.5f) / SIZE, (static_cast<float>(y) + 0.5f) / SIZE);
            const unsigned q = static_cast<unsigned>(std::lround(h * 65535.0f));
            pixels[(static_cast<size_t>(y) * SIZE + x) * 2] = static_cast<uint8_t>(q & 0xFF);
            pixels[(static_cast<size_t>(y) * SIZE + x) * 2 + 1] = static_cast<uint8_t>(q >> 8);
        }
    }
    MacroNormalSource source;
    source.pixels = pixels.data();
    source.width = SIZE;
    source.height = SIZE;
    source.bytesPerChannel = 2;
    source.terrainSize = 128.0f;
    source.maxHeight = 20.0f;
    const std::vector<MacroNormalLevel> levels = bakeMacroNormal(source);
    ASSERT_FALSE(levels.empty());
    EXPECT_EQ(levels[0].width, SIZE);
    const float step = source.terrainSize / SIZE;
    for (int y = 1; y < SIZE - 1; y++) {
        for (int x = 1; x < SIZE - 1; x++) {
            const float u0 = (static_cast<float>(x) - 0.5f) / SIZE;
            const float u1 = (static_cast<float>(x) + 1.5f) / SIZE;
            const float v0 = (static_cast<float>(y) - 0.5f) / SIZE;
            const float v1 = (static_cast<float>(y) + 1.5f) / SIZE;
            const float u = (static_cast<float>(x) + 0.5f) / SIZE;
            const float v = (static_cast<float>(y) + 0.5f) / SIZE;
            const float dx = (heightOf(u1, v) - heightOf(u0, v)) * source.maxHeight / (2.0f * step);
            const float dz = (heightOf(u, v1) - heightOf(u, v0)) * source.maxHeight / (2.0f * step);
            const float length = std::sqrt(dx * dx + dz * dz + 1.0f);
            const size_t at = (static_cast<size_t>(y) * SIZE + x) * 2;
            const float bakedX = static_cast<float>(levels[0].rg[at]) / 255.0f * 2.0f - 1.0f;
            const float bakedZ = static_cast<float>(levels[0].rg[at + 1]) / 255.0f * 2.0f - 1.0f;
            EXPECT_NEAR(bakedX, -dx / length, 1.0f / 127.0f) << x << ", " << y;
            EXPECT_NEAR(bakedZ, -dz / length, 1.0f / 127.0f) << x << ", " << y;
        }
    }
    // mips to one texel, each normal renormalized: flat in the mean
    EXPECT_EQ(levels.back().width, 1);
    EXPECT_NEAR(static_cast<float>(levels.back().rg[0]) / 255.0f * 2.0f - 1.0f, 0.0f, 0.05f);
    EXPECT_EQ(levels.size(), 7u);

    // capped at 2048 a side
    EXPECT_EQ(macroNormalSize(4097), TERRAIN_MACRO_NORMAL_MAX_SIZE);
}
