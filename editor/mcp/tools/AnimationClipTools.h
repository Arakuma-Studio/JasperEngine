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

namespace jasperengine {
    struct ClipAudioCue;
}
namespace jasperengine::editor {
    class Project;
    struct SceneProject;
}
namespace jasperengine::anim {
    struct ClipRig;
}

namespace jasperengine::editor::mcp::clip_tools {

    // edit_animation with `clip` (file mode): settings, tracks by path, events, create {from}
    void editClip(const CallPtr& call);
    // get_animation with `clip`: the file (or a model sub-clip), `times` sampled, `root` bindings
    void getClip(const CallPtr& call);
    // get_animation {root, list_properties: "<path>"}: what a property track can animate on that
    // GameObject [{component, property, kind, label, group, value, discrete?}] (anim-properties)
    void listProperties(const CallPtr& call);

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
    // warnings, users, stats}; `text` gets the summary appended. False (with data.error) when it
    // is not a readable clip.
    bool clipAssetInfo(Project* project, const std::filesystem::path& file, Json& data, std::string& text);

    // get_asset_info of a model: each item of `animations` (by its "name") gains keys and
    // runtime_bytes, of what a player of that clip loads; both null when the clip cannot be built
    void addModelClipStats(const std::filesystem::path& model, Json& animations);

    // A GameObject's hierarchy as a rig for check_animation and optimize_animation: its rest is what
    // the Transforms hold now. Here, beside the other Transform readers, so the clip quality tools
    // do not include Transform.h (tools/check_fanout.py)
    bool rigFromEntity(SceneProject* sceneProject, Entity root, anim::ClipRig& rig, std::string& error);

    // get_asset_info's `avatar` of a model (anim-retarget 10): {animation_type, humanoid, mapped,
    // missing, extra_mapped, twist, human_scale, notes}; null for a model without bones
    Json avatarJson(Project* project, const std::filesystem::path& model);

    // write_file of an .anim: false + error when the text can't be read as a clip (not JSON,
    // another format or version, Unity YAML); `warnings` the lenient reader's ("path: message")
    bool checkClipText(const std::string& text, std::vector<std::string>& warnings, std::string& error);
    // One item of edit_animation's `audio` (S40 R12, R25), validated: a clip's audio cue, or with
    // entityMode an entity Animation's audio or snapshot action (no source, stop or follow_speed;
    // duration and lane allowed, read by the caller). The error names the fix.
    bool readAudioCue(Project* project, const Json& item, const std::string& where, bool entityMode, ClipAudioCue& cue, std::string& error);
    // an asset-relative path as the tools show it (project-relative)
    std::string toolPath(Project* project, const std::string& assetRelative);

}
