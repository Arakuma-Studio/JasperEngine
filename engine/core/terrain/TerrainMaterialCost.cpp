// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/TerrainMaterialCost.h"

#include <algorithm>
#include <functional>
#include <vector>

using namespace jasperengine::terraincore;

namespace {

    // the reads of one map of a layer at the tier's worst pixel
    int readsPerMap(const TerrainLayerCostInput& layer, const TerrainTierInfo& tier, bool slopeCounts) {
        int reads = tier.readsPerMap;
        if (tier.hexAndStochastic) {
            // three taps per projection: S11's numbers, flat 3 + two sides x 3, or flat 3 + two sides
            if (layer.stochastic) reads = slopeCounts ? 9 : 3;
            else if (layer.hex) reads = slopeCounts ? 5 : 3;
        }
        if (layer.flatOnly && slopeCounts && !layer.hex && !layer.stochastic) {
            // never a side plane: near and far of the flat projection, at most
            reads = std::min(reads, tier.readsPerMap >= 2 ? 2 : 1);
        }
        return reads;
    }

    int mapsOf(const TerrainLayerCostInput& layer, const TerrainTierInfo& tier) {
        if (!layer.pbr) return 1;
        return 1 + (layer.normalMap ? 1 : 0) + (layer.surfaceMap && tier.surfaceMaps ? 1 : 0);
    }

    int layerCost(const TerrainLayerCostInput& layer, const TerrainTierInfo& tier, bool slopeCounts) {
        return mapsOf(layer, tier) * readsPerMap(layer, tier, slopeCounts);
    }

}

MaterialCost jasperengine::terraincore::materialCost(const TerrainMaterialInputs& inputs, TerrainQuality quality) {
    const TerrainTierInfo& tier = terrainTierInfo(quality);
    MaterialCost cost;
    const int layerCount = std::min(inputs.layerCount, TERRAIN_MAX_LAYERS);

    int keep = inputs.limitLayersPerPixel ? tier.limitedLayersPerPixel : tier.layersPerPixel;
    if (keep <= 0 || keep > layerCount) keep = layerCount;
    cost.layersPerPixel = keep;

    cost.blendMapsRead = tier.trimBlendMaps ? terrainBlendMapCount(layerCount) : 3;
    const int extras = (inputs.holes ? 1 : 0) + (inputs.macroNormal && tier.macroNormal ? 1 : 0);

    // the most expensive layers are the ones a pixel may keep
    std::vector<int> flatCosts;
    std::vector<int> slopeCosts;
    for (int i = 0; i < layerCount; i++) {
        flatCosts.push_back(mapsOf(inputs.layers[i], tier));
        slopeCosts.push_back(layerCost(inputs.layers[i], tier, true));
    }
    std::sort(flatCosts.begin(), flatCosts.end(), std::greater<int>());
    std::sort(slopeCosts.begin(), slopeCosts.end(), std::greater<int>());

    const auto twoLayers = [&](const std::vector<int>& costs) {
        int sum = 0;
        for (int i = 0; i < std::min(keep, 2) && i < static_cast<int>(costs.size()); i++) sum += costs[i];
        return sum;
    };
    const auto keptLayers = [&](const std::vector<int>& costs) {
        int sum = 0;
        for (int i = 0; i < keep && i < static_cast<int>(costs.size()); i++) sum += costs[i];
        return sum;
    };

    if (quality == TerrainQuality::LOW) {
        // colour for both, the normal for the strongest only, and one plane: one read each
        const int kept = std::min(keep, layerCount);
        bool anyNormal = false;
        for (int i = 0; i < layerCount; i++) anyNormal = anyNormal || (inputs.layers[i].pbr && inputs.layers[i].normalMap);
        const int reads = kept + (anyNormal && kept > 0 ? 1 : 0);
        cost.flat = cost.blendMapsRead + reads + extras;
        cost.slope = cost.flat;
        cost.worst = cost.flat;
    } else {
        cost.flat = cost.blendMapsRead + twoLayers(flatCosts) + extras;
        cost.slope = cost.blendMapsRead + twoLayers(slopeCosts) + extras;
        cost.worst = cost.blendMapsRead + keptLayers(slopeCosts) + extras;
    }

    // R32, D4: the base material, what its slots gave up, the lighting and the terrain's own
    int base = inputs.baseMaterialSlots;
    if (inputs.holes) base--;                       // the occlusion map (R21)
    if (inputs.postColor) base--;                   // the emissive map (mesh.frag)
    cost.textureSlots = base + inputs.lightingSlots + cost.blendMapsRead + 1 + (inputs.holes ? 1 : 0);
    return cost;
}
