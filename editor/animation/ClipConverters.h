// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 4.5, 9.R3/R4/R15)
// - Purpose: the conversions into clips, shared by the UI (Inspector buttons, Animation window,
//   Pose window Bake menu) and MCP (edit_animation {clip, create: {from}}, edit_pose_animation
//   bake.clip). Pure functions that read a scene and produce clip data or a plan; the scene
//   changes are made by the caller through one CreateClipCmd (command/type/CreateClipCmd.h), so
//   every action that writes a file and changes a scene is one scene undo step (R4).
// - Owner: T0 (API); T2d implements it (and PoseBaker's bake-to-clip on top of bakeChannels).
// - Thread-safety: main thread (reads scenes).
// - Invariants:
//   * convertEntityAnimation keeps keyframe tracks only (times shifted by the frame's startTime,
//     targets as paths relative to `root`: the actual entities, R3), maps loop/duration to
//     wrap/length; tweens, sprite animations, particles, nested animations and missing actions
//     are listed in `notConverted`. Sampling the clip on `root` gives the entity animation's
//     values; where the format can't say the same (before a late frame: the scene value in a
//     looping Animation's first cycle, a looping track's cycle; a cubic track's first key) a
//     warning says so, and a looping track that can't be offset exactly is not converted.
//   * extractModelSubClip copies the sub-clip with `source` {kind "model", path, name} set and
//     a new GUID; the model is not touched.
//   * nothing here writes files or the scene.

#pragma once

#include "animation/AnimationClip.h"
#include "component/AnimationPlayerComponent.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine{
    class Scene;
}

namespace jasperengine::editor{

    class Project;

    struct ClipConvertReport{
        size_t convertedTracks = 0;
        std::vector<Entity> convertedTrackEntities;     // the track entities Convert may remove
        std::vector<std::string> notConverted;          // "Tween 'Fade' (alpha tween)", ...
        std::vector<ClipWarning> warnings;
        // legacy-layout model entities standing in for the glTF scene root that tracks animate:
        // their keys were taken off the root's matrix (the reload moves it to the root node)
        std::vector<Entity> legacyRootModels;
    };

    // What "Use Animation Player" on a model does in its one scene step (R15)
    struct UseAnimationPlayerPlan{
        Entity model = NULL_ENTITY;
        std::vector<Entity> removeEntities;             // the model's entity animations and their tracks
        std::vector<AnimationPlayerClip> clips;         // every "<model>#<name>"
        bool addPlayer = true;                          // false: the model has one, `clips` are appended
        bool reloadModel = false;                       // legacy layout: reload in the node hierarchy (R3)
    };

    // One channel of baked frames (PoseBaker, scripts): the entity it was sampled on and the keys
    struct BakedChannel{
        Entity target = NULL_ENTITY;
        ClipChannel channel = ClipChannel::Rotation;
        std::vector<float> times;
        std::vector<std::vector<float>> values;         // ClipKey::value layout per key
        EaseType ease = EaseType::LINEAR;               // every segment (step when stepped)
    };

    class ClipConverters{
    public:
        // Entity Animation -> clip data. `root` NULL_ENTITY = the Animation's target.
        static bool convertEntityAnimation(Scene* scene, Entity animation, Entity root, ClipData& out,
                                           ClipConvertReport& report, std::string* error);

        // Model sub-clip ("<model>#<clip>") -> editable clip data
        static bool extractModelSubClip(const std::string& reference, ClipData& out,
                                        std::vector<ClipWarning>& warnings, std::string* error);

        static bool planUseAnimationPlayer(Scene* scene, Entity model, UseAnimationPlayerPlan& out, std::string* error);

        // Baked frames -> clip data with paths relative to `root` (the model root); `length` < 0:
        // the last key; `loop` gives wrap loop
        static bool bakeChannels(Scene* scene, Entity root, const std::vector<BakedChannel>& channels, float length, bool loop,
                                 ClipData& out, std::string* error);

        // The root Convert to Clip uses when none is chosen: the Animation's target, else the lowest
        // common Transform ancestor of its tracks' targets (a top-level Animation has no target)
        static Entity defaultConvertRoot(Scene* scene, Entity animation);

        // The path a clip binds `entity` by when `root` plays it (R3: from the actual entities).
        // Parts of a model in the legacy layout get their node-hierarchy path (the layout an
        // Animation Player reloads such a model in: anim::modelNodePaths). False when `entity` is
        // not `root` or under it, or a name on the way can't be addressed.
        static bool clipPath(Scene* scene, Entity root, Entity entity, std::string& out);

        // The Animation Player entry of a clip: `clip` a .anim path (absolute, project- or
        // asset-relative) or "<model>#<clip>"; .anim entries carry the file's GUID (when known)
        static AnimationPlayerClip playerEntry(Project* project, const std::string& clip);
        // the clip entry form of a file: asset-relative, '/' separated
        static std::string assetRelative(Project* project, const std::filesystem::path& path);

        // A free .anim path in `directory` (project-relative) for `stem` ("Walk.anim", "Walk 2.anim"...)
        static std::string uniqueClipPath(Project* project, const std::string& directory, const std::string& stem);
        // where Create New Clip saves for `root`: the folder of its model or prefab, else assets/
        static std::string defaultClipDirectory(Project* project, uint32_t sceneId, Entity root);
    };

}
