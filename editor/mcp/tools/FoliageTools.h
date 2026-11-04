// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What T1's tools call for foliage (T5: no tool of its own): the terrain an MCP call means, the
// bake_impostors op, the overlays and sample_terrain's foliage key.

#include "mcp/McpCall.h"
#include "mcp/McpJson.h"
#include "terrain/TerrainOps.h"

#include <optional>
#include <vector>

namespace jasperengine::editor {
    class Project;
    struct SceneProject;
}

namespace jasperengine::editor::mcp {

    struct TerrainTarget {
        Project* project = nullptr;
        SceneProject* scene = nullptr;
        Entity entity = NULL_ENTITY;
        Scene* engineScene() const;
    };

    // The scene and entity args: the only Terrain, or the selected one
    bool resolveTerrainTarget(const CallPtr& call, TerrainTarget& target);
    // bake_impostors: a job that spans frames (T5 R56)
    void bakeImpostorsFromOps(const CallPtr& call, const TerrainTarget& target, const std::vector<terrain::Op>& ops, bool dryRun);
    // visualize_terrain's density and tree_density (T5 R57)
    void visualizeFoliage(const CallPtr& call, const TerrainTarget& target);
    // sample_terrain {foliage: true} (T5 R55), over a local region when there is one
    struct FoliageRegionXZ {
        float minX, minZ, maxX, maxZ;
    };
    Json foliageStatsJson(Project* project, SceneProject* scene, Entity entity, const std::optional<FoliageRegionXZ>& region);

}
