// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ecs/Entity.h"
#include "mcp/McpCall.h"

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

}
