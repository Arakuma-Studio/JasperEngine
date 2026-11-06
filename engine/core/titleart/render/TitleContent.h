// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.6, §6.7 passes 1-2, R25, R26)
// - Purpose: what the renderer's 2D title passes need from text and geometry, as one seam: the
//   elements of a Text, Shape, Vector or Image layer (outlines after warp -> distort -> weight,
//   rims with their plates), each element's R8 coverage at a mask frame (ThorVG fill and mitred
//   stroke, fill_counters), the layer's own pixels at a frame (image layers, `artwork` vectors),
//   the glyph boxes of `line` / `char` gradient spans, and a shape mask's outline.
// - The renderer holds one provider (TitleArtRenderer::setContentProvider, or the process
//   default of setDefaultTitleContentProvider); geometry::TitleGeometry and TitleTextLayout are
//   what the module's own provider wraps. A layer of those types with no provider draws nothing
//   and reports `geometry_unavailable`. Tests install providers that hand out fixed elements and
//   masks, so the passes are tested on exact inputs.
// - Coordinates: layer space (document units, y up, the layer's position at the origin) for
//   elements and boxes; a MaskFrame maps it to mask pixels, rows top first (CoverageMask and
//   pictures), which the renderer reverses into its y-up element frames on upload.
// - Thread-safety: called from the render job's CPU steps on the main thread; an implementation
//   may cache by the layer's content and must return equal outputs for equal inputs (R17).

#pragma once

#include "Export.h"
#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/geometry/Path.h"
#include "titleart/geometry/TitleGeometry.h"

#include <functional>
#include <memory>
#include <vector>

namespace jasperengine::titleart {

    // which elements of a layer one render draws (a glyph cell of S81's Glyph Atlas)
    struct TitleElementFilter {
        int cluster = -1;           // >= 0: only this grapheme cluster's outlines (TitleTextLayout's index)
    };

    // the glyph boxes `line` / `char` gradient spans read (layer space)
    struct TitleSpans {
        std::vector<TitleRect> chars;   // per drawn glyph, its ink box
        std::vector<TitleRect> lines;   // per drawn glyph, its line's box (same length as chars)
    };

    class JASPERENGINE_API TitleContentProvider {
    public:
        virtual ~TitleContentProvider() = default;

        // the elements of a Text, Shape, Vector or Image layer (faces first, then enabled rims
        // innermost first) and the problems found making them
        virtual geometry::LayerElements elements(const Json& layer, const TitleArtDoc& doc, const AssetResolver* assets,
                                                 const TitleElementFilter& filter) = 0;
        // one element's coverage at `frame` (rows top first), fill_counters applied when it asks;
        // the element's `offset` is the renderer's (it shifts the frame), not drawn here
        virtual geometry::CoverageMask coverage(const geometry::TitleElement& element, const geometry::MaskFrame& frame) = 0;
        // the layer's own straight-alpha sRGB pixels at `frame` (rows top first): image layers,
        // `artwork` vector layers; false when the layer has none
        virtual bool picture(const Json& layer, const TitleArtDoc& doc, const AssetResolver* assets, const geometry::MaskFrame& frame,
                             TitleImageData& out) = 0;
        // the glyph boxes of a text layer's spans; false when it has none (shapes, images)
        virtual bool spans(const Json& layer, const TitleArtDoc& doc, const AssetResolver* assets, const TitleElementFilter& filter,
                           TitleSpans& out) = 0;
        // a shape mask's outline (its `shape` object: the shape layer's geometry members), layer units
        virtual geometry::Path shapeOutline(const Json& shape) = 0;
    };

    using TitleContentFactory = std::function<std::shared_ptr<TitleContentProvider>()>;
    // the provider every renderer made after this call starts with (none until set)
    JASPERENGINE_API void setDefaultTitleContentProvider(TitleContentFactory factory);
    JASPERENGINE_API std::shared_ptr<TitleContentProvider> makeDefaultTitleContentProvider();

    // An image's straight pixels resampled onto a mask frame (bilinear, rows top first), the
    // way an image layer's picture and coverage are drawn: `bounds` is where the image lies in
    // layer space. Providers use it for image layers.
    JASPERENGINE_API void drawImageToFrame(const TitleImageData& image, const TitleRect& bounds, const geometry::MaskFrame& frame,
                                           TitleImageData& out);

}
