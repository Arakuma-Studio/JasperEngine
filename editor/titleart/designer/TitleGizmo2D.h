// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R34, D5, D16)
// - Purpose: the 2D gizmo of the Title Designer's canvas (every selected layer whose Dimension is
//   2D): hit tests, drag maths producing FieldEdits, the drag readout, and its ImDrawList drawing.
//   Tools: Rect (T, default: the oriented content box with 8 handles, drawn 8 px and hit 24 px;
//   corners scale `transform.scale` uniformly about the opposite corner with the position
//   compensated, Alt about the pivot; edges write `h_scale` / `v_scale` on Text layers, `aspect`
//   and `size` on Shape layers and a uniform scale on Image and Vector layers; just outside a
//   corner a rotate zone turns about the pivot, Shift in 15° steps), Move (W: X / Y arrows and a
//   centre square; Shift constrains a free move to one axis), Rotate (E: a ring) and Scale (R: a
//   centre box, uniform). A pivot circle sits at the layer's `transform.position` (Pivot mode) or
//   the content box centre (Center mode, Unity's Pivot/Center, Z); dragging it moves only the
//   gizmo's pivot (view state: pivot() after the drag, no edit).
// - Several targets: one gizmo on the union of their boxes (document axes), every target
//   transformed about the shared pivot in one gesture; parents (groups, mirrored ones too) are
//   handled through each target's `parentToDoc`: a document-space similarity G maps a layer with
//   parent P to position P^-1 G P (position), rotation + sign(det P) x the turn, scale x the factor.
// - Gestures are absolute: update() computes every value from the targets as they were at
//   begin(), so the canvas stages the edits each frame and Esc only drops them. Every field a
//   gesture drives is in every update's edits (also when back at its start value).
// - Per frame (lead): setTargets(); when idle hitTest() for the hover and cursor(); on press
//   begin(); while dragging update() and stage its edits; on release end() (commit) or end()
//   and drop (Esc); draw() last, over the art.
// - Field paths: `transform.position` ([x, y]), `transform.rotation`, `transform.scale`,
//   `h_scale`, `v_scale`, `size`, `aspect`.
// - Drawing: every stroke twice, a 7.5 px translucent dark halo then the 3.5 px colour (5 px
//   hovered), so the handles read over any art (the `overlay` helpers, shared with the handles,
//   the snap lines and the 3D gizmo).
// - Thread-safety: main thread (a value type; no ImGui calls outside draw()).

#pragma once

#include "titleart/designer/TitleDesignerTypes.h"
#include "titleart/designer/TitleSnap.h"

#include <string>
#include <vector>

namespace jasperengine::editor::titledesigner {

    using jasperengine::titleart::LayerType;
    using jasperengine::titleart::TitleTransform;

    // Unity's Pivot / Center (Z)
    enum class PivotMode : uint8_t { Pivot, Center };

    // one selected layer as the canvas describes it
    struct Gizmo2DTarget {
        std::string layerId;
        LayerType type = LayerType::Text;
        TitleTransform transform;           // as stored (parent space)
        Affine2 parentToDoc;                // the parents' transforms (identity at the top level)
        TitleRect localBox;                 // the content box in the layer's own space (before its transform)
        double hScale = 1.0, vScale = 1.0;  // Text: h_scale, v_scale
        double size = 200.0, aspect = 1.0;  // Shape: size, aspect
        bool hasPivot = false;              // a pivot the user dragged (view state, document space)
        TitleVec2 pivot;
        double effectReach = 0.0;           // canvas px the effects draw outside the box (Guides > Effect Reach); 0 = none
    };

    enum class Gizmo2DHandle : uint8_t {
        None,
        // the box's own corners and edges (in the layer's space: BL is (x0, y0))
        CornerBL, CornerBR, CornerTR, CornerTL,
        EdgeL, EdgeR, EdgeB, EdgeT,
        // just outside a corner: rotate about the pivot
        RotateBL, RotateBR, RotateTR, RotateTL,
        MoveX, MoveY, MoveXY,       // Move tool
        Ring,                       // Rotate tool
        ScaleBox,                   // Scale tool
        Pivot,
        Inside                      // inside the box (any tool): a free move
    };

    // the cursor shape a handle asks for; Rotate has no ImGui cursor: hide it and draw
    // overlay::rotateCursor at the mouse
    enum class Gizmo2DCursor : uint8_t { Arrow, Move, Rotate, ResizeNS, ResizeEW, ResizeNESW, ResizeNWSE };
    ImGuiMouseCursor imguiCursor(Gizmo2DCursor cursor);

    struct Gizmo2DSettings {
        float handleSize = 8.0f;        // drawn, screen px
        float handleHit = 24.0f;        // hit square, screen px
        float rotateZone = 24.0f;       // how far past a corner's hit square the rotate zone reaches
        float armLength = 80.0f;        // Move arrows
        float ringRadius = 72.0f;       // Rotate ring
        bool cjkLabels = false;         // "Horizontal Scale 92 % (長体)": the CJK editor font is present
    };

    class TitleGizmo2D {
    public:
        // the selection as it is this frame (during a drag: as staged; the gesture keeps its start)
        void setTargets(std::vector<Gizmo2DTarget> targets, Tool tool, PivotMode mode, const CanvasXform& view);
        void setSettings(const Gizmo2DSettings& value) { settings = value; }
        const Gizmo2DSettings& getSettings() const { return settings; }
        bool empty() const { return targets.empty(); }

        Gizmo2DHandle hitTest(ImVec2 screen) const;
        Gizmo2DCursor cursor(Gizmo2DHandle handle) const;

        // false (no gesture) for None or a handle the current tool does not have
        bool begin(Gizmo2DHandle handle, ImVec2 screen, const GizmoModifiers& modifiers);
        // the edits of the gesture so far (absolute, from the state at begin); `snap` null or
        // disabled: no snapping; Ctrl uses its increments (the settings of `snap`, defaults without)
        std::vector<FieldEdit> update(ImVec2 screen, const GizmoModifiers& modifiers, const SnapContext* snap = nullptr);
        void end();
        bool active() const { return drag.handle != Gizmo2DHandle::None; }
        Gizmo2DHandle activeHandle() const { return drag.handle; }

        // during a drag: the gesture and only the values it changed ("Rotate · 12.5°",
        // "Scale · 1.25 · X 40"); "" before the first change
        std::string readout() const;
        // the undo step's name: "Move Layer", "Rotate 3 Layers", "Change Horizontal Scale"
        std::string gestureName() const;
        // the snap of the last update (drawSnap draws it)
        const SnapResult& lastSnap() const { return drag.snap; }

        // the gizmo's pivot (document space): a pivot drag's result once it moved
        // (read both before end(): the canvas stores it as the layer's pivot, view state)
        TitleVec2 pivot() const { return pivotDoc; }
        bool pivotMoved() const { return drag.pivotMoved; }

        // the box corners on screen (BL, BR, TR, TL); false without a box
        bool screenCorners(ImVec2 out[4]) const;

        void draw(ImDrawList* drawList, Gizmo2DHandle hovered, Gizmo2DHandle activeHandle) const;

    private:
        enum class Gesture : uint8_t { None, Move, Rotate, Scale, EdgeText, EdgeShape, Pivot };

        struct Drag {
            Gizmo2DHandle handle = Gizmo2DHandle::None;
            Gesture gesture = Gesture::None;
            std::vector<Gizmo2DTarget> start;
            Affine2 frameToDoc;
            TitleRect frame;
            TitleVec2 pivot;
            TitleVec2 press;            // document
            ImVec2 pressScreen;
            double lastAngle = 0.0;     // rotation: the pointer's angle about the pivot (CCW degrees)
            double turned = 0.0;        // rotation: accumulated, CCW degrees
            // the result so far, for the readout
            bool changed = false;
            TitleVec2 moved;            // document delta (moves)
            double turn = 0.0;          // clockwise degrees
            double factor = 1.0;
            std::string field;          // edge gestures: "h_scale", "v_scale", "aspect", "size"
            double fieldValue = 0.0, fieldStart = 0.0;
            double aspectValue = 0.0, aspectStart = 0.0;
            TitleTransform first;       // the first target's transform now
            bool pivotMoved = false;
            TitleVec2 pivotNow;         // a pivot drag: where the pivot is now
            SnapResult snap;
        };

        void layout();
        bool hasRect() const;
        TitleVec2 framePoint(int index) const;          // corners 0..3 (BL, BR, TR, TL), edge midpoints 4..7 (L, R, B, T)
        ImVec2 screenOf(int index) const;
        bool insideFrame(ImVec2 screen) const;
        std::vector<FieldEdit> applySimilarity(const Affine2& g, double turnCw, double factor, bool rotates, bool scales);
        std::vector<FieldEdit> updateEdge(TitleVec2 mouse, const GizmoModifiers& modifiers, const SnapSettings& increments);

        std::vector<Gizmo2DTarget> targets;
        Tool tool = Tool::Rect;
        PivotMode mode = PivotMode::Pivot;
        CanvasXform view;
        Gizmo2DSettings settings;
        // derived by layout()
        Affine2 frameToDoc;
        TitleRect frame;
        bool hasFrame = false;
        TitleVec2 pivotDoc;
        Drag drag;
    };

    // a nudge (arrow keys): 1 canvas px, Shift 10, `dx` / `dy` in -1..1 (+y up)
    TitleVec2 nudgeDelta(int dx, int dy, bool shift);
    // the position edits that move every target by `docDelta` (document space, converted to each
    // layer's parent space)
    std::vector<FieldEdit> nudgeEdits(const std::vector<Gizmo2DTarget>& targets, TitleVec2 docDelta);

    // a number for a readout or label: at most `decimals` decimals, trailing zeros dropped ("12.5", "40")
    std::string formatReadout(double value, int decimals = 1);

    // ---- the canvas overlay's stroke style (gizmos, handles, snap lines) ----
    namespace overlay {
        constexpr float HALO_WIDTH = 7.5f;
        constexpr float STROKE_WIDTH = 3.5f;
        constexpr float HOVER_WIDTH = 5.0f;
        constexpr float HOVER_HALO_WIDTH = 9.5f;
        constexpr ImU32 HALO_COLOR = IM_COL32(7, 7, 10, 204);
        constexpr ImU32 ACCENT = IM_COL32(74, 163, 255, 255);
        constexpr ImU32 ACCENT_LIT = IM_COL32(143, 198, 255, 255);
        constexpr ImU32 WHITE = IM_COL32(255, 255, 255, 255);
        // X red, Y green, Z blue, and their hovered variants
        inline constexpr ImU32 AXIS[3] = {IM_COL32(255, 90, 76, 255), IM_COL32(78, 227, 95, 255), IM_COL32(74, 157, 255, 255)};
        inline constexpr ImU32 AXIS_LIT[3] = {IM_COL32(255, 148, 137, 255), IM_COL32(141, 250, 153, 255), IM_COL32(143, 198, 255, 255)};

        // a stroke: the halo, then the colour
        void line(ImDrawList* drawList, ImVec2 a, ImVec2 b, ImU32 color, bool hovered);
        void polyline(ImDrawList* drawList, const ImVec2* points, int count, ImU32 color, bool hovered, bool closed);
        void circle(ImDrawList* drawList, ImVec2 centre, float radius, ImU32 color, bool hovered);
        // a filled triangle with a dark rim
        void triangle(ImDrawList* drawList, ImVec2 a, ImVec2 b, ImVec2 c, ImU32 color);
        // a filled square handle with a dark rim (screen aligned), `side` px
        void square(ImDrawList* drawList, ImVec2 centre, float side, ImU32 color, bool hovered);
        // a diamond (a square turned 45°)
        void diamond(ImDrawList* drawList, ImVec2 centre, float side, ImU32 color, bool hovered);
        // a filled disc with a dark rim
        void disc(ImDrawList* drawList, ImVec2 centre, float radius, ImU32 color, bool hovered);
        // text on a dark plate, at `at` (its centre when `centred`, else its left edge, vertically centred)
        void label(ImDrawList* drawList, ImVec2 at, const std::string& text, ImU32 color, bool centred);
        // a circular-arrow cursor at the mouse (ImGui has no rotate cursor)
        void rotateCursor(ImDrawList* drawList, ImVec2 at);
    }

}
