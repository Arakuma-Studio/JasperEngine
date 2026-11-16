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
#include "math/Vector4.h"

#include "imgui.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
        // a pen's pressure 0..1, or -1 for a mouse (Backend::penPressure; V1 R35)
        float pressure = -1.0f;
        // this frame's mouse wheel (ImGuiIO::MouseWheel); the view still zooms with it
        float wheel = 0.0f;
    };

    // line segments in world space the view draws in its scene, depth tested: a cursor on a
    // surface (V1 R103), as the terrain and voxel brush decals are drawn
    struct SceneToolDecal {
        std::vector<Vector3> points;    // two per segment
        std::vector<Vector4> colors;    // one per segment, sRGB with alpha
        uint64_t revision = 0;          // changes when the lines do
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
        // an Alt+left click that did not become an orbit (released before the drag threshold)
        virtual bool altClick(const SceneToolEvent&) { return false; }
        // a right-button drag with Ctrl or Shift held, in view pixels; true: the tool takes it and the
        // camera does not look around (brush size and strength gestures)
        virtual bool rightDrag(const SceneToolEvent&, float, float) { return false; }
        virtual void drawOverlay(SceneRender&, ImDrawList&) {}
        // null: none
        virtual const SceneToolDecal* decal() const { return nullptr; }
        // the badge's exit button
        virtual const char* exitTooltip() const { return "Exit the tool"; }
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
        // a SceneToolEvent's canvas pixels in screen coordinates (a drag's box, drawn in the overlay)
        static ImVec2 canvasToScreen(uint32_t sceneId, float x, float y);
        // the Scene the tool of `sceneId` works on (SceneRender asks by Scene: gizmos, hover redraw)
        static void setScene(uint32_t sceneId, const Scene* scene);
        static bool activeOn(const Scene* scene);
        // the active tool of the view showing `scene` (null: none)
        static SceneTool* activeFor(const Scene* scene);
    };

}
