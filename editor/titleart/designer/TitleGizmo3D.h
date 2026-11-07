// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/ui/viewport/Gizmo3D.tsx, gizmoEdit.ts, rotation3d.ts and Handle.tsx
// (commit ecaa136, "Gizmo: fixed screen-space schematic, identical at every position") by
// SafEight, used with the author's permission.
//
// Contract (docs/specs/S82-title-designer.md R35, D9; S86 R56 supplies the targets)
// - Purpose: the 3D gizmo of the Title Designer's front-on canvas, for S86's Dimension 3D layers
//   and their letters: a fixed screen-space schematic, identical at every position, zoom and
//   rotation: X arm 84 px right, Y 84 px up, Z 60 px down-left (toward the viewer); three quarter
//   arcs at radius 50 (elliptical toward Z), each between the other two arms; a free-rotate ring at
//   114 px; a centre square for screen-plane moves. The centre is snapped to the device-pixel grid
//   and nothing is drawn when the pivot is behind the camera. The Scene view keeps true-axis
//   gizmos (D9): this schematic is only for the canvas's fixed front-on camera.
// - Hit rules: shafts take presses from 0.74 of the arm outwards (the arcs sit at 0.6, so both stay
//   grabbable where they cross), heads have their own shape with a 4 px margin, strokes are 20 px
//   wide (the ring 14), the centre square at least 24 px; the object stays draggable between
//   strokes (hitTest returns None there).
// - Drag mapping: an arrow measures travel along its drawn arm over the true px per world unit at
//   the pivot (Z at X's rate); an arc steps along its tangent and settles on the nearest ring point
//   within ±45° (a rate floor keeps a short stretch of ring from racing); turns compose on the
//   orientation at the press and are written back as the Euler solution nearest the previous value
//   (no flips); the ring is a trackball at 0.4° per px; Shift = 15° steps or the target's move step.
// - The target (S86 R56): Gizmo3DTarget names the pivot, the world displacement per unit of each
//   move value, how the rotation channels compose, the values at the press and a write-back that
//   turns the gesture's deltas into FieldEdits (a layer's transform, or selected letters' CharAdjust
//   members), so S86 plugs a layer or a letter in without changes here.
// - Per frame (lead): layout(target, camera); when idle hitTest(); on press begin(); while dragging
//   update() and stage its edits (absolute from the press: Esc drops them); end(); draw().
// - Thread-safety: main thread (a value type; no ImGui calls outside draw()).

#pragma once

#include "titleart/designer/TitleDesignerTypes.h"
#include "titleart/designer/TitleSnap.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor::titledesigner {

    struct Vec3d {
        double x = 0.0, y = 0.0, z = 0.0;
        double operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
        double& operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
    };

    // a 3 x 3 matrix, m[row][column], acting on column vectors
    struct Mat3d {
        double m[3][3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
        Mat3d operator*(const Mat3d& o) const;
        Vec3d operator*(const Vec3d& v) const;
        Mat3d transposed() const;
        Mat3d inverse() const;
        double determinant() const;
        static Mat3d diagonal(double x, double y, double z);
    };

    // the maths of rotation3d.ts (pure, unit-tested)
    namespace rotation3d {
        constexpr double ARM_PX = 84.0;         // X / Y arms, screen px
        constexpr double Z_ARM_PX = 60.0;       // Z arm, on the down-left diagonal
        constexpr double ARC_PX = 50.0;         // rotation arcs (0.6 of the arm)
        constexpr double RING_PX = ARM_PX + 30.0;   // free-rotate ring, clear of the axis letters
        constexpr double SHAFT_GRAB = 0.74;     // a shaft takes presses from this share of its arm
        constexpr double ROTATE_PER_PX = 0.4;   // trackball degrees per px
        constexpr double ROTATE_SNAP = 15.0;    // Shift

        // the drawn arm of axis 0..2 from the centre, screen px (y down): X right, Y up, Z down-left
        ImVec2 schematicArm(int axis);
        // the comp's axis i (X right, Y up, Z toward the viewer); the arc of axis i runs from axis
        // i+1 to axis i+2 (cyclic), positive about axis i
        Vec3d compAxis(int axis);
        // world distance along comp axis i of a drag (gx, gy) screen px, `k` screen px per world
        // unit at the pivot: X and Y follow the cursor, Z moves at the same rate
        double armTravel(int axis, double gx, double gy, double k);
        // three.js's Euler matrices (radians): ZYX = Rz Ry Rx, YXZ = Ry Rx Rz
        Mat3d eulerZYX(double x, double y, double z);
        Mat3d eulerYXZ(double x, double y, double z);
        // of `candidates` (degrees), each unwrapped by whole turns toward `previous`, the closest to it
        Vec3d nearestBranch(const std::vector<Vec3d>& candidates, const Vec3d& previous);
        // `w0` turned `degrees` about world unit axis `n`, in world space (on the left)
        Mat3d turnAbout(const Mat3d& w0, const Vec3d& n, double degrees);
        // `w0` turned `alpha` about the screen's up axis, then `beta` about its right axis (world
        // unit vectors), in world space
        Mat3d trackball(const Mat3d& w0, const Vec3d& right, const Vec3d& up, double alpha, double beta);
        // the move values giving world displacement `w` for a target whose values move it `axes`
        Vec3d moveValues(const std::array<Vec3d, 3>& axes, const Vec3d& w);
    }

    // A target's rotation channels [X, Y, Z] (degrees) and its world orientation.
    // Layer (S86's transform.rotation_x / rotation_y / rotation): R = Rz(-rotZ) Ry(rotY) Rx(rotX).
    // Letter (tilt_x / tilt_y / rotation inside its layer, whose rotation and flip are `base`):
    // world = base Ry(tiltY) Rx(tiltX) Rz(-rotate).
    class RotationModel {
    public:
        static RotationModel layer();
        static RotationModel glyph(const Mat3d& base);
        Mat3d orient(const Vec3d& rot) const;
        // the channels of `w`: of the two Euler solutions, the one nearest `previous`, each channel
        // unwrapped toward it
        Vec3d fromOrient(const Mat3d& w, const Vec3d& previous) const;

    private:
        bool glyphModel = false;
        Mat3d base, baseInverse;
    };

    enum class Gizmo3DHandle : uint8_t { None, MoveX, MoveY, MoveZ, MovePlane, RotateX, RotateY, RotateZ, FreeRotate };
    enum class Gizmo3DGesture : uint8_t { Move, Rotate };

    // What the gizmo edits (S86 R56), as it is now
    struct Gizmo3DTarget {
        std::string id;                         // the layer id
        Vec3d origin;                           // the pivot, world space (canvas px, z toward the viewer)
        // world displacement per unit of the X / Y / Z move values (a letter's runs through its layer)
        std::array<Vec3d, 3> axes = {Vec3d{1.0, 0.0, 0.0}, Vec3d{0.0, 1.0, 0.0}, Vec3d{0.0, 0.0, 1.0}};
        RotationModel rotation = RotationModel::layer();
        Vec3d pos;                              // move values (letters: the primary one's)
        Vec3d rot;                              // rotation channels
        std::string unit = "px";                // "px" or "%" (readout)
        double step = 10.0;                     // Shift step of the move values
        // The gesture's result as edits: `dPos` added to the move values, `dRot` to the rotations,
        // both from the values at the press. Return every field the gesture drives (Move: all
        // three move values, Rotate: all three channels), also those whose delta is 0, so applying
        // the edits to the document as it was at the press is exact.
        std::function<std::vector<FieldEdit>(const Vec3d& dPos, const Vec3d& dRot, Gizmo3DGesture gesture)> write;
    };

    // A layer's target: move values transform.position [x, y] and transform.z, rotations
    // transform.rotation_x, rotation_y and rotation (S86 R56), its own axes
    Gizmo3DTarget layerGizmo3DTarget(const std::string& layerId, const Vec3d& origin, const Vec3d& position, const Vec3d& rotation, double moveStep);

    // the canvas camera: world -> screen (false when behind the camera), the screen's right and up
    // as world unit vectors, and the device pixel ratio (ImGui's DisplayFramebufferScale)
    struct Gizmo3DCamera {
        std::function<bool(const Vec3d& world, ImVec2& screen)> project;
        Vec3d right{1.0, 0.0, 0.0};
        Vec3d up{0.0, 1.0, 0.0};
        float pixelRatio = 1.0f;
        // the canvas's own front-on orthographic view
        static Gizmo3DCamera frontOn(const CanvasXform& view, float pixelRatio = 1.0f);
    };

    class TitleGizmo3D {
    public:
        // false (and nothing drawn) when the pivot is behind the camera
        bool layout(const Gizmo3DTarget& target, const Gizmo3DCamera& camera);
        bool visible() const { return shown; }
        ImVec2 centre() const { return c; }
        // the drawn tip of arm 0..2 on screen
        ImVec2 armEnd(int axis) const;
        // screen px per world unit at the pivot
        double pxPerUnit() const { return k; }

        Gizmo3DHandle hitTest(ImVec2 screen) const;
        bool begin(Gizmo3DHandle handle, ImVec2 screen, const GizmoModifiers& modifiers);
        std::vector<FieldEdit> update(ImVec2 screen, const GizmoModifiers& modifiers);
        void end();
        bool active() const { return drag.handle != Gizmo3DHandle::None; }

        // "Move X", "Rotate Y", "Free Rotate", "Move" (the lead names the step: "Rotate 3D Layer")
        std::string actionName() const;
        // the hovered action, or during a drag the action and only the values it changed
        // ("Rotate X · X 12.5°")
        std::string readout(Gizmo3DHandle hovered) const;

        void draw(ImDrawList* drawList, Gizmo3DHandle hovered, Gizmo3DHandle activeHandle) const;

    private:
        struct Drag {
            Gizmo3DHandle handle = Gizmo3DHandle::None;
            Gizmo3DTarget target;
            ImVec2 press;
            ImVec2 last;
            Mat3d w0;
            Vec3d prev;             // the channels written last (the branch nearest them wins)
            double th = 0.0, th0 = 0.0;
            Vec3d nowPos, nowRot;
        };
        ImVec2 arcPoint(int axis, double th) const;
        // the angle in [from, to] whose arc point is nearest `p`, and its distance
        double nearestOnArc(int axis, double from, double to, ImVec2 p, double* distance) const;
        Vec3d plane(double gx, double gy) const;

        bool shown = false;
        Gizmo3DTarget current;
        ImVec2 c;
        double k = 1.0;
        ImVec2 ur, uu;              // screen px per world unit along the screen's right / up at the pivot
        Vec3d right{1.0, 0.0, 0.0}, up{0.0, 1.0, 0.0};
        Drag drag;
    };

}
