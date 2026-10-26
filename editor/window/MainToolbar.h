// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "imgui.h"
#include "Project.h"
#include "window/ScriptStatusBadge.h"

namespace jasperengine::editor {

    // Unity-style main toolbar: a full-width bar between the menu and the dock space
    // with the transform tools (left), Play/Pause/Step and the script status (centre), Layout
    // and the editor scripts' Tools strip (right).
    // It also owns their keyboard shortcuts.
    class MainToolbar {
    public:
        enum class Tool {
            Hand,
            Move,
            Rotate,
            Scale,
            Rect
        };

    private:
        Project* project;
        ScriptStatusBadge scriptBadge;

        // Physics (G): a modifier on Move/Rotate/Scale, not a tool of its own (GizmoSelected
        // keeps the tool). Global like the tool, for every 3D Scene view.
        bool physicsMode = false;

        bool toolSupported(const SceneProject& sceneProject, Tool tool) const;
        bool physicsSupported(const SceneProject& sceneProject) const;
        void applyTool(SceneProject& sceneProject, Tool tool) const;

        void drawTools(float rowHeight);
        void drawPlayControls(float rowHeight);
        void drawLayoutMenu(float rowHeight);
        // The "Tools" strip: toolbar buttons project editor scripts declare (---@toolbar)
        float toolsStripWidth(float rowHeight) const;
        void drawToolsStrip(float rowHeight);

    public:
        MainToolbar(Project* project);

        // Draws the bar; call after the menu bar and before the dock space.
        void show();
        // Play shortcuts, and tool shortcuts when toolShortcuts (no text editor has
        // the keyboard); call once per frame when no popup is open.
        void handleShortcuts(bool toolShortcuts);

        // Play mode commands (also for the menu): Play toggles play mode on/off,
        // Pause toggles pause while in play mode, Step advances one paused frame.
        bool isInPlayMode() const;
        bool isPaused() const;
        bool canTogglePlay() const;
        bool canPause() const;
        bool canStep() const;
        void togglePlay();
        void togglePause();
        void step();

        // Transform tools act on every open Scene view that supports them
        Tool getTool() const;
        bool canUseTool(Tool tool) const;
        void setTool(Tool tool);
        bool isGlobalTransform() const;
        void toggleGlobalTransform();

        // Physics Placement on the transform tools: 3D scenes, not in Play mode. Turning it
        // off finishes any gesture (PhysicsPlacementWindow::physicsModeChanged).
        bool isPhysicsMode() const;
        bool canUsePhysics() const;
        void setPhysicsMode(bool on);
        void togglePhysicsMode();
    };

}
