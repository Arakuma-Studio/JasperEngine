// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/text/offset.ts by SafEight, used with the author's permission
// (clipper2-ts there; the vendored C++ Clipper2, engine/libs/clipper2, here).
//
// Contract (docs/specs/S80-title-art-core.md R24, §6.6 "weight")
// - Purpose: polygon plumbing for outline offsets: face weight (Illustrator Offset Path),
//   plate closing, hulls and boxes (Plate.h), and stroke outlines (SVG strokes as outlines for
//   S86's meshes). Curves are flattened adaptively to a tolerance tied to the element size
//   (about 0.005 % of it: 0.01 px on a 200 px title, 0.08 px exported at 8x), Clipper's grid is
//   0.001 layer units, so polygon edges stay invisible at large export scales.
// - Polygons: y up, closed; unions are non-zero, so outers come out counter-clockwise (positive
//   area) and holes clockwise.
// - Thread-safety: pure.

#pragma once

#include "titleart/geometry/Path.h"

#include <vector>

namespace jasperengine::titleart::geometry {

    using Polygon = std::vector<TitleVec2>;
    using Polygons = std::vector<Polygon>;

    // Clipper's decimal grid: 0.001 layer units, well under the flattening tolerance
    constexpr int CLIPPER_PRECISION = 3;

    // the flattening / arc tolerance of an element of em or shape size `size`
    double flattenTolerance(double size);
    // the closed contours of a path as polygons; curves get just enough chords for the chord
    // error to stay under `tol` (the bound |B''| h^2 / 8 per segment of parameter step h)
    Polygons flattenPath(const Path& path, double tol);
    // polygons as closed move / line / close contours
    Path polygonsToPath(const Polygons& polygons);
    // the union under the path's fill rule (non-zero unless `evenOdd`): one consistent orientation
    Polygons unionPolygons(const Polygons& polygons, bool evenOdd = false);
    // round-joined offset of normalized polygons by `delta` (> 0 grows, < 0 shrinks)
    Polygons roundOffset(const Polygons& polygons, double delta, double tol);
    // face weight: the outline grown by `delta` with round joins (shrunk when negative: strokes
    // thinner than 2 |delta| vanish); the path itself for delta 0
    Path weightPath(const Path& path, double delta, double size);

    enum class OffsetJoin : uint8_t { Miter, Round, Bevel };
    enum class OffsetCap : uint8_t { Butt, Round, Square };
    // the outline of a stroke of `width` along a path's contours (open ones get `cap`)
    Path strokeOutline(const Path& path, double width, OffsetJoin join, OffsetCap cap, double miterLimit, double size);

}
