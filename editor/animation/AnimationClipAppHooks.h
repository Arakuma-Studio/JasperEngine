// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 4.1, 4.4, 9.R4-R7)
// - Purpose: the App's wiring points for the clip editor, so App.cpp and ViewTools.cpp (shared
//   wiring files) need no further edit when the clip tasks land: the clip mode's part of
//   App::stopTransientPreviews (the entity mode is AnimationWindow::stopTransientPreviews), the
//   undo/redo routing to the project history, the keys the window takes from the main toolbar,
//   scene and project close, the main menu's Assets > Create > Animation Clip and clip drag-drop
//   onto scene objects. (The MCP window test hook of focus_window {window: "animation"} went with
//   the editor's other test-only MCP hooks: the window is tested through real X input, the
//   public MCP tools and unit tests of the clip mode.)
// - Owner: T0 (struct + every App/ViewTools call site). Setters: T3a stopTransientPreviews /
//   ownsUndo / takesKeys / sceneClosed / projectClosed (the AnimationClipController), T2b
//   createClipAsset, T2d dropClips (T2b's Scene view and Hierarchy drop targets call
//   App::dispatchClipDrop).
// - Thread-safety: main thread; set once at startup.
// - Invariants: every hook is optional (unset = the behaviour before clips: the menu item is
//   hidden); hooks never open modal popups
//   synchronously from the call sites (they run inside the App frame).

#pragma once

#include "ecs/Entity.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    struct AnimationClipAppHooks{
        // before a scene save, play, scene switch/close: the clip mode's previews restore their
        // snapshots (R6)
        std::function<void()> stopTransientPreviews;
        // the newest undoable step across the scene and project histories is a clip commit made
        // through the editor UI, whatever window has focus (R5b/R5c): S24's timeline undoes it
        // next, and the Animation window keeps its keys. The App asks clipUndoGoesToProject
        // (below), not this directly
        std::function<bool()> ownsUndo;
        // the Animation window is focused and handles its keys (Space, K, R, comma/period,
        // Delete, Ctrl+D...): the main toolbar's single-key shortcuts and the scene key block stay
        // off. When undo goes to the project history with another window focused (a clip commit
        // is the newest step, R5b), the scene keys stay on except undo/redo.
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
        // once per editor frame (App::engineRender, `frame` = ImGui::GetFrameCount()): the clip
        // controller's update(dt, frame), so previews play and deferred stops finish whether
        // or not the Animation window is open (set by AnimationClipController)
        std::function<void(Project* project, double dt, int frame)> frame;

        // ---- anim layers (Animator Controllers, spec 11.4); set by the Animator window ----
        // opens Window > Animation > Animator on a .controller (project-relative): with
        // `entity` != NULL_ENTITY of `sceneId`, bound to that GameObject's Animation Player (live
        // view in play mode, previews on it). Called by the Inspector's Controller field "Open",
        // the Project window's double-click on a .controller and MCP. Unset: no Animator window.
        std::function<void(Project* project, const std::string& controller, uint32_t sceneId, Entity entity)> openAnimator;
        // Assets > Create > Animator Controller / Avatar Mask (main menu and the Project window's
        // Create menu): "New Animator Controller.controller" (a Base Layer, no states) / "New Avatar
        // Mask.mask" (every part 1, default 1) in the Project window's current folder, one project
        // undo step. Set by the Inspector task; unset: the items are hidden.
        std::function<void(Project* project)> createControllerAsset;
        std::function<void(Project* project)> createMaskAsset;
    };

    // the editor-wide hooks (set once at startup by the tasks that own them)
    inline AnimationClipAppHooks& animationClipAppHooks(){
        static AnimationClipAppHooks hooks;
        return hooks;
    }

    // The App's clip key focus (LastFocusedWindow::AnimationClip): ownsUndo (R5b), except while a
    // Physics Placement gesture runs or settles in the selected scene (`placementBusy`): then
    // the scene keeps the keys and the timeline's undo cancels the gesture first, its rule since
    // before clips (PhysicsPlacementWindow::beforeHistoryChange; final review M3)
    inline bool clipUndoGoesToProject(bool placementBusy){
        const AnimationClipAppHooks& hooks = animationClipAppHooks();
        return !placementBusy && hooks.ownsUndo && hooks.ownsUndo();
    }

}
