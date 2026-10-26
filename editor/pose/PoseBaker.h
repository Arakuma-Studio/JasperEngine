// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Baking a pose animation to a standard Animation of a model GameObject (spec: pose animation
// phase 2, section 4.5): the frames at frame_rate on the model's rig become one RotateTracks per
// mapped slot bone and one TranslateTracks for the hips (keys at the frames' times, step easing
// when step_rate > 0, else linear), in the Animation named `name` (default: the file stem) of
// the model's ModelComponent.animations, so Model:playAnimation(name) plays it. Baking again
// rewrites the keys of the same track entities in place. Everything is one scene undo step.
//
// The keys sit at the frames' own times (f * length / count, count = round(length * fps)), so
// the last key is exactly at the length: the loop closes without a jump.
//
// Bake to Animation Clip (animation clips spec 4.5, 9.R3/R4) writes the same keys to a shared
// .anim instead: the mapped bones' paths relative to the model root, computed from the actual
// entities (ClipConverters::clipPath), listed in the model's Animation Player (added when it
// has none). File and scene change are one scene undo step (CreateClipCmd).

#pragma once

#include "ecs/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;
    class PoseAnimationDocument;

    struct PoseBakeResult{
        Entity animation = NULL_ENTITY;
        std::string name;           // the Animation's name (unique in the scene)
        bool created = false;       // false: an existing Animation was re-baked
        int frames = 0;             // keys per track
        int tracks = 0;
        float length = 0.0f;
    };

    struct PoseClipBakeResult{
        std::string clip;           // the .anim written (project-relative)
        std::string name;           // the player state that plays it
        bool created = false;       // false: a clip baked from this animation before was rewritten
        bool addedPlayer = false;
        size_t entry = 0;           // its Animation Player entry
        int frames = 0;             // keys per track
        int tracks = 0;
        float length = 0.0f;
        size_t reloadedModels = 0;  // the model reloaded in its node hierarchy (R3)
    };

    class PoseBaker{
    public:
        // `model`: a model GameObject of scene `sceneId` (edit mode). False + error, with
        // nothing changed, when the model is no usable humanoid or the animation has no poses.
        static bool bake(Project* project, uint32_t sceneId, Entity model, PoseAnimationDocument& document,
                         const std::string& name, PoseBakeResult& result, std::string& error);
        // What the Pose Animator Inspector section's Bake button does: bakes the animation the
        // model's Pose Animator plays (`animation` as the component stores it, relative to the
        // assets folder) under the default name
        static bool bakeAnimatorAsset(Project* project, uint32_t sceneId, Entity model, const std::string& animation,
                                      PoseBakeResult& result, std::string& error);
        // the same to an Animation Clip (bakeToClip with the default name and path)
        static bool bakeAnimatorAssetToClip(Project* project, uint32_t sceneId, Entity model, const std::string& animation,
                                            PoseClipBakeResult& result, std::string& error);
        // Bake to Animation Clip: `clipPath` project-relative .anim; empty: "<name>.anim" next to
        // the pose animation (`name` empty: its file stem). An existing file there is rewritten
        // (its GUID kept) only when it was baked from this pose animation; otherwise a free name
        // is taken ("walk 2.anim"), and an explicit `clipPath` that exists is refused.
        static bool bakeToClip(Project* project, uint32_t sceneId, Entity model, PoseAnimationDocument& document,
                               const std::string& name, const std::string& clipPath, PoseClipBakeResult& result, std::string& error);
        // "Baked 'anim/walk.poseanim' to the clip 'assets/anim/walk.anim' (23 tracks, 31 keys each), ..."
        static std::string clipBakeStatusLine(const std::string& animation, const PoseClipBakeResult& result);

        // "Baked 'anim/walk.poseanim' to the Animation 'walk' (23 tracks, 31 keys each): ..."
        static std::string bakeStatusLine(const std::string& animation, const PoseBakeResult& result);
    };

}
