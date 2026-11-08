// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Gradient-line maths ported from Pachislop src/ui/viewport/gradient.ts, shape handles from
// shapeHandles.ts and ParamHandles.tsx, distort points from DistortHandles.tsx, the light dial from
// LightDial.tsx and the light sphere from src/ui/LightSphere.tsx, by SafEight, used with the
// author's permission.
//
// Contract (docs/specs/S82-title-designer.md R33)
// - Purpose: the canvas handles of the selected effect, generator, mask, part or layer: hit tests,
//   drags producing FieldEdits, live labels and ImDrawList drawing. Registry members come from
//   their definition's gizmo hints (TitleEffectRegistry GizmoHint: Point, Direction, Radius,
//   Light); document members outside the registry from fixed tables here: a fill's gradient line
//   (`angle`, `scale`, `offset` of a linear / radial / reflected / diamond fill) or angle dial (an
//   angle fill), bevel and satin lights (a dial), distort corners and mesh points (with an on-canvas
//   Reset), shape handles (`corner`, `inner`, `points`, a speech `tail`), the warp `bend`, and a
//   gradient or shape mask's position and direction.
// - Global light: a direction or light handle whose binding follows the document's light (the
//   definition's GlobalLightBinding with `use_global_light` on; a bevel's or satin's own flag)
//   edits `global_light.angle` (and `altitude`), document members (FieldEdit layer ""), converted
//   through the binding's relation and convention, and its label says "Global Light 120°"; with
//   the flag off it edits the param itself.
// - Spaces: effect and generator directions are screen-fixed (document space, like the global
//   light); Point params are canvas px (+y up) through HandleFrame::pointToDoc (a following
//   generator: offsets from its followed layer's centre); fills, distort and shape handles live in
//   the layer's own space; a mask's values in its space (its layer's when linked, else the canvas).
// - Gestures are absolute (from the values at begin(); Esc drops the staged edits) and every field
//   a handle drives is in every update's edits. Shift: 15° steps on angles, a point snaps to the
//   layer centre within 8 screen px, the light dial's altitude, distort's mirrored point.
// - Per frame (lead): clear(), setView(), addNode() for the selected node (or the add* builders),
//   then hitTest / begin / update / end as with the gizmo; draw() above the gizmo (handles win the
//   hit test over it).
// - Thread-safety: main thread (values; no ImGui calls outside draw() and lightSphere()).

#pragma once

#include "titleart/designer/TitleDesignerTypes.h"
#include "titleart/designer/TitleSnap.h"
#include "titleart/TitleEffectRegistry.h"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor::titledesigner {

    using jasperengine::titleart::AngleConvention;
    using jasperengine::titleart::EffectDef;
    using jasperengine::titleart::GeneratorDef;
    using jasperengine::titleart::GizmoHint;
    using jasperengine::titleart::GlobalLightBinding;
    using jasperengine::titleart::ParamDef;
    using jasperengine::titleart::ParamUnit;

    // the layer a handle belongs to, as the canvas places it
    struct HandleFrame {
        std::string layerId;
        Affine2 layerToDoc;         // the layer's own space -> document (parentToDoc x layerTransformAffine)
        TitleRect localBox;         // its content box, layer space
        double sourceSize = 0.0;    // layerSourceSize: the unit of its % members
        Affine2 pointToDoc;         // Point params' space -> document (canvas px: identity)
    };

    // the document's global_light
    struct GlobalLight {
        double angle = 120.0;       // degrees counter-clockwise, where the light comes from
        double altitude = 30.0;
    };
    GlobalLight globalLightOf(const Json& root);

    enum class HandleKind : uint8_t {
        Point, Direction, Radius, Light, AngleDial,
        GradientStart, GradientEnd, GradientLine,
        DistortPoint, DistortReset,
        ShapeCorner, ShapeInner, ShapePoints, ShapeTail, WarpBend
    };

    // ---- the pure maths ----

    // the global light angle G that makes a bound param read `paramAngle` (the inverse of
    // angleFromGlobalLight), wrapped to (-180, 180]
    double globalAngleForParam(const GlobalLightBinding& binding, double paramAngle);

    // the light sphere (and dial): a pointer `dx`, `dy` screen px (y down) from the centre of a
    // sphere of `radius` px; the distance sets the altitude (centre 90°, rim 0°), the direction the
    // angle (0..360, in `convention`); `snap` (Shift) rounds to 15° / 5°
    void lightFromSphere(double dx, double dy, double radius, AngleConvention convention, bool snap, double& angle, double& altitude);

    // A fill's gradient line in its box (layer space, y up), as the 2D fill maps it: v = p - c,
    // hs = half box, dir = (cos a, -sin a), ext = |dir.x| hs.x + |dir.y| hs.y, s = scale:
    // linear t = dot(v, dir) / (2 ext s) + 0.5 - offset; reflected |dot(v, dir)| / (ext s) - offset;
    // radial |v| / (|hs| s) - offset; diamond (|v.x| + |v.y|) / ((hs.x + hs.y) s) - offset.
    // Handle `a` sits where t = 0, `b` where t = 1.
    struct GradientParams {
        std::string kind = "linear";    // linear, reflected, radial, diamond, angle
        double angle = 90.0;            // clockwise on screen, 0 = left to right
        double scale = 1.0;
        double offset = 0.0;            // -1..1
    };
    struct GradientPoints {
        TitleVec2 c;
        bool hasA = true;               // angle gradients have no start handle
        TitleVec2 a, b;
    };
    GradientPoints gradientHandles(const GradientParams& params, const TitleRect& box);
    // the params after dragging handle a (`start`) or b to `p`, the other staying; `snap` rounds the angle to 15°
    GradientParams dragGradientHandle(const GradientParams& params, const TitleRect& box, bool start, TitleVec2 p, bool snap);
    // the line dragged by `delta`: only the part along the axis moves the offset
    GradientParams dragGradientLine(const GradientParams& params, const TitleRect& box, TitleVec2 delta);

    // the extra geometry addNode needs for some layers
    struct HandleNodeExtras {
        const std::vector<TitleVec2>* shapeVertices = nullptr;  // a Shape layer's on-curve outline points (layer space)
        const TitleRect* distortFrame = nullptr;                // the undistorted frame (default: the frame's localBox)
        std::function<TitleVec2(TitleVec2)> warp;               // places a shape handle on the warped outline (layer space)
    };

    class TitleHandles {
    public:
        void clear();
        void setView(const CanvasXform& value) { view = value; }

        // ---- the handles of a selection node (Effect, Part, Layer, Mask); none for others ----
        void addNode(const Json& root, const NodeRef& node, const HandleFrame& frame, const HandleNodeExtras& extras = HandleNodeExtras());

        // ---- builders (paths are long field paths in the layer, S80 §6.2) ----
        // the gizmo hints of an effect (`paramsPath` "effects[2].params" or
        // "part_effects.rim:<id>[0].params") or of a generator layer ("generator.params")
        void addEffect(const HandleFrame& frame, const EffectDef& def, const Json& params, const std::string& paramsPath, const GlobalLight& light);
        void addGenerator(const HandleFrame& frame, const GeneratorDef& def, const Json& params, const std::string& paramsPath, const GlobalLight& light);
        // a material fill ("style.face.fill"): the gradient line, or an angle dial for an angle fill
        void addFill(const HandleFrame& frame, const Json& fill, const std::string& fillPath, const std::string& owner);
        // a dial at the box centre writing one angle (`space`: the angle's space -> document)
        void addAngleDial(const HandleFrame& frame, const std::string& path, double angle, const std::string& owner, const std::string& field,
                          AngleConvention convention, const Affine2& space);
        // a material's bevel ("style.face.bevel") and satin lights
        void addBevelLight(const HandleFrame& frame, const Json& bevel, const std::string& path, const GlobalLight& light);
        void addSatin(const HandleFrame& frame, const Json& satin, const std::string& path, const GlobalLight& light);
        // distort corners / mesh points over `distortFrame` (layer space), and Reset
        void addDistort(const HandleFrame& frame, const TitleRect& distortFrame, const Json& distort, const std::string& path);
        // a parametric shape's handles: `shape` holds its geometry members, `prefix` "" for a layer
        // or "masks[0].shape." for a shape mask; `vertices` its base outline's on-curve points
        void addShape(const HandleFrame& frame, const Json& shape, const std::string& prefix, const std::vector<TitleVec2>& vertices,
                      const std::function<TitleVec2(TitleVec2)>& warp = nullptr);
        // the warp bend, above the top edge (on screen, whatever the flips)
        void addWarpBend(const HandleFrame& frame, const Json& warp, const std::string& path, bool flipX, bool flipY);
        // a mask ("masks[1]"): its position, and a gradient mask's direction and length; `frame`'s
        // layerToDoc is the mask's space
        void addMask(const HandleFrame& frame, const Json& mask, const std::string& path);

        // ---- interaction ----
        int count() const { return (int)handles.size(); }
        HandleKind kind(int index) const { return handles[(size_t)index].kind; }
        // the live label ("Angle 60° · Distance 10 px", "Global Light 120°")
        const std::string& label(int index) const { return handles[(size_t)index].label; }
        ImVec2 screenPosition(int index) const;
        // the topmost handle under `screen`, -1 for none
        int hitTest(ImVec2 screen) const;
        bool begin(int index, ImVec2 screen, const GizmoModifiers& modifiers);
        std::vector<FieldEdit> update(ImVec2 screen, const GizmoModifiers& modifiers, const SnapContext* snap = nullptr);
        void end();
        bool active() const { return activeIndex >= 0; }
        // the undo step: "Change Drop Shadow Angle", "Change Global Light Angle"
        std::string gestureName() const;

        // `highlight`: every handle lit (hovering the card's ✥)
        void draw(ImDrawList* drawList, int hovered, int activeHandle, bool highlight = false) const;

    private:
        struct Handle {
            HandleKind kind = HandleKind::Point;
            std::string layer;              // FieldEdit layer
            std::string owner;              // "Drop Shadow", "Face"
            std::string field;              // "Angle"
            std::string path, path2, path3; // the fields it writes: the main one, the second (distance, altitude, scale), the third (offset)
            std::string layer2;             // the second field's layer (a global angle with the layer's own distance)
            HandleFrame frame;
            Affine2 space;                  // the values' space -> document
            AngleConvention convention = AngleConvention::ClockwiseFromRight;
            double value = 0.0, value2 = 0.0, value3 = 0.0;
            double min = 0.0, max = 0.0, min2 = 0.0, max2 = 0.0;
            bool hasSecond = false;
            ParamUnit unit = ParamUnit::None;
            bool global = false;
            GlobalLightBinding binding;     // registry handles: the definition's
            bool bindingAltitude = false;   // the altitude follows the global light too
            TitleVec2 at;                   // in `space`: a point's value, a direction's anchor, a shape handle
            int slot = 0;                   // light dials: their place beside the box
            std::string text;               // gradient kind, shape kind
            TitleRect box;                  // gradient box, distort frame
            int index = 0;                  // distort point, gradient handle
            bool flag = false;              // mesh (distort), sides (points), flipY (bend), mode none (distort)
            std::vector<TitleVec2> list;    // distort offsets
            double size = 0.0, aspect = 1.0, extra = 0.0;   // shapes: size, aspect, the corner's vertex x
            std::string label;
        };

        void push(Handle handle);
        void refreshLabel(Handle& h) const;
        ImVec2 anchorScreen(const Handle& h) const;
        ImVec2 dialCentre(const Handle& h) const;
        ImVec2 positionOf(const Handle& h) const;
        TitleVec2 boxCentreDoc(const HandleFrame& frame) const;
        void addHint(const HandleFrame& frame, const GizmoHint& hint, const std::vector<ParamDef>& defs, const Json& params, const std::string& paramsPath,
                     const GlobalLightBinding& binding, const char* owner, const GlobalLight& light);

        std::vector<Handle> handles;
        CanvasXform view;
        int lightSlots = 0;
        int activeIndex = -1;
        Handle start;                   // the active handle at begin
        TitleVec2 pressDoc;
        ImVec2 pressScreen;
    };

    // The Properties row's light sphere (Pachislop's LightSphere): drag the highlight; the
    // distance from the centre sets the altitude (centre = 90°), the direction the angle; Shift
    // snaps to 15° / 5°. True while the values change (ImGui::IsItemActivated /
    // IsItemDeactivatedAfterEdit after it bracket the gesture).
    bool lightSphere(const char* id, double& angle, double& altitude, AngleConvention convention, float size = 72.0f);

}
