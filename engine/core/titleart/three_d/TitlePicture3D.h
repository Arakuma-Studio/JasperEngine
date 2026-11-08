// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R1a)
// - Purpose: a 3D layer's own picture (an Image layer's, an `artwork` Vector layer's: face fill
//   `source`), projected straight on in unsheared layer space. Ported from Pachislop
//   src/engine/three/materials.ts (faceImageTexture, edgeImageTexture, nearestInsideColours) by
//   SafEight, used with the author's permission.
//   - The face texture: the picture premultiplied (sRGB values), longest side at most 4096 px,
//     drawn over the face material by `face_image` x its alpha against the traced edge's alpha.
//   - The edge texture: opaque, every texel the straight colour of the nearest picture pixel
//     whose alpha reaches the trace threshold (exact Euclidean nearest site by the separable
//     lower envelope, Felzenszwalb and Huttenlocher), longest side at most 1024 px: the bevel and
//     side walls take it by `edge_image`.
//   - The box: the layer-space rectangle (y up) the textures cover.
// - Built on a job (makeTitlePicture3D is pure CPU work over the content provider).
// - Thread-safety: pure; the content provider must be safe to call from a job (TitleGeometry is).

#pragma once

#include "Export.h"
#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtDoc.h"

#include <memory>
#include <vector>

namespace jasperengine::titleart {

    class TitleContentProvider;

    struct TitlePicture3D {
        int width = 0, height = 0;
        std::vector<uint8_t> face;              // premultiplied sRGB RGBA, rows top first
        int edgeWidth = 0, edgeHeight = 0;
        std::vector<uint8_t> edge;              // opaque straight sRGB RGBA, rows top first
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // layer space, y up
        double edgeAlpha = 0.5;                 // the traced edge's alpha (trace threshold / 255)
    };

    // RGBA (alpha 255) where every pixel has the colour of the nearest pixel of `rgba` (straight,
    // w x h) whose alpha reaches `threshold`; all black when none does
    JASPERENGINE_API std::vector<uint8_t> nearestInsideColours(const std::vector<uint8_t>& rgba, int w, int h, int threshold);

    // the picture of an Image or artwork Vector layer at `pxPerUnit` (capped as above); null when
    // the layer has no pixels of its own
    JASPERENGINE_API std::shared_ptr<TitlePicture3D> makeTitlePicture3D(const Json& layer, const TitleArtDoc& doc, TitleContentProvider& content,
                                                                        const AssetResolver* assets, double pxPerUnit);

}
