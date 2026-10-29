// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"

#include <cstdint>

namespace jasperengine::editor{

    // The Scene view's mouse and keys for splines (S18 R25, R26), called by SceneWindow: the
    // Draw Spline tool (a stroke, or a knot per click with a dragged Bezier handle), and the
    // Edit Spline context (knot picks, Shift/Ctrl add or toggle, Ctrl+click inserts on the curve
    // or extends past an end, Bezier handle drags, the marquee, Esc). The gizmo drag of the
    // selected knots is SceneRender's (SplineSceneRender.cpp).
    class SplineSceneTools{
    public:
        enum class Press{
            None,       // not the spline tools': the view handles it
            Selected,   // a knot was picked: the release selects nothing, the gizmo may drag it
            Captured    // the spline tools own the press until its release
        };

        // x, y: engine canvas coordinates of the cursor
        static Press mousePress(Project* project, SceneProject* sceneProject, float x, float y, bool gizmoHandleHovered);
        static void mouseDrag(Project* project, SceneProject* sceneProject, float x, float y);
        static void mouseRelease(Project* project, SceneProject* sceneProject, float x, float y);
        // a marquee over the view (NDC corners): true when it selected knots
        static bool marquee(Project* project, SceneProject* sceneProject, const Vector2& start, const Vector2& end, bool add);
        // a double click finishes a click-mode drawing
        static bool doubleClick(Project* project, SceneProject* sceneProject, float x, float y);
        // a press was captured and its release is still to come
        static bool isCapturing(uint32_t sceneId);

        // Esc (cancel the drawing, clear the knots, leave Edit Spline) and Enter (finish the
        // drawing), while the view has the keyboard: true when one was taken
        static bool handleKeys(Project* project, SceneProject* sceneProject);
        // Delete/Backspace (edit.delete): the drawing's last knot, the selected knots; false
        // when they are not the spline tools'
        static bool deleteRequested(Project* project);
        // F: the selected knots' bounds (or the spline's) to frame; false when not editing knots
        static bool frameBounds(Project* project, uint32_t sceneId, AABB& bounds);
    };

}
