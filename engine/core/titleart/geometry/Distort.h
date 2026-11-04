// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/text/distort.ts by SafEight, used with the author's permission.
//
// Contract (docs/specs/S80-title-art-core.md R24, §6.6)
// - Purpose: free distort (Photoshop Distort / Perspective, a mesh warp) over a frame box in
//   layer space (y up). Offsets are fractions of the frame (x of its width, y of its height,
//   +y up). `corners`: the projective map sending the frame to the four moved corners (TL, TR,
//   BR, BL), so lines stay straight, with the denominator floored at 0.02 so points past the
//   vanishing line stay finite. `mesh`: a bicubic patch whose 4 x 4 control points (row-major
//   from the top-left) start evenly spaced over the frame, with a C1 linear extension outside
//   it (per-character nudges can leave the frame).
// - Thread-safety: pure.

#pragma once

#include "titleart/TitleArtDoc.h"
#include "titleart/geometry/Path.h"
#include "titleart/geometry/Remap.h"

#include <array>
#include <optional>

namespace jasperengine::titleart::geometry {

    enum class DistortMode : uint8_t { None, Corners, Mesh };

    struct DistortParams {
        DistortMode mode = DistortMode::None;
        std::array<TitleVec2, 4> corners{};
        std::array<TitleVec2, 16> mesh{};
    };
    // a layer's `distort` object
    DistortParams decodeDistort(const Json& distort);
    bool distortIsIdentity(const DistortParams& distort);

    // the point map over `frame` (the undistorted box the offsets refer to); none for an identity
    std::optional<PointMap> makeDistorter(const TitleRect& frame, const DistortParams& distort);
    // one point through the distort (on-canvas handles, S82)
    TitleVec2 distortPoint(const TitleRect& frame, const DistortParams& distort, const TitleVec2& p);
    Path distortPath(const Path& path, const TitleRect& frame, const DistortParams& distort);

}
