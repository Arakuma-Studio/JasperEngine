// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/text/plate.ts by SafEight, used with the author's permission.
//
// Contract (docs/specs/S80-title-art-core.md R26, §6.1 Rim `plate`)
// - Purpose: the base outline a rim grows from (S86's backplates use the same function, so both
//   match exactly): `contour` (the outline itself, or with `close` its morphological closing:
//   Clipper2 union, offset +r, offset -r, round joins), `hull` (the convex hull) or `box` (a
//   box leaning by `slant` plus the text's shear, its corners cut by straight chamfers of
//   `corner`, clamped to half the shorter side). Input and output are layer space, y up; the
//   rim width is added on top by the caller.
// - A plate base is grown again by the rim, and its curves are mostly the closing arcs, so it
//   flattens at 4x the face tolerance (0.02 % of the size).
// - Thread-safety: pure.

#pragma once

#include "titleart/TitleArtDoc.h"
#include "titleart/geometry/Offset.h"
#include "titleart/geometry/Path.h"

namespace jasperengine::titleart::geometry {

    enum class PlateKind : uint8_t { Contour, Hull, Box };

    struct PlateParams {
        PlateKind kind = PlateKind::Contour;
        double close = 0.0;     // % of size
        double corner = 4.0;    // % of size
        double slant = 0.0;     // deg
    };
    // a rim's `plate` object
    PlateParams decodePlate(const Json& plate);
    // true when the plate is the outline itself (contour without closing)
    bool plateIsIdentity(const PlateParams& plate);

    // Andrew's monotone chain, counter-clockwise (positive area, y up)
    Polygon convexHull(Polygon points);
    // the base outline of `plate` for outlines `outline` of em / shape size `size` and text shear
    // `shear` (deg, added to a box's lean); `outline` itself for an identity plate
    Path plateBase(const Path& outline, const PlateParams& plate, double size, double shear);

}
