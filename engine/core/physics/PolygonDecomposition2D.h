// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Splits a 2D polygon into convex pieces for Box2D (physics-2d design spec
// docs/superpowers/specs/2026-10-31-physics-2d-design.md, D8 and section 6.1.3).
//
// Pure function: no Box2D, no engine state, safe on any thread. Deterministic: the same input
// gives bit-identical pieces on every platform (plain float/double arithmetic, no hashing, no
// randomness).
//
// Units: any. The engine passes points (pixels) and a weld distance in the same unit.

#pragma once

#include "Export.h"
#include "math/Vector2.h"

#include <string>
#include <vector>

namespace jasperengine {

    struct Decomposition2D {
        std::vector<std::vector<Vector2>> pieces; // convex, counter-clockwise, each 3..maxVertices points
        bool convexInput = false;                  // the cleaned input was already convex
        bool selfIntersecting = false;             // input was not simple: pieces are its convex hull
        int droppedPieces = 0;                     // degenerate pieces removed (zero area / slivers)
        std::string error;                         // non-empty when nothing could be built (< 3 points left, zero area)
    };

    // points: a closed polygon (the last point connects to the first), either winding.
    //
    // 1. Consecutive points closer than weldDistance are welded (first against last too: a closing
    //    duplicate is dropped), exact duplicates always; collinear points are removed
    //    (|cross| < 1e-6 * scale^2, scale = the largest bounding-box extent). Fewer than 3 points
    //    left, a zero area or a non-finite point: error, no pieces.
    // 2. Clockwise input is reversed, so pieces are always counter-clockwise.
    // 3. Not simple (two non-adjacent edges cross or touch): selfIntersecting, and the pieces are
    //    the convex hull of the cleaned points split into fans.
    // 4. Convex with at most maxVertices points: one piece, convexInput.
    // 5. Otherwise ear clipping then Hertel-Mehlhorn merging into pieces of at most maxVertices.
    // 6. Pieces narrower than weldDistance (or with no area) are dropped and counted.
    //
    // Pieces use the cleaned input's points unchanged and cover exactly the cleaned polygon
    // without overlaps (minus dropped slivers). maxVertices below 3 is taken as 3.
    JASPERENGINE_API Decomposition2D decomposePolygon2D(const std::vector<Vector2>& points, float weldDistance,
                                                        int maxVertices = 8);

}
