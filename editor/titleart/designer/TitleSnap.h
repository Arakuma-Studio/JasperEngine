// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R32)
// - Purpose: snapping of the Title Designer's canvas drags. A moving box (a move) or point (a
//   scale corner, a handle, the pivot) snaps within 8 SCREEN px (the radius divided by the zoom)
//   to the canvas edges and centre, the other layers' bounds and centres, the guides and, when it
//   is shown, the pixel grid. The result is the corrected delta, the lines it matched (drawn while
//   dragging) and, for a box, the gaps to its nearest neighbours ("24 px"). Holding Ctrl (Cmd)
//   replaces snapping by Unity's increments (10 px, 15°, 0.1 scale; editable in the Snap menu):
//   the gizmos call the increment functions instead of snapBox / snapPoint.
// - Coordinates: document space (canvas px, origin at the centre, +y up); `SnapContext::zoom` is
//   the canvas zoom (screen px per canvas px).
// - Design: pure functions over a context the canvas fills once per gesture (the other layers'
//   bounds exclude the moving ones); drawing is separate (drawSnap).
// - Thread-safety: values and pure functions; drawSnap on the main thread inside an ImGui frame.

#pragma once

#include "titleart/designer/TitleDesignerTypes.h"

#include <string>
#include <vector>

namespace jasperengine::editor::titledesigner {

    // the keys held during a canvas drag (Ctrl is Cmd on macOS: ImGui's io.KeyCtrl / KeySuper,
    // the canvas maps them)
    struct GizmoModifiers {
        bool shift = false;
        bool ctrl = false;
        bool alt = false;
    };

    // the Snap menu (view state)
    struct SnapSettings {
        bool enabled = true;            // Snap toggle
        float radiusPx = 8.0f;          // screen px
        bool pixelGrid = false;         // snap to whole canvas px (on while the pixel grid shows)
        double moveIncrement = 10.0;    // Ctrl: canvas px
        double rotateIncrement = 15.0;  // Ctrl: degrees
        double scaleIncrement = 0.1;    // Ctrl: scale steps
    };

    // what a drag may snap to
    struct SnapContext {
        SnapSettings settings;
        float zoom = 1.0f;
        TitleRect canvas;                   // canvasRect(width, height)
        std::vector<TitleRect> others;      // the other visible layers' bounds (document space)
        std::vector<double> guidesX;        // vertical guides (their x)
        std::vector<double> guidesY;        // horizontal guides (their y)

        // the snap radius in canvas px
        double threshold() const { return zoom > 0.0f ? settings.radiusPx / zoom : 0.0; }
    };

    // a matched line, drawn while dragging: vertical at x = `at` from y = `from` to `to`, or
    // horizontal at y = `at` from x = `from` to `to`
    struct SnapLine {
        bool vertical = true;
        double at = 0.0;
        double from = 0.0, to = 0.0;
    };

    // the gap between the moving box and a neighbour, labelled "24 px"
    struct SnapGap {
        TitleVec2 from, to;
        double length = 0.0;
    };

    struct SnapResult {
        TitleVec2 delta;                // the corrected delta
        bool snappedX = false, snappedY = false;
        std::vector<SnapLine> lines;
        std::vector<SnapGap> gaps;
    };

    // the canvas of a document as a rectangle of document space
    TitleRect canvasRect(int width, int height);

    // `box` (at the press) moved by `delta`: its left, centre and right (bottom, centre, top) snap
    // to the nearest target within the radius, per axis
    SnapResult snapBox(const SnapContext& context, const TitleRect& box, TitleVec2 delta);
    // `point` (at the press) moved by `delta`
    SnapResult snapPoint(const SnapContext& context, TitleVec2 point, TitleVec2 delta);

    // Ctrl's increments
    double snapToIncrement(double value, double step);
    TitleVec2 incrementMove(TitleVec2 delta, const SnapSettings& settings);
    double incrementAngle(double degrees, const SnapSettings& settings);
    double incrementScale(double scale, const SnapSettings& settings);

    // "24 px"
    std::string formatGap(double length);

    // the matched lines (magenta, across the canvas view) and the gap labels
    void drawSnap(ImDrawList* drawList, const CanvasXform& view, const SnapResult& result);

}
