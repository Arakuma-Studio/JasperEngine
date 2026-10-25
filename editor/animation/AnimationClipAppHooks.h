// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 4.1, 4.4, 9.R4-R7)
// - Purpose: the App's wiring points for the clip editor, so App.cpp and ViewTools.cpp (shared
//   wiring files) need no further edit when the clip tasks land: the clip mode's part of
//   App::stopTransientPreviews (the entity mode is AnimationWindow::stopTransientPreviews), the
//   undo/redo routing to the project history, the keys the window takes from the main toolbar,
//   scene and project close, the main menu's Assets > Create > Animation Clip, clip drag-drop onto
//   scene objects, and the MCP window test hook of focus_window {window: "animation",
//   animation: {...}}.
// - Owner: T0 (struct + every App/ViewTools call site). Setters: T3a stopTransientPreviews /
//   ownsUndo / takesKeys / sceneClosed / projectClosed (the AnimationClipController), T2b
//   createClipAsset, T2d dropClips (T2b's Scene view and Hierarchy drop targets call
//   App::dispatchClipDrop), T3b windowTestHook.
// - Thread-safety: main thread; set once at startup.
// - Invariants: every hook is optional (unset = the behaviour before clips: the menu item is
//   hidden, the test hook fails with "not available"); hooks never open modal popups
//   synchronously from the call sites (they run inside the App frame).

#pragma once

#include "ecs/Entity.h"
#include "json.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    // which editor windows have keyboard focus when undo/redo is routed
    struct ClipUndoFocus{
        bool sceneView = false;
        bool inspector = false;
        bool hierarchy = false;
        bool animationWindow = false;
    };

    struct AnimationClipAppHooks{
        // before a scene save, play, scene switch/close: the clip mode's previews restore their
        // snapshots (R6)
        std::function<void()> stopTransientPreviews;
        // Ctrl/Cmd+Z (and Edit > Undo) go to the project history: the newest undoable step across
        // the scene and project histories is a clip commit made through the editor UI, whatever
        // window has focus (R5b/R5c: AnimationClipController's history timeline)
        std::function<bool(const ClipUndoFocus& focus)> ownsUndo;
        // (R5b, T3a round 2) the same for Ctrl/Cmd+Y / Shift+Z and Edit > Redo, decided on its own:
        // the newest redoable step (the one undone last) across the scene and project histories
        // is a clip commit. Unset: redo follows ownsUndo
        std::function<bool(const ClipUndoFocus& focus)> ownsRedo;
        // the Animation window is focused and handles its keys (Space, K, R, comma/period,
        // Delete, Ctrl+D...): the main toolbar's single-key shortcuts and the scene key block stay
        // off. While only Record routes undo (the Scene view focused), the scene keys stay on
        // except undo/redo.
        std::function<bool()> takesKeys;
        // a scene is closing (its previews' snapshots are dropped, owners released)
        std::function<void(uint32_t sceneId)> sceneClosed;
        // the project closes (switch) or the editor shuts down, before any scene is destroyed:
        // previews restored, documents and the shared controller released
        // (AnimationClipController::release)
        std::function<void()> projectClosed;
        // Assets > Create > Animation Clip (main menu; the Project window's own Create menu calls
        // the same): creates "New Animation.anim" in the Project window's current folder as one
        // project undo step. Unset: the menu item is hidden.
        std::function<void(Project* project)> createClipAsset;
        // clip files ("resource_files" payload, absolute paths, .anim or model files whose
        // sub-clips are meant) dropped onto `target` of `sceneId`: adds or reuses an Animation
        // Player and appends the clips as one scene step (CreateClipCmd). True when handled.
        std::function<bool(Project* project, uint32_t sceneId, Entity target, const std::vector<std::string>& paths)> dropClips;
        // MCP focus_window {window: "animation", animation: {...}} (JASPER_MCP_TEST_HOOKS=1 only):
        // drives the window's clip mode for tests; fills `out` (returned as the result's
        // `viewport_input`), false + `error` on bad input
        std::function<bool(Project* project, const nlohmann::json& hook, nlohmann::json& out, std::string& error)> windowTestHook;
        // once per editor frame (App::engineRender, `frame` = ImGui::GetFrameCount()): the clip
        // controller's update(dt, frame), so previews play and deferred stops finish whether
        // or not the Animation window is open (set by AnimationClipController)
        std::function<void(Project* project, double dt, int frame)> frame;
    };

    // the editor-wide hooks (set once at startup by the tasks that own them)
    inline AnimationClipAppHooks& animationClipAppHooks(){
        static AnimationClipAppHooks hooks;
        return hooks;
    }

}
