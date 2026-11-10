// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/text/warp.ts by SafEight, used with the author's permission; its
// warp mesh constructions come from Patchy (MIT, Copyright (c) 2026 Seth A. Robinson; the
// notice is in Warp.cpp).
//
// Contract (docs/specs/S80-title-art-core.md R24, §6.6 "Outline operations")
// - Purpose: Photoshop's Warp Text on outlines: the fifteen styles as Bezier patches (cubic
//   4 x 2 / 4 x 3 / 4 x 4, quadratic 3 x 3) over the text box, with Horizontal / Vertical
//   Distortion scaling the mesh rows and columns (Photoshop's perspective-like taper).
// - Space: the mesh lives in content space as Photoshop's (top-left origin, +y down, w x h);
//   points map layer (y up) -> (u, v) in [0, 1]^2 -> patch -> layer. Unlike Photoshop the warped
//   frame is re-centred on the box centre, so a layer does not drift as its bend changes.
// - Thread-safety: pure.

#pragma once

#include "titleart/TitleArtDoc.h"
#include "titleart/geometry/Path.h"
#include "titleart/geometry/Remap.h"

#include <optional>
#include <string>
#include <vector>

namespace jasperengine::titleart::geometry {

    enum class WarpStyle : uint8_t {
        None, Arc, ArcLower, ArcUpper, Arch, Bulge, ShellLower, ShellUpper, Flag, Wave, Fish, Rise,
        Fisheye, Inflate, Squeeze, Twist
    };
    constexpr int WARP_STYLE_COUNT = 16;    // with None
    const char* warpStyleName(WarpStyle style);         // "none", "arc_lower" ... (the document's)
    bool warpStyleFromName(const std::string& name, WarpStyle& out);

    struct WarpParams {
        WarpStyle style = WarpStyle::None;
        double bend = 0.0, horizontal = 0.0, vertical = 0.0;    // -100..100
    };
    // a layer's `warp` object
    WarpParams decodeWarp(const Json& warp);
    bool warpIsIdentity(const WarpParams& warp);

    // the control mesh of a style at `bend` over a w x h content box, distortion applied
    struct WarpMesh {
        int nu = 4, nv = 2;                 // control points per row (u) and rows (v), 2..4
        std::vector<double> xs, ys;         // row-major, content space (y down)
    };
    WarpMesh warpMesh(const WarpParams& warp, double w, double h);

    // the layer-space point map of a warp over `box` and the warped frame (re-centred on the box
    // centre); none for an identity warp or an empty box
    struct Warper {
        PointMap map;
        TitleRect frame;
    };
    std::optional<Warper> makeWarper(const TitleRect& box, const WarpParams& warp);

    // the path warped over `box` (layer space); the path itself for an identity
    Path warpPath(const Path& path, const TitleRect& box, const WarpParams& warp);
    // `box` after the warp; `box` for an identity
    TitleRect warpFrame(const TitleRect& box, const WarpParams& warp);

}
