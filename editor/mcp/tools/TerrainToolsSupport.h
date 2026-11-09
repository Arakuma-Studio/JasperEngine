// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ecs/Entity.h"
#include "mcp/McpCall.h"

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor{
    class Project;
    struct SceneProject;
}

namespace jasperengine::editor::mcp {

    // T1 R70: sample_terrain's region, stats, slope_bins, issues and hash, added to `data`; false (the
    // call failed) on a bad argument
    bool sampleTerrainExtras(const CallPtr& call, Project* project, SceneProject* scene, Entity entity, bool world, Json& data);

    // sample_terrain's grid defaults to 9 only when none of these is asked
    bool sampleTerrainAsksExtras(const Json& args);

    // T4 R19, R46: a point's state (absent, overview, full) on a sample of a Terrain World; nothing for a
    // plain Terrain. localX, localZ: entity-local.
    void addTerrainWorldPointState(Scene* scene, Entity entity, float localX, float localZ, Json& sample);
    // T4 R45: sample_terrain's world key: the world's status added to data, and its wait_for job; completes
    // the call (an error naming edit_terrain_world {create} on a plain Terrain)
    void sampleTerrainWorld(const CallPtr& call, Project* project, SceneProject* scene, Entity entity, Json data);

}
