// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/text/remap.ts by SafEight, used with the author's permission.
//
// Contract (docs/specs/S80-title-art-core.md §6.6 "curve refit")
// - Purpose: non-linear point maps (warp, free distort) applied to outlines. The image of every
//   segment is re-fitted as quadratics through mapped samples, subdivided until the fit is
//   within tolerance, so warped titles stay smooth curves at any export scale instead of
//   polylines. Straight input edges bend too, including a contour's implicit closing edge;
//   projective maps (`straight`) map lines by their end points.
// - Output: closed contours of move / line / quadratic commands (layer space, y up).
// - Thread-safety: pure.

#pragma once

#include "titleart/geometry/Path.h"

#include <functional>

namespace jasperengine::titleart::geometry {

    struct PointMap {
        std::function<TitleVec2(const TitleVec2&)> map;
        double tol = 1e-3;          // fit tolerance, layer units
        double span = 1.0;          // longest original-space span one fitted piece starts from
        bool straight = false;      // the map keeps straight lines straight (projective)
    };

    Path remapPath(const Path& path, const PointMap& map);

}
