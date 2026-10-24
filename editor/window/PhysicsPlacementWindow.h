// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"
#include "physics/PlacementSession.h"
#include "math/Ray.h"
#include "math/Vector2.h"

#include "imgui.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // Window > Physics Placement (design spec docs/superpowers/specs/2026-10-23-physics-placement-design.md
    // section 7): the settings panel, and the owner of the interactive placement sessions.
    //
    // Physics (G, MainToolbar::isPhysicsMode) is a modifier on Move/Rotate/Scale. While it is on,
    // every open 3D scene has a PlacementSession, made on first need and dropped when Physics
    // turns off, Play starts, the scene goes away or the project switches. show() is the frame's
    // tick: it steps every session by wall-clock time, even with the panel closed, the way the
    // Terrain window's show() keeps its previews going.
    //
    // The Scene view drives the gestures: SceneRender the gizmo drags (Move, Rotate, Gather/Spread),
    // SceneWindow hold-Space (Drop), Shift-drag (Snap) and Esc. Each gesture remembers the input
    // that began it, so its release ends it even when the mouse or the key goes up elsewhere.
    class PhysicsPlacementWindow{
    public:
        static constexpr const char* WINDOW_NAME = "Physics Placement";

        // What began a scene's running gesture
        enum class GestureInput{
            None,
            Gizmo,     // a Move/Rotate/Scale handle drag
            Space,     // hold Space
            Snap,      // Shift + LMB drag on a selected object
            Panel      // Gather / Distribute buttons
        };

        PhysicsPlacementWindow(Project* project);
        ~PhysicsPlacementWindow();

        void show();
        void open();
        void setOpen(bool open);
        bool isOpen() const;

        // The panel's settings: the project's, saved to workspace.yaml once an edit ends
        const PhysicsPlacementSettings& getSettings() const;
        // Applies live to every session (the Gizmos menu toggles come through here too)
        void setSettings(const PhysicsPlacementSettings& settings);

        // MainToolbar: Physics turned on or off (off finishes every gesture, settles included,
        // and drops the sessions), or the tool changed (Hand and Rect finish every gesture)
        void physicsModeChanged(bool on);
        void toolChanged(bool physicsTool);

        // Save and Play (EditorHost::stopTransientPreviews): every gesture cancelled, the scenes
        // back to their poses from before it
        void cancelGestures();
        // The scene is about to be deleted (closed, reloaded, its stage exited): its session goes
        // while the scene still exists
        void sceneDeleting(uint32_t sceneId);
        void resetForProjectSwitch();

        // Physics shows in this scene's view: the badge, the HUD and the orange gizmos
        // (Physics on, a 3D scene, nothing playing)
        bool isShownInScene(uint32_t sceneId) const;
        // ...and its Scene view input goes to physics: the pointer with Move, Rotate or Scale,
        // no camera preview, no edit layer, no terrain brush
        bool isActiveInScene(uint32_t sceneId) const;
        // The scene (or Prefab Mode stage) a SceneRender draws; NULL_PROJECT_SCENE if none
        uint32_t findSceneId(const Scene* scene) const;

        // Starts a gesture on the scene's selection; false when nothing can move (the HUD says
        // why). gesture must suit input: Gizmo takes Move/Rotate/GatherSpread, Space Drop,
        // Snap Snap.
        bool beginGesture(uint32_t sceneId, PlacementGesture gesture, GestureInput input);
        // The session while a gesture that input began is held (phase Active); null otherwise,
        // also once Esc or Ctrl+Z cancelled it
        PlacementSession* heldGesture(uint32_t sceneId, GestureInput input) const;
        // Releases a gesture that input began (no-op otherwise)
        void endGesture(uint32_t sceneId, GestureInput input);
        // Esc, and Ctrl+Z while a gesture runs or settles: true when one was cancelled
        bool cancelGesture(uint32_t sceneId);
        bool isBusy(uint32_t sceneId) const;

        // What the Scene view draws: false when physics does not show in the scene
        bool collectOverlay(uint32_t sceneId, PlacementOverlay& overlay) const;
        // The HUD (top-left of the canvas) and the floating labels; project maps a world point
        // to screen coordinates (false when behind the camera)
        void drawSceneHud(uint32_t sceneId, ImDrawList* drawList, const ImVec2& canvasMin, const ImVec2& canvasMax,
                          const std::function<bool(const Vector3&, ImVec2&)>& project) const;
        // The orange PHYSICS badge of the Scene view toolbar row
        void drawSceneBadge(uint32_t sceneId) const;

    private:
        struct SceneSession{
            std::unique_ptr<PlacementSession> session;
            uint64_t structureVersion = 0;
            std::vector<Entity> selection;
            GestureInput input = GestureInput::None;
        };

        Project* project;

        bool windowOpen = false;
        bool focusRequested = false;
        bool settingsDirty = false;

        std::map<uint32_t, SceneSession> sessions;
        std::chrono::steady_clock::time_point lastTick;
        bool hasLastTick = false;
        uint32_t lastSelectedSceneId = NULL_PROJECT_SCENE;

        PhysicsPlacementSettings& settings();
        void applySettings();

        SceneProject* sceneFor(uint32_t sceneId) const;
        SceneSession* findSession(uint32_t sceneId);
        const SceneSession* findSession(uint32_t sceneId) const;
        PlacementSession* sessionFor(uint32_t sceneId);
        // Ends a held gesture, then runs its settle to the end at once (bounded by the session's
        // maxSettleSeconds), so what the user did is committed before physics lets go
        void finish(SceneSession& sceneSession);
        void finishAll();
        void dropSessions(bool cancel);

        void tick();

        void drawPanel();
        void drawCacheRow();
        // Bake selection / Bake scene: decompositions requested for roots through PlacementWorld
        // (the shapes and cache keys movers get), what was left out kept for the panel
        void bake(SceneProject* sceneProject, const std::vector<Entity>& roots);
        bool bakeRan = false;
        size_t bakeParts = 0;
        std::vector<std::string> bakeNotes;
    };

}
