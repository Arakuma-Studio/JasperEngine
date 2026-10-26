// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The clip-file side of the animation tools (animation clips spec 5, 9.R4/R28/R29), called by
// WorldTools.cpp (edit_animation, get_animation, preview_animation), PoseTools.cpp (bake.clip)
// and AssetTools.cpp (get_asset_info, write_file, view_asset of .anim files). Main thread.

#include "mcp/McpCall.h"
#include "mcp/McpJson.h"

#include "ecs/Entity.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
    struct SceneProject;
}

namespace jasperengine::editor::mcp::clip_tools {

    // edit_animation with `clip` (file mode): settings, tracks by path, events, create {from}
    void editClip(const CallPtr& call);
    // get_animation with `clip`: the file (or a model sub-clip), `times` sampled, `root` bindings
    void getClip(const CallPtr& call);

    // The arguments one mode refuses with a fix-it message (two-way table of 9.R29); "" when
    // none. `fileMode`: the call has `clip`.
    std::string rejectedArgument(const Json& args, bool fileMode);

    // get_animation {entity}: the Animation Player that plays the GameObject (its own, else the
    // nearest ancestor's) as [{entity, states: [{name, clip, wrap}], default, bindings}]; empty
    // array when none
    Json playersJson(Project* project, SceneProject* scene, Entity entity);

    // preview_animation on a GameObject with (or under) an Animation Player: true when it took
    // the call (the GameObject plays clips), false to leave it to the entity Animation preview
    bool previewPlayer(const CallPtr& call, SceneProject* scene, Entity entity);

    // get_asset_info of an .anim: {version, guid, length, wrap, frame_rate, tracks, events,
    // warnings, users}; `text` gets the summary appended. False (with data.error) when it is not
    // a readable clip.
    bool clipAssetInfo(Project* project, const std::filesystem::path& file, Json& data, std::string& text);

    // write_file of an .anim: false + error when the text can't be read as a clip (not JSON,
    // another format or version, Unity YAML); `warnings` the lenient reader's ("path: message")
    bool checkClipText(const std::string& text, std::vector<std::string>& warnings, std::string& error);

}
