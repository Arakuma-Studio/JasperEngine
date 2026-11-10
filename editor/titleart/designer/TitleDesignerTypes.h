// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md §6.2)
// - Purpose: the small value types every part of the Title Designer shares: the canvas tools,
//   field references and edits (a row, gizmo, handle or search hit writes FieldEdits, which
//   become one `set` op per layer), and the canvas-to-screen mapping the gizmos, handles,
//   snapping and picking use.
// - Coordinates: document space is canvas pixels with the origin at the canvas centre and +y up
//   (S80 §6.1); screen space is ImGui's (pixels, +y down).
// - Thread-safety: values; the free functions are pure.

#pragma once

#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"

#include "imgui.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor::titledesigner {

    using Json = jasperengine::titleart::Json;
    using jasperengine::titleart::TitleRect;
    using jasperengine::titleart::TitleVec2;

    // the canvas tools (Unity's Q W E R T)
    enum class Tool : uint8_t { Hand, Move, Rotate, Scale, Rect };
    const char* toolName(Tool tool);            // "hand", "move", "rotate", "scale", "rect"
    bool toolFromName(const std::string& name, Tool& out);

    // one field of the document: `layer` a layer id ("" for a document member), `path` the long
    // field path of S80 §6.2 ("style.rims[1].width", "effects[0].params.size", "canvas.width")
    struct FieldRef {
        std::string layer;
        std::string path;
        friend bool operator==(const FieldRef& a, const FieldRef& b) { return a.layer == b.layer && a.path == b.path; }
        friend bool operator<(const FieldRef& a, const FieldRef& b) { return a.layer != b.layer ? a.layer < b.layer : a.path < b.path; }
    };

    // one value to write
    struct FieldEdit {
        std::string layer;
        std::string path;
        Json value;
    };

    // the op batch of a list of edits: one `set` op per layer, in first-seen order (document
    // members without `layer`), layers addressed as "id:<id>"
    Json editsToOps(const std::vector<FieldEdit>& edits);

    // A 2x3 affine of document space (or of a layer's parent space): x' = a x + c y + e,
    // y' = b x + d y + f (y up)
    struct Affine2 {
        double a = 1.0, b = 0.0, c = 0.0, d = 1.0, e = 0.0, f = 0.0;
        TitleVec2 apply(TitleVec2 p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
        // the linear part only (directions)
        TitleVec2 applyLinear(TitleVec2 v) const { return {a * v.x + c * v.y, b * v.x + d * v.y}; }
        double determinant() const { return a * d - b * c; }
        Affine2 inverse() const;
        // (this * other)(p) = this(other(p))
        Affine2 operator*(const Affine2& other) const;
        static Affine2 translate(double x, double y) { return {1.0, 0.0, 0.0, 1.0, x, y}; }
        static Affine2 scale(double sx, double sy) { return {sx, 0.0, 0.0, sy, 0.0, 0.0}; }
        // degrees clockwise on screen (S80's rotation; y up, so a positive angle turns +x towards -y)
        static Affine2 rotateClockwise(double degrees);
    };

    // A layer's transform (S80 §6.1) as an affine from its own space into its parent's: flips
    // about the position, then uniform scale, then rotation clockwise, then the translation
    Affine2 layerTransformAffine(const jasperengine::titleart::TitleTransform& transform);

    // the canvas on screen: `zoom` screen pixels per canvas pixel, `origin` the screen position of
    // document (0, 0) (the canvas centre)
    struct CanvasXform {
        float zoom = 1.0f;
        ImVec2 origin = ImVec2(0.0f, 0.0f);

        ImVec2 toScreen(TitleVec2 p) const { return ImVec2(origin.x + (float)p.x * zoom, origin.y - (float)p.y * zoom); }
        TitleVec2 toDoc(ImVec2 s) const { return {(s.x - origin.x) / zoom, (origin.y - s.y) / zoom}; }
        // a length on screen as canvas pixels
        double toDocLength(float screenPixels) const { return zoom > 0.0f ? screenPixels / zoom : 0.0; }
    };

    // the page a node shows (Properties), what a selection entry names
    enum class NodeKind : uint8_t { Document, Layer, Part, Effect, Mask };

    // One selected thing: a layer, one of its parts ("face", "rim:<id>", "bevel", "side",
    // "band:<id>"), an effect (layer stack or part stack) or a mask, by ids
    struct NodeRef {
        NodeKind kind = NodeKind::Document;
        std::string layer;      // layer id ("" for the document)
        std::string part;       // Part, and a part's effects: the part key
        std::string item;       // Effect / Mask: its id
        friend bool operator==(const NodeRef& a, const NodeRef& b) {
            return a.kind == b.kind && a.layer == b.layer && a.part == b.part && a.item == b.item;
        }
        friend bool operator!=(const NodeRef& a, const NodeRef& b) { return !(a == b); }
        friend bool operator<(const NodeRef& a, const NodeRef& b) {
            if (a.kind != b.kind) return a.kind < b.kind;
            if (a.layer != b.layer) return a.layer < b.layer;
            if (a.part != b.part) return a.part < b.part;
            return a.item < b.item;
        }
    };

}
