// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 4.2, 9.R4/R15/R30)
// - Purpose: the Inspector's clip actions, implemented by the tasks that own them, so that
//   InspectorWindow includes no window and no converter (PoseInspectorHooks style). Each button
//   shows only once its hook is set; unset (the default) the Inspector hides it.
// - Owner: T0 (struct); T2c calls them from the Inspector (Animation Player section, .anim asset
//   section, AnimationComponent and Model sections); T3a/T3b set the window hooks (open*),
//   T2d the converter hooks (convert/extract/useAnimationPlayer/addToPlayer/createNewClip).
// - Thread-safety: main thread; set once at startup.
// - Invariants: hooks run in edit mode only (the Inspector disables the buttons while playing);
//   each reports its own errors (alert or status line); scene-changing ones are one CreateClipCmd
//   scene step, pure clip edits a project step.

#pragma once

#include "ecs/Entity.h"

#include <cstdint>
#include <functional>
#include <string>

namespace jasperengine::editor{

    class Project;

    struct AnimationClipInspectorHooks{
        // Open a clip in the Animation window, bound to `root` (NULL_ENTITY: target-less, the
        // clip's preview model). `clip` = .anim path or "<model>#<clip>" as a component stores it.
        std::function<void(Project* project, uint32_t sceneId, Entity root, const std::string& clip)> openClip;
        // "Open in Animation window" of an entity Animation (AnimationComponent section)
        std::function<void(Project* project, uint32_t sceneId, Entity animation)> openEntityAnimation;
        // "Create New Clip..." of the Animation Player section: save dialog, file + clip entry
        std::function<void(Project* project, uint32_t sceneId, Entity root)> createNewClip;
        // "Convert to Clip..." of an entity Animation (dialog: root, remove converted tracks)
        std::function<void(Project* project, uint32_t sceneId, Entity animation)> convertToClip;
        // "Extract to .anim" of a model sub-clip (Model animations table, read-only clip rows,
        // the .anim asset section of a sub-clip); `model` NULL_ENTITY when not from a scene
        std::function<void(Project* project, uint32_t sceneId, Entity model, const std::string& subClip)> extractSubClip;
        // "Use Animation Player" of the Model section (R15)
        std::function<void(Project* project, uint32_t sceneId, Entity model)> useAnimationPlayer;
        // "Add to Animation Player" of a Model animations row
        std::function<void(Project* project, uint32_t sceneId, Entity model, const std::string& subClip)> addToAnimationPlayer;
    };

    // the editor-wide hooks (set once at startup by the tasks that own them)
    inline AnimationClipInspectorHooks& animationClipInspectorHooks(){
        static AnimationClipInspectorHooks hooks;
        return hooks;
    }

}
