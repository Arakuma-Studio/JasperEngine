// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/generators/rings/brushSwirl.ts (buildStrokes, ribbonGeometry,
// drawSplatter, reach) by SafEight, used with the author's permission.

// Contract (docs/specs/S83-title-effects-generators.md §6.7.7)
// - Purpose: Brush Swirl's CPU layout: each stroke's centreline samples (position, tangent,
//   tapered half width, arc length), the ribbon of triangles the MeshBrushSwirl program shades,
//   and the splatter droplets, as pure functions of the params and the element seed, bit for
//   bit Pachislop's (its mulberry32 stream through gen::Rng, its b/common.ts hash through
//   gen::hashN, its Float32Array rounding of the samples), so they are testable without a GPU.
// - Spaces: layer px, +y up; `angle` is Pachislop's start angle (degrees counter-clockwise): the
//   definition negates its clockwise param (D16) before calling.
// - Stroke coordinates: along a stroke of M samples, u = k / (M - 1) runs 0..1 and `len` is the
//   arc length from the start; both grow monotonically along the stroke. `reveal` (S83's new
//   param) draws each stroke up to u = reveal: the ribbon keeps every triangle (the program cuts
//   at u, so a live frame changes one uniform and re-uploads nothing), the splatter keeps the
//   droplets thrown at or before it (each droplet's random values are drawn either way, so a
//   revealed droplet is the one the full stroke throws).
// - Thread-safety: pure functions.

#pragma once

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jasperengine::titleart::analysis {

    enum class BrushPath : uint8_t { Arc, Spiral, Slash, Ring };

    struct BrushStrokeParams {
        BrushPath path = BrushPath::Arc;
        double cx = 0.0, cy = 0.0;          // the centre, layer px
        double radius = 380.0;
        double sweep = 300.0;               // degrees (arc, spiral)
        double angle = 150.0;               // the start, degrees counter-clockwise (Pachislop's)
        int strokes = 4;
        double spacing = 26.0;
        double width = 70.0;
        double taperIn = 15.0, taperOut = 85.0;     // percent
        double seed = 1.0;                  // the element seed (gen::seedOf)
    };

    struct BrushStroke {
        // per centreline sample, rounded to float as Pachislop's Float32Arrays round them
        std::vector<float> x, y;            // layer px
        std::vector<float> tx, ty;          // unit tangent, the direction of travel
        std::vector<float> hw;              // half width (pressure, wobble and round caps)
        std::vector<float> len;             // arc length from the start
        double width = 0.0;                 // the full, untapered width
        double seed = 0.0;                  // the bristle noise's seed
        size_t size() const { return x.size(); }
    };

    // one vertex of the ribbon's triangle list (3 a triangle)
    struct BrushRibbonVertex {
        float x = 0.0f, y = 0.0f;           // layer px
        float u = 0.0f;                     // along the stroke, 0..1
        float across = 0.0f;                // px from the centreline, + to the left of travel
        float len = 0.0f, hw = 0.0f;        // the sample's arc length and half width
        float seed = 0.0f, width = 0.0f;    // the stroke's
    };

    struct BrushDroplet {
        float cx = 0.0f, cy = 0.0f;         // layer px
        float w = 0.0f, h = 0.0f;           // full size, w along `rotation`
        float rotation = 0.0f;              // radians counter-clockwise: the stroke's direction there
        float noise = 0.0f;                 // the outline noise's offset
        float u = 0.0f;                     // the stroke coordinate it is thrown at (1: flicked past the end)
    };

    // droplets at Splatter 100 %
    constexpr int BRUSH_MAX_SPLATS = 120;

    // the strokes of `p` (Pachislop buildStrokes)
    JASPERENGINE_API std::vector<BrushStroke> buildBrushStrokes(const BrushStrokeParams& p);

    // triangle strips along each stroke as a triangle list, `padPx` layer px wider than the ink on
    // each side for the anti-aliased rim (Pachislop ribbonGeometry), strokes in order
    JASPERENGINE_API std::vector<BrushRibbonVertex> brushRibbon(const std::vector<BrushStroke>& strokes, double padPx);

    // the droplets sprayed off the strokes' sides and flicked past their ends at `splatter`
    // percent, those thrown at u <= reveal (Pachislop drawSplatter)
    JASPERENGINE_API std::vector<BrushDroplet> brushSplatter(const std::vector<BrushStroke>& strokes, double splatter, double seed,
                                                             double reveal = 1.0);

    // the path's length at the stroke radius, layer px (the main stroke's; Pachislop's sample spacing)
    JASPERENGINE_API double brushPathLength(const BrushStrokeParams& p);

    // the furthest ink from the centre, layer px (Pachislop reach)
    JASPERENGINE_API double brushReach(const BrushStrokeParams& p, double splatter);

}
