// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/engine/three/outline.ts by SafEight, used with the author's
// permission (clipper2-ts there; the vendored C++ Clipper2, engine/libs/clipper2, here).
//
// Contract (docs/specs/S86-title-3d.md R6, §6.2)
// - Purpose: the 2D outline plumbing of the 3D mesher. Outlines are flattened with a fixed
//   number of chords per curve segment (Pachislop's `curveSegments`), then normalised by a
//   Clipper2 union on a 0.01 layer-unit grid (OUTLINE_GRID) with miter limit 2:
//   near-collinear vertices closer than 2 x eps to their neighbours' line are dropped, outer
//   contours come out counter-clockwise (positive area, y up) and holes clockwise, islands inside
//   holes become shapes of their own. With that convention the filled region is on the left of
//   travel and an edge with direction d has the outward normal (d.y, -d.x).
// - eps is the outline tolerance of a title: max(1e-4, size x 1e-4) (outlineEps).
// - Thread-safety: pure functions (the mesher's jobs call them in parallel).

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"
#include "titleart/geometry/Path.h"

#include <vector>

namespace jasperengine::titleart::three_d {

    using Contour = std::vector<TitleVec2>;

    struct ShapeContours {
        Contour outer;                  // counter-clockwise
        std::vector<Contour> holes;     // clockwise
    };
    using Shapes = std::vector<ShapeContours>;

    // Clipper's grid of the 3D outlines: 0.01 layer units (Pachislop's precision 2)
    constexpr double OUTLINE_GRID = 0.01;
    // the sharpest corner a miter join keeps (a multiple of the offset), as the extruder's miters
    constexpr double OUTLINE_MITER_LIMIT = 2.0;
    // vertices nearer than this x eps to their neighbours' line are dropped
    constexpr double OUTLINE_SIMPLIFY = 2.0;

    // the outline tolerance of a layer of source size `size`
    JASPERENGINE_API double outlineEps(double size);

    JASPERENGINE_API double signedArea(const Contour& contour);
    JASPERENGINE_API double contourLength(const Contour& contour);
    // consecutive near-duplicates (closer than eps) removed, a closing duplicate included
    JASPERENGINE_API Contour cleanContour(const Contour& contour, double eps);

    // any closed contours (any winding) -> clean outer / hole shapes under the fill rule
    JASPERENGINE_API Shapes normalizeContours(const std::vector<Contour>& raw, bool evenOdd, double eps);
    // the union of already normalised shape sets
    JASPERENGINE_API Shapes unionShapes(const Shapes& shapes, double eps);
    // grown (delta > 0) or shrunk by delta with miter (limit 2) or round joins; fillCounters drops
    // the holes first, so counters become solid
    JASPERENGINE_API Shapes offsetShapes(const Shapes& shapes, double delta, bool roundJoin, bool fillCounters, double eps);
    // `outer` minus `holes` (any orientation) for a region whose contours touch or overlap: the
    // contours are pulled one grid step into the region first, so touching ones merge, then cut
    JASPERENGINE_API Shapes subtractContours(const Contour& outer, const std::vector<Contour>& holes, double eps);

    // a point strictly inside a normalised shape: just inside the middle of its longest outer edge
    JASPERENGINE_API TitleVec2 interiorPoint(const ShapeContours& shape);
    // non-zero winding of (x, y) against any-orientation contours
    JASPERENGINE_API int windingNumber(const std::vector<Contour>& contours, double x, double y);
    JASPERENGINE_API bool shapeContains(const ShapeContours& shape, double x, double y);
    // per shape, the index of the last group whose raw contours contain its interior point, or -1
    JASPERENGINE_API std::vector<int> shapeOwners(const Shapes& shapes, const std::vector<std::vector<Contour>>& groups);
    JASPERENGINE_API TitleRect shapesBounds(const Shapes& shapes);

    // a path's contours as closed polylines, `curveSegments` chords per quadratic or cubic
    // segment (Pachislop's fixed flattening; halved in draft by the caller)
    JASPERENGINE_API std::vector<Contour> flattenContours(const geometry::Path& path, int curveSegments);
    // polygonal path of normalised shapes (outers and holes as closed line contours)
    JASPERENGINE_API geometry::Path shapesToPath(const Shapes& shapes);
    // content hash of a shape set, in order (the mesher's cache keys)
    JASPERENGINE_API uint64_t hashShapes(const Shapes& shapes);

}
