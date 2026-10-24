// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 4.1, 4.4, 9.R4-R6)
// - Purpose: the App's wiring points for the clip editor, so App.cpp (a shared wiring file) needs
//   no further edit when the clip tasks land: the Animation window's part of
//   App::stopTransientPreviews, the undo/redo routing to the project history, the keys the
//   window takes from the main toolbar, clip drag-drop onto scene objects, and the MCP window
//   test hook of focus_window {window: "animation"}.
// - Owner: T0 (struct + App/ViewTools call sites); T3a sets stopTransientPreviews / ownsUndo /
//   takesKeys / sceneClosed, T2b/T2d dropClips (T2b's Scene view and Hierarchy drop targets call
//   App::dispatchClipDrop), T3b windowTestHook.
// - Thread-safety: main thread; set once at startup.
// - Invariants: every hook is optional (unset = the behaviour before clips); hooks never open
//   modal popups synchronously from the call sites (they run inside the App frame).

#pragma once

#include "ecs/Entity.h"
#include "json.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    struct AnimationClipAppHooks{
        // before a scene save, play, scene switch/close: every clip/entity preview of the
        // Animation window restores its snapshot (R6)
        std::function<void()> stopTransientPreviews;
        // Ctrl/Cmd+Z/Y (and Edit > Undo/Redo) go to the project history: the window has focus and
        // its last action was a pure clip edit, or Record is on and the Scene view (or Inspector)
        // has focus (R4/R5). The hook may also look at ImGui focus itself.
        std::function<bool(bool sceneViewFocused)> ownsUndo;
        // the window is focused: the main toolbar's single-key shortcuts stay off (the window
        // has Space, K, R, comma/period...)
        std::function<bool()> takesKeys;
        // a scene is closing (its previews' snapshots are dropped, owners released)
        std::function<void(uint32_t sceneId)> sceneClosed;
        // clip files ("resource_files" payload, absolute paths, .anim or model files whose
        // sub-clips are meant) dropped onto `target` of `sceneId`: adds or reuses an Animation
        // Player and appends the clips as one scene step (CreateClipCmd). True when handled.
        std::function<bool(Project* project, uint32_t sceneId, Entity target, const std::vector<std::string>& paths)> dropClips;
        // MCP focus_window {window: "animation", animation: {...}} (JASPER_MCP_TEST_HOOKS=1 only):
        // drives the window's clip mode for tests; fills `out`, false + `error` on bad input
        std::function<bool(Project* project, const nlohmann::json& hook, nlohmann::json& out, std::string& error)> windowTestHook;
    };

    // the editor-wide hooks (set once at startup by the tasks that own them)
    inline AnimationClipAppHooks& animationClipAppHooks(){
        static AnimationClipAppHooks hooks;
        return hooks;
    }

}
