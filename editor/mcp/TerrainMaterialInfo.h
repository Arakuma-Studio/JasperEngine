// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T3 R43: what sample_terrain says of a terrain's material and holes, and of one point, kept out of
// WorldTools.cpp, which holds only the calls.

#include "mcp/McpJson.h"
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
    // a point's hole, and with full its collision_gap, nav_excluded, layers_painted, layers_drawn and fetches
    void addTerrainPointMaterial(Scene* scene, Entity entity, float localX, float localZ, bool full, Json& sample);
    // the most points a sample_terrain call reads the full material of: a dense grid of heights stays
    // under the result's size cap and its time
    constexpr size_t MAX_MATERIAL_POINTS = 64;
    // stats.coverage of a local rectangle: its hole fraction and layer-count histogram
    Json terrainCoverageJson(TerrainComponent& terrain, float minX, float minZ, float maxX, float maxZ);

}
