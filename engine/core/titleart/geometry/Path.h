// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.6)
// - Purpose: the outline type every Title Art geometry stage passes on: glyph outlines from
//   HarfBuzz hb-draw, shapes, SVG paths, warped, distorted and offset outlines, plates, and what
//   the mask rasterizer (ThorVG), S86's mesher and S87's SVG export read.
// - Content: move / line / quadratic / cubic / close commands in one coordinate space (em units
//   from a font, then layer space: y up, the source's own winding; CFF cubics stay cubics).
//   Fill rule non-zero unless `evenOdd`.
// - Thread-safety: a value.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"

#include <vector>

namespace jasperengine::titleart::geometry {

    enum class PathVerb : uint8_t { Move, Line, Quad, Cubic, Close };

    // 2 x 3 affine: x' = a x + c y + e, y' = b x + d y + f
    struct Affine {
        double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
        TitleVec2 apply(const TitleVec2& p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
        // this after `other` (apply other first)
        Affine after(const Affine& other) const;
        static Affine translate(double x, double y);
        static Affine scale(double sx, double sy);
        static Affine rotateDegrees(double degrees);     // counter-clockwise in a y-up space
    };

    class JASPERENGINE_API Path {
    public:
        void moveTo(double x, double y);
        void lineTo(double x, double y);
        void quadTo(double cx, double cy, double x, double y);
        void cubicTo(double c1x, double c1y, double c2x, double c2y, double x, double y);
        void close();
        void append(const Path& other);
        void clear();

        bool empty() const { return verbs.empty(); }
        size_t contourCount() const;
        const std::vector<PathVerb>& verbList() const { return verbs; }
        // the points of every verb in order (Move/Line 1, Quad 2, Cubic 3, Close 0)
        const std::vector<TitleVec2>& pointList() const { return points; }

        // the bounds of the control points (contain the curve)
        TitleRect controlBounds() const;
        // the bounds of the curve (extrema of the segments)
        TitleRect bounds() const;
        void transform(const Affine& m);
        // the outline as polylines, one per contour, each segment within `tolerance` of the curve
        std::vector<std::vector<TitleVec2>> flatten(double tolerance) const;
        // the signed area of the flattened outline (positive counter-clockwise in a y-up space)
        double signedArea(double tolerance) const;

        bool evenOdd = false;

    private:
        std::vector<PathVerb> verbs;
        std::vector<TitleVec2> points;
    };

}
