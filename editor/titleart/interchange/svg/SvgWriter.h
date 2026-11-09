// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R41, D7)
// - Purpose: the SVG of a title's vector-representable content. One file per language: the
//   caller resolves the document for its language first (resolveTitleLanguage, fit) and passes
//   the export frame.
// - Space: `viewBox` is the export frame in canvas pixels (image space, y down: document y-up
//   converted); `width`/`height` are the frame in px x `scale`. Every outline is written with its
//   layer's and groups' transforms baked into the coordinates, so the file needs no transform to
//   register with a PNG export of the same frame.
// - Content: text, shape and `shape`-mode Vector layers become <path> outlines after warp,
//   distort and weight (TitleGeometry elements). Face fills: solid; linear and radial
//   userSpaceOnUse gradients (stop alpha as stop-opacity, a midpoint as an extra stop at the
//   midpoint colour, the fill's scale and offset applied); reflected as a linear gradient with
//   its stops mirrored about the centre (exact also beyond the box, where `spreadMethod="reflect"`
//   would repeat); the other kinds as their first stop's colour with a warning. Rims are copies
//   of their outline under the face, outermost first: filled and stroked in the rim's paint,
//   stroke 2 x the cumulative width, the rim's join, miter limit 4, moved by the rim's offset;
//   `fill_counters` rims keep only their outer contours; hull and box plates are their own
//   outlines. Shape and gradient masks become <mask> (density and invert kept; feather is drawn
//   hard, with a warning); clipping to a vector base becomes a <clipPath> (a <mask> when the base
//   has rims or strokes, which a clip path cannot carry). Each layer is a <g> with its name as
//   `id` (slugged, unique), its opacity and its blend as CSS mix-blend-mode.
// - Left out, each with a warning naming the layer: effects, bevels, textures, patterns and the
//   other material layers (reflection, satin, gloss ...), generators, adjustment layers, image
//   layers, `artwork` Vector layers, layer and image masks, 3D layers. `embedRaster` embeds the
//   non-vector layers as registered PNG <image>s from the caller's render; `strict` fails with the
//   list instead.
// - Deterministic: the same document and frame give the same bytes.
// - Thread-safety: pure (the raster callback is the caller's).

#pragma once

#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct TitleSvgOptions {
        double scale = 1.0;             // width / height in px per canvas px
        bool embedRaster = false;       // non-vector layers as registered PNG <image>s
        bool strict = false;            // anything left out fails the export
    };

    // a non-vector layer rendered alone (with its effects and masks): PNG bytes and the canvas
    // pixels it covers (image space)
    struct TitleSvgRaster {
        std::vector<uint8_t> png;
        titleart::TitleRectI frame;
    };
    using TitleSvgRasterFn = std::function<bool(const titleart::Json& layer, TitleSvgRaster& out)>;

    struct TitleSvgResult {
        bool ok = false;
        std::string code;                       // svg_unrepresentable (strict), bad_frame
        std::string error;
        std::string text;
        std::vector<std::string> warnings;      // "Glow: outer_glow effect left out" ...
        std::vector<std::string> leftOut;       // the subset strict refuses (layer: item)
        size_t layers = 0;                      // layer groups written
    };

    // the layers `embedRaster` asks the raster callback for (visible, drawn, without an SVG form:
    // image, artwork vector, generator and 3D layers), in document order: an export renders them
    // before writing
    std::vector<std::string> titleSvgRasterLayers(const titleart::TitleArtDoc& doc);

    // `frame`: canvas pixels (image space) the SVG covers; `assets` resolves fonts and SVGs
    TitleSvgResult writeTitleSvg(const titleart::TitleArtDoc& doc, const titleart::TitleRectI& frame, const TitleSvgOptions& options,
                                 const titleart::AssetResolver* assets, const TitleSvgRasterFn& raster = {});

    // export_title's `svg` format (TitleSvgExport.cpp), from registerTitleExportFormats
    void registerTitleSvgExport();

}
