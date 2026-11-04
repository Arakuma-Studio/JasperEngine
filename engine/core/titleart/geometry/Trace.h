// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/text/trace.ts by SafEight, used with the author's permission.
//
// Contract (docs/specs/S80-title-art-core.md §6.1 Image `trace`, §6.6)
// - Purpose: an image's alpha (and a Vector layer's artwork) as an outline in layer space (y
//   up, centred, the larger side `size`, exactly where the image is drawn): S86's extrusion
//   input, hull / box plates of image layers and outlines() of pixel layers.
// - Steps: alpha resampled (area average) to at most 1024 samples on the longer side and 2 per
//   layer unit of `size`, never above the image's own pixels, padded by one transparent sample;
//   marching squares at `threshold` with linear interpolation along cell edges, saddles split
//   by the cell mean; outer loops counter-clockwise, holes clockwise; loops under `despeckle`
//   (% of size^2) dropped; closed Douglas-Peucker then Chaikin corner cutting that keeps real
//   corners (`smooth`, % of size); every choice by point order, so the result never depends on
//   where a walk started; every loop starts at its first vertex in (x, y) order.
// - Thread-safety: pure.

#pragma once

#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/geometry/Path.h"

namespace jasperengine::titleart::geometry {

    struct TraceParams {
        int threshold = 128;        // 1..255: alpha at or above it is inside
        double smooth = 0.0;        // % of size
        double despeckle = 0.0;     // % of size^2
    };
    // an image layer's `trace` object
    TraceParams decodeTrace(const Json& trace);

    // the half extents of an image fitted so its larger side is `size`
    TitleVec2 imageHalfExtents(int width, int height, double size);
    Path traceImage(const TitleImageData& image, double size, const TraceParams& trace);

}
