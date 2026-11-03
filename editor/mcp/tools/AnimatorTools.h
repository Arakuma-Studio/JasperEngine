// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The MCP side of S21's Animator assets that other tool modules use (docs/specs/S21-animation.md
// D17 and "Upstream absorption"): upstream's get_animation {mask[, root]} and {controller}
// reads, the layer / mask coverage parts of get_animation {entity} for Animation Players, the
// write_file checks of .mask and .controller text, and get_animator's controller summary
// (validation, users, the dialect read). AnimatorTools.cpp registers the animation group's
// tools. Main thread.

#include "mcp/McpCall.h"
#include "mcp/McpJson.h"

#include "ecs/Entity.h"

#include <string>
#include <vector>

namespace jasperengine {
    struct ClipWarning;
}

namespace jasperengine::anim {
    struct ControllerDef;
}

namespace jasperengine::editor {
    class AnimatorAssetDocument;
    class Project;
    struct SceneProject;
}

namespace jasperengine::editor::mcp {

    class McpRegistry;
    void registerAnimatorTools(McpRegistry& registry);

}

namespace jasperengine::editor::mcp::animator_tools {

    // get_animation {mask[, root]}: the mask (default, body_parts, root_hierarchy, paths,
    // preview_model), its warnings and users; with root the weight of every entity the root's
    // Animation Player or Animator binds (or every descendant), with its rule
    void getMask(const CallPtr& call);
    // get_animation {controller}: get_animator {controller}'s summary
    void getController(const CallPtr& call);

    // get_animation {entity}'s Animation Player additions: `layers` (index, name, weight, blend,
    // mask, states; play mode: live weight, mask, blend and queue), `mask_coverage`, and
    // `animator` when the player hands over to the GameObject's Animator
    void addPlayerLayers(Project* project, SceneProject* scene, Entity root, Json& player);

    // write_file of a .mask / .controller: false + error when the text can't be read (not JSON,
    // another format, a newer version, Unity YAML); `warnings` the lenient reader's ("path:
    // message"), a note for a version 1 file (converted when the editor saves it), and for a
    // controller its validation
    bool checkMaskText(const std::string& text, std::vector<std::string>& warnings, std::string& error);
    bool checkControllerText(Project* project, const std::string& text, std::vector<std::string>& warnings, std::string& error);

    // a controller's summary: path, read_as (a converted version 1 file), parameters, layers,
    // diagnostics (the compiler's), warnings (the validation), load_warnings, users (Animators)
    Json controllerSummary(Project* project, const AnimatorAssetDocument& document);
    // the validation of a controller (validateController with the clips through their
    // documents) and the users (Animators, controllers) of an Animator asset, as JSON
    std::vector<ClipWarning> controllerWarnings(Project* project, const anim::ControllerDef& def);
    Json controllerUsers(Project* project, const std::string& controllerPath);
    std::string controllerSummaryText(const AnimatorAssetDocument& document, const Json& summary);

}
