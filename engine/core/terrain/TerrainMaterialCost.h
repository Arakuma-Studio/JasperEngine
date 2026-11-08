// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_MATERIAL_COST_H
#define TERRAIN_MATERIAL_COST_H

// T3 R8, R32, R39: the terrain material's cost, as the readouts show it: reads of its own maps per
// fragment and the texture slots of the variant. It is the shader's own count written as a formula,
// and TerrainMaterialsGpu's readback holds the two together (AC7). Pure.

#include "Export.h"
#include "terrain/TerrainLayers.h"
#include "terrain/TerrainQuality.h"

namespace jasperengine::terrain {

    struct TerrainLayerCostInput {
        bool pbr = false;
        bool normalMap = false;
        bool surfaceMap = false;
        bool flatOnly = false;      // Cliff Projection: Flat Only
        bool hex = false;
        bool stochastic = false;
    };

    struct TerrainMaterialInputs {
        int layerCount = 0;
        TerrainLayerCostInput layers[TERRAIN_MAX_LAYERS];
        bool limitLayersPerPixel = false;
        bool holes = false;
        bool macroNormal = false;       // and the tier draws it
        bool resample = false;
        // the variant's slots: the worst of the scene's lighting unless told otherwise
        bool postColor = true;
        int lightingSlots = 8;
        int baseMaterialSlots = 5;      // base colour, metallic-roughness, occlusion, emissive, normal
    };

    struct MaterialCost {
        int flat = 0;           // a flat fragment with two layers, near, no far share
        int slope = 0;          // the same on a slope, with the tier's most reads per map
        int worst = 0;          // every kept layer at its most
        int textureSlots = 0;   // of 16
        int blendMapsRead = 0;
        int layersPerPixel = 0; // the most drawn at a fragment
    };

    JASPERENGINE_API MaterialCost materialCost(const TerrainMaterialInputs& inputs, TerrainQuality quality);

    // The portable limit per stage (sokol_gfx.h)
    inline constexpr int TERRAIN_TEXTURE_SLOT_LIMIT = 16;

}

#endif // TERRAIN_MATERIAL_COST_H
