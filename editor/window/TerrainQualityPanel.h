// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T3 R39, R40, R42: the Quality widget's shared parts: the cost strip the Inspector, the Terrain window
// and Project Settings show, its warnings, and the Scene view's Terrain dropdown.

#include "component/TerrainComponent.h"
#include "terrain/TerrainMaterialCost.h"
#include "terrain/TerrainQuality.h"
#include "texture/Material.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    // The numbers of one tier: `Fetches per pixel: Flat 9 · Slope 15 · Worst 21 · Textures 16/16`
    std::string terrainCostStripText(const terraincore::MaterialCost& cost);
    // Above the tier's budget (R8) or the slots (R32); reason names what costs most
    bool terrainCostOverBudget(const terraincore::MaterialCost& cost, terraincore::TerrainQuality tier, const terraincore::TerrainMaterialInputs& inputs, std::string& reason);

    // The strip for a tier, red above its budget with the reason in a tooltip. shown is -1 for the terrain's
    // effective tier, with a selector that shows another tier's numbers without changing anything.
    void drawTerrainCostStrip(const TerrainComponent& terrain, int& shown);

    // R40: what a feature took from the base material, and a custom shader that draws over holes
    std::vector<std::string> terrainQualityWarnings(const TerrainComponent& terrain, const Material& material);
    // why a tier does not draw a feature, empty when it does
    std::string terrainFeatureGreyReason(const char* feature, terraincore::TerrainQuality tier);

    // R39: a layer's painted coverage in percent, from its blend map's texels
    float terrainLayerCoverage(TerrainComponent& terrain, int layer);

    // R42: the Scene view's Terrain dropdown (Preview Quality, View), with the badge of an active preview.
    // Ends at Play, and is never saved.
    void drawTerrainPreviewDropdown(const std::string& popupId);
    void clearTerrainPreview();

}
