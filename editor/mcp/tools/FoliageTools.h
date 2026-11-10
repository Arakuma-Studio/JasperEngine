// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What T1's tools call for foliage (T5: no tool of its own): edit_terrain's foliage ops, the
// bake_impostors op and sample_terrain's foliage key.

#include "ecs/Entity.h"
#include "mcp/McpCall.h"
#include "mcp/McpJson.h"
#include "terrain/TerrainOps.h"

#include <optional>
#include <vector>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::terraincore {
    struct OpSpace;
}

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

    // The ops T5 registers: they write trees as well as density, so edit_terrain hands them here
    bool isFoliageOp(const terraincore::Op& op);
    // A list of foliage ops as a job on a snapshot (T5 R23), or bake_impostors alone (R56)
    void editFoliageFromOps(const CallPtr& call, const TerrainTarget& target, std::vector<terraincore::Op> ops, const terraincore::OpSpace& space, bool dryRun);
    // bake_impostors: a job that spans frames (T5 R56)
    void bakeImpostorsFromOps(const CallPtr& call, const TerrainTarget& target, const std::vector<terraincore::Op>& ops, bool dryRun);
    // sample_terrain {foliage: true} (T5 R55), over a local region when there is one
    struct FoliageRegionXZ {
        float minX, minZ, maxX, maxZ;
    };
    Json foliageStatsJson(Project* project, SceneProject* scene, Entity entity, const std::optional<FoliageRegionXZ>& region);

}
