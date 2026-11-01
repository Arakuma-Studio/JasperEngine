// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The MCP side of animation layers, Avatar Masks and Animator Controllers (anim layers spec 10,
// 11.5): the edit_animator tool (AnimatorTools.cpp registers it), get_animation's {mask} and
// {controller} modes and the layer / controller parts of get_animation {entity}, the write_file
// checks of .mask and .controller text, and the edit_animator op logic (applyOps: pure, tested).
// Main thread.

#include "mcp/McpCall.h"
#include "mcp/McpJson.h"

#include "animation/AnimatorController.h"
#include "ecs/Entity.h"

#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
    struct SceneProject;
}

namespace jasperengine::editor::mcp {

    class McpRegistry;
    void registerAnimatorTools(McpRegistry& registry);

}

namespace jasperengine::editor::mcp::animator_tools {

    // get_animation {mask[, root]}: the mask, its warnings and users; with root the weight every
    // entity the root's player binds (or every descendant) gets, with its rule
    void getMask(const CallPtr& call);
    // get_animation {controller}: edit_animator's summary of the file
    void getController(const CallPtr& call);

    // get_animation {entity}'s player additions: `layers` (index, name, weight, blend, mask,
    // states; play mode: live weight and queue), `mask_coverage` and `controller`
    void addPlayerLayers(Project* project, SceneProject* scene, Entity root, Json& player);

    // write_file of a .mask / .controller: false + error when the text can't be read (not JSON,
    // another format or version, Unity YAML); `warnings` the lenient reader's ("path: message"),
    // and for a controller its validation
    bool checkMaskText(const std::string& text, std::vector<std::string>& warnings, std::string& error);
    bool checkControllerText(Project* project, const std::string& text, std::vector<std::string>& warnings, std::string& error);

    // edit_animator's ops on a controller (all or nothing): false + error naming the op
    // ("ops[2].to: ...") at the first refusal, `data` then unchanged. Paths are MCP's
    // (project-relative) and are stored asset-relative with their GUIDs.
    bool applyOps(Project* project, const Json& ops, anim::ControllerData& data, std::string& error);

    // the summary edit_animator returns: {controller, guid, parameters, layers: [{index, name,
    // weight, blend, mask, default_state, states, transitions}], warnings, users}
    Json controllerSummary(Project* project, const std::string& path, const anim::ControllerData& data,
                           const std::vector<ClipWarning>& loadWarnings);

}
