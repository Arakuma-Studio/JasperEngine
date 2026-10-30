// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md R22-R26)
// - Purpose: the Animator's editor entry points implemented by the windows that own them, so the
//   Inspector and the Project window include no window (PoseInspectorHooks style). Each button or
//   panel shows only once its hook is set.
// - Thread-safety: main thread; set once at startup (the AnimatorWindow constructor).

#pragma once

#include "ecs/Entity.h"

#include <cstdint>
#include <functional>
#include <string>

namespace jasperengine::editor{

    class Project;

    struct AnimatorEditorHooks{
        // opens a .controller in the Animator window, bound to an Animator (NULL_ENTITY: the
        // asset alone); the Project window's double-click and the Inspector's Open button
        std::function<void(Project* project, uint32_t sceneId, Entity entity, const std::string& controller)> openController;
        // Configure Avatar...: the humanoid bone mapping of a model GameObject (writes .humanoid)
        std::function<void(Project* project, uint32_t sceneId, Entity model)> configureAvatar;
        // the Animator window's selection (a state, transition, blend tree, layer) drawn in the
        // Inspector instead of the scene selection; true when it drew
        std::function<bool()> drawSelection;
    };

    inline AnimatorEditorHooks& animatorEditorHooks(){
        static AnimatorEditorHooks hooks;
        return hooks;
    }

}
