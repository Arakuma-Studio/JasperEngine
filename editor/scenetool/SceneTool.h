// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Scene-view tool framework (docs/specs/S50-grid-tilemap.md D9, R66): an active SceneTool owns
// the view's left button, keys, overlay, cursor and status; camera navigation (Alt, the right and
// middle buttons, Space-pan) keeps its priority and selection and gizmos are suspended. One active
// tool per Scene view. The Scene window's existing special cases (splines, lattice, terrain brushes,
// fluid painting) are untouched: this framework sits in front of them.

#pragma once

#include "math/Ray.h"
#include "math/Vector3.h"

#include "imgui.h"

#include <cstdint>
#include <memory>
#include <string>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor {

    struct SceneProject;
    class SceneRender;

    // a mouse position in the view: canvas pixels (the engine's, toEngineCanvas), the pick ray, and
    // the modifiers and button of this event
    struct SceneToolEvent {
        float x = 0.0f;
        float y = 0.0f;
        Ray ray;
        bool shift = false;
        bool ctrl = false;
        bool alt = false;
        int button = 0;
    };

    enum class SceneToolPress { NotMine, Captured };

    class SceneTool {
    public:
        virtual ~SceneTool() = default;
        virtual const char* id() const = 0;                 // "tilemap.paint"
        virtual const char* label() const = 0;              // "Paint (B)"
        virtual bool available(const SceneProject&) const = 0;
        virtual SceneToolPress press(const SceneToolEvent&) = 0;
        virtual void drag(const SceneToolEvent&) {}
        virtual void release(const SceneToolEvent&) {}
        virtual void hover(const SceneToolEvent&) {}
        virtual bool key(ImGuiKey, const SceneToolEvent&) { return false; }
        virtual void cancel() {}                            // Esc, tool switch, scene change: revert
        virtual void drawOverlay(SceneRender&, ImDrawList&) {}
        virtual ImGuiMouseCursor cursor() const { return ImGuiMouseCursor_Arrow; }
        virtual std::string status() const { return {}; }
    };

    // one active tool per Scene view, keyed by the view's scene id
    class SceneTools {
    public:
        static void activate(uint32_t sceneId, std::unique_ptr<SceneTool> tool);
        static SceneTool* active(uint32_t sceneId);
        static void deactivate(uint32_t sceneId);   // calls cancel()

        // ---- what the Scene window provides so a tool can draw (no D9 member can carry them)
        // the view's image origin in screen coordinates and the engine-pixels-per-view-pixel scale
        static void setCanvas(uint32_t sceneId, ImVec2 canvasMin, ImVec2 engineScale);
        // a world point on the view's image, in screen coordinates; false when behind the camera
        static bool worldToScreen(uint32_t sceneId, SceneRender& render, const Vector3& world, ImVec2& screen);
        // the Scene the tool of `sceneId` works on (SceneRender asks by Scene: gizmos, hover redraw)
        static void setScene(uint32_t sceneId, const Scene* scene);
        static bool activeOn(const Scene* scene);
    };

}
