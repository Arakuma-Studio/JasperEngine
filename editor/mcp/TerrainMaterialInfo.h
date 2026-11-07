// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T3 R43: what sample_terrain says of a terrain's material and holes, and of one point, kept out of
// WorldTools.cpp, which holds only the calls.

#include "McpTypes.h"
#include "Scene.h"
#include "component/TerrainComponent.h"
#include "terrain/TerrainMaterialCost.h"
#include "terrain/TerrainQuality.h"

namespace jasperengine::editor::mcp {

    // The inputs of the cost formula of a terrain, as the readouts and sample_terrain read it
    terrain::TerrainMaterialInputs terrainMaterialInputs(const TerrainComponent& terrain, bool postColor);
    // The tier a terrain draws at, and what decided it (R2)
    terrain::TerrainQualityResolved terrainEffectiveTier(const TerrainComponent& terrain);

    // info.material
    Json terrainMaterialJson(Scene* scene, Entity entity);
    // info.holes: null without a map
    Json terrainHolesJson(TerrainComponent& terrain);
    // a point's hole, collision_gap, nav_excluded, layers_painted, layers_drawn and fetches
    void addTerrainPointMaterial(Scene* scene, Entity entity, float localX, float localZ, Json& sample);
    // stats.coverage of a local rectangle: its hole fraction and layer-count histogram
    Json terrainCoverageJson(TerrainComponent& terrain, float minX, float minZ, float maxX, float maxZ);

}
