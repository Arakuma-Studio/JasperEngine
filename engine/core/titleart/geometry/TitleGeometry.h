// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R24-R26, §6.6, §6.7 passes 1-2)
// - Purpose: the interface between text and geometry (outlines, rims, plates, coverage masks)
//   and the renderer's 2D title passes. Geometry turns a layer into its elements: the face
//   groups and every enabled rim, each with its final outlines (after warp -> distort -> weight,
//   rims' plates applied) and the stroke the mask adds; the renderer rasterizes them into R8
//   coverage masks and shades them (distance fields, bevels, materials) outermost first.
// - Elements: rim k's outline is the face outline grown by the sum of the widths of rims 0..k
//   (innermost first; `grow` is that cumulative width in layer units). A rim's mask is the fill
//   of its outlines plus a stroke of width 2 x grow with the rim's join and miter limit 4;
//   `fill_counters` floods holes from the border (every pixel the flood does not reach is
//   opaque). Per-character faces (CharAdjust.face) split the face into one element per material
//   while rims stay continuous. Images and `artwork` SVGs carry no outlines: their rims grow from
//   the alpha's distance field (round joins), `fromAlpha`.
// - Queries (S81's Glyph Atlas, S82's canvas picking, S86's 3D input, S87's fit and SVG export):
//   outlines per glyph, rim bands (inside the outline = Face, within rim k's cumulative band =
//   Rim k+1), content bounds and reach bounds (content plus the effects' reach: §6.3's ROI rule).
//   All are pure CPU functions of a layer snapshot, cached by its geometry hash.
// - Thread-safety: TitleGeometry instances are not shared between threads; the free functions
//   are pure.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"
#include "titleart/geometry/Path.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::titleart {
    class AssetResolver;
}

namespace jasperengine::titleart::geometry {

    enum class ElementKind : uint8_t { Face, Rim };
    enum class StrokeJoin : uint8_t { Miter, Round, Bevel };

    struct TitleElement {
        ElementKind kind = ElementKind::Face;
        int rimIndex = -1;                  // Rim: 0 = innermost
        std::string rimId;
        int faceGroup = 0;                  // Face: 0 = the layer's face, n = the nth CharAdjust.face material
        std::vector<Path> outlines;         // layer space, y up (fills; rims include their plates)
        double grow = 0.0;                  // Rim: cumulative width in layer units (the stroke is 2 x grow)
        StrokeJoin join = StrokeJoin::Miter;
        double miterLimit = 4.0;
        bool fillCounters = false;
        TitleVec2 offset;                   // Rim offset in layer units (+y up)
        const Json* material = nullptr;     // into the layer snapshot
    };

    struct LayerElements {
        std::vector<TitleElement> elements;     // faces first, then rims innermost first; drawn outermost first
        TitleRect baseBounds;                   // layer space: every element's outline bounds
        double unit = 1.0;                      // layer units per `%` point (source size / 100)
        bool fromAlpha = false;                 // image / artwork layers: rims from the alpha
        std::vector<TitleProblem> problems;     // glyph_missing, svg_feature_ignored ...
    };

    // the pixels a mask covers: mask px per layer unit and the layer point at pixel (0, 0)'s corner
    struct MaskFrame {
        int width = 0, height = 0;
        double scale = 1.0;
        TitleVec2 origin;                       // layer space, the top-left corner (y up: the largest y)
    };

    struct CoverageMask {
        int width = 0, height = 0;
        std::vector<uint8_t> pixels;            // R8, rows top first
    };

    // the final outline paths of a glyph (or a shape / SVG contour group) after warp, distort, weight
    struct GlyphOutline {
        uint32_t cluster = 0;                   // LaidOutText cluster index (shapes and SVGs: 0)
        Path path;
    };

    class JASPERENGINE_API TitleGeometry {
    public:
        explicit TitleGeometry(const AssetResolver* assets);
        ~TitleGeometry();
        TitleGeometry(const TitleGeometry&) = delete;
        TitleGeometry& operator=(const TitleGeometry&) = delete;

        // the elements of a Text, Shape, Vector or Image layer (the renderer's pass 1)
        LayerElements elements(const Json& layer, const TitleArtDoc& doc);
        // final outlines per glyph, layer space
        std::vector<GlyphOutline> outlines(const Json& layer, const TitleArtDoc& doc);
        // the cumulative grow of each enabled rim, layer units, innermost first
        std::vector<double> rimBands(const Json& layer);
        // document space: the layer's content (outlines after its transform, image and SVG quads,
        // a generator's bounds; a group the union of its children's)
        TitleRect contentBounds(const Json& layer, const TitleArtDoc& doc);
        // content bounds grown by the effects' reach x px_scale (part effects on their part's
        // bounds) and 1.5 x the feather of its masks
        TitleRect reachBounds(const Json& layer, const TitleArtDoc& doc, double pxScale);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    // ThorVG coverage of one element at `frame` (non-zero fill, plus the rim stroke)
    JASPERENGINE_API CoverageMask rasterizeElement(const TitleElement& element, const MaskFrame& frame);
    // fill_counters: every pixel a 4-connected flood from the border through alpha < 128 does not
    // reach becomes opaque
    JASPERENGINE_API void fillCounters(CoverageMask& mask);

}
