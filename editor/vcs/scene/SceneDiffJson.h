// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A scene diff as JSON (diff_scene, `jasperengine-editor scene-diff --json`) and as text (the
// CLI, the Console)

#include "vcs/scene/SceneDiff.h"

#include "mcp/McpJson.h"

#include <string>

namespace jasperengine::editor::vcs::scene {

    // {added, removed, modified, objects: [{id, name, state, reparented?, base_parent?, parent?,
    // reordered?, settings?, prefab_member?, instance_root?, components: [{key, state,
    // properties: [{path, base?, current?}]}]}]}; the id of Scene Settings is "settings"
    mcp::Json sceneDiffJson(const SceneDiffResult& diff);

    // One line per change: "~ Player (1001)", "    transform.position: [0, 0, 0] -> [1, 2, 3]"
    std::string sceneDiffText(const SceneDiffResult& diff);

}
