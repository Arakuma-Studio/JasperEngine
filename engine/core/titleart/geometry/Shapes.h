// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/text/shapes.ts by SafEight, used with the author's permission.
//
// Contract (docs/specs/S80-title-art-core.md R25, §6.6 "Shapes")
// - Purpose: the eleven parametric shapes of Shape layers and shape masks (rect,
//   parallelogram, ellipse, orb, polygon, star, burst, shield, speech, shout, ribbon) as outlines
//   in layer space: y up, centred on the origin, every contour clockwise like TrueType glyphs, so
//   fills, counters and offsets treat shapes and glyphs alike. Corners are true circular arcs
//   (k = 4/3 tan(phi/4) r) clamped to half an edge; burst jitter is seeded (mulberry32); speech
//   and shout tails are spliced into one contour; ribbons are three same-winding contours.
// - The body is `size` tall and `size x aspect` wide; tails reach out to their tip.
// - Thread-safety: pure.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/geometry/Path.h"

#include <string>

namespace jasperengine::titleart::geometry {

    struct ShapeParams {
        std::string shape = "rect";
        double size = 200.0, aspect = 1.0, corner = 0.0, slant = 0.0;
        int points = 5;
        double inner = 0.5, jitter = 0.0;
        int64_t seed = 1;
        TitleVec2 tail{0.0, -60.0};     // %, +y up
        double tailWidth = 20.0;        // %
    };
    // a shape layer's or a shape mask's geometry members
    JASPERENGINE_API ShapeParams decodeShape(const Json& object);
    JASPERENGINE_API Path shapePath(const ShapeParams& shape);

    // mulberry32: small, fast, deterministic per seed (Pachislop's, for burst / shout jitter)
    class Mulberry32 {
    public:
        explicit Mulberry32(int64_t seed);
        double next();      // [0, 1)
    private:
        uint32_t state;
    };

}
