// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Unity animation asset importer (docs/specs/S94-rendering-anim-gaps.md F35, F36): a Unity
// YAML `.anim`, `.controller` or `.mask` becomes the engine's JSON asset (AnimationClipIO,
// AnimatorAssetIO). Editor-side: Assets > Import Unity Animation..., a drop of one into the
// Project window, MCP edit_animation / edit_animator_controller / edit_avatar_mask create.unity.
// - A controller keeps its parameters, layers (weight, blending, mask), state machines with
//   sub-state machines, states (speed, cycle offset and their parameters, mirror, foot IK, tag),
//   transitions with their conditions (state, entry, any-state and sub-state machine exit ones;
//   Solo mutes the others as Unity does) and 1D, 2D and Direct blend trees. Its motions and masks
//   resolve through the Unity project's GUIDs (UnityProject): each `.anim` or `.mask` is imported
//   beside the controller (an earlier import of the same Unity file there is reused). A motion
//   of a model file (an .fbx clip) and what has no engine equivalent (synced layers, state
//   machine behaviours, parameter-driven mirror) are reported, not guessed.
// - A mask keeps its humanoid body parts (Unity's arm part also weighs the hand) and its
//   transform list (unlisted transforms weigh 0, as in Unity).
// - Every written file is returned with its contents (`files`): the editor writes them as one undo
//   step (importUnityAnimationIntoProject); nothing is written into the Unity project.

#include "UnityAnimImport.h"

#include "json.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::unity {

    struct AssetImportOptions {
        std::filesystem::path file;            // the Unity .anim, .controller or .mask
        std::filesystem::path dest;            // the engine file to write (absolute; the same extension)
        std::filesystem::path engineAssets;    // the engine project's assets folder (references are relative to it)
        std::filesystem::path unityProject;    // empty: the nearest ancestor with Assets/ and ProjectSettings/
        bool mirrorX = true;                   // clips: Unity's left-handed X mirrored (UnityAnimImport.h)
    };

    struct AssetImportResult {
        bool ok = false;
        std::string error;
        // {source, kind, path, ...the asset's report: a clip's (UnityAnimImport.h); a controller's
        //  parameters, layers, states, clips [{unity, path, reused}], masks, unmapped [{what,
        //  reason}], notes; a mask's body_parts, paths, notes}
        nlohmann::json report;
        // every file to write: the asset first, then the clips and masks it references
        std::vector<std::pair<std::filesystem::path, std::string>> files;
    };

    // .anim, .controller or .mask (by extension) holding Unity YAML
    bool isUnityAnimationAsset(const std::filesystem::path& file);

    // a Unity .anim as a clip (its `source` the Unity project's path), for callers that write it
    // themselves (MCP edit_animation)
    AnimImportResult importUnityClip(const std::filesystem::path& file, bool mirrorX);

    AssetImportResult importUnityAnimationAsset(const AssetImportOptions& options);

    // into `project` (options.engineAssets is set to it): every file written as one undo step of
    // the project history ("Import Unity Animation <name>"). result.ok false + result.error on failure.
    AssetImportResult importUnityAnimationIntoProject(jasperengine::editor::Project* project, AssetImportOptions options);

    // `folder`/<stem of `file`><extension>, numbered (" 2", " 3"...) past existing files
    std::filesystem::path freeImportPath(const std::filesystem::path& folder, const std::filesystem::path& file);

}
