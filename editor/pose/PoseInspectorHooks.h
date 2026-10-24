// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Pose Animator Inspector section's Bake and Open buttons (spec: pose animation phase 2,
// section 4.1). The runtime task does not own the baker (phase 2, PoseBaker) nor the Pose
// Animation window (phase 4): they set these hooks, and each button shows only once its hook is
// set. Unset (the default), the section has neither button.

#ifndef POSE_INSPECTOR_HOOKS_H
#define POSE_INSPECTOR_HOOKS_H

#include "ecs/Entity.h"

#include <cstdint>
#include <functional>
#include <string>

namespace jasperengine::editor{

    class Project;

    struct PoseInspectorHooks{
        // Bake `animation` into an Animation clip of the model entity (one scene undo step).
        // Called in edit mode only; reports its own errors.
        std::function<void(Project* project, uint32_t sceneId, Entity model, const std::string& animation)> bake;
        // Open `animation` in the Pose Animation window, bound to the model entity
        std::function<void(Project* project, uint32_t sceneId, Entity model, const std::string& animation)> open;
        // A scene command is about to read and write `entity`'s Transform (PropertyCmd,
        // ObjectTransformCmd: Inspector, gizmo, MCP set_transform / set_properties): a posed bone
        // goes back to its rest first, so the command records and edits the rest pose, never the
        // posed values (final review)
        std::function<void(uint32_t sceneId, Entity entity)> beforeTransformWrite;
    };

    // the editor-wide hooks (set once at startup by the tasks that own them)
    inline PoseInspectorHooks& poseInspectorHooks(){
        static PoseInspectorHooks hooks;
        return hooks;
    }

}

#endif // POSE_INSPECTOR_HOOKS_H
