// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R20-R23, §6.6)
// - Purpose: the layout of a Text layer: shaping over its font, its Fallback Fonts and the
//   built-in face (TextShaper, per grapheme cluster), horizontal lines or vertical columns
//   (TTB shaping with vert/vrt2, tate-chu-yoko, proportional palt/vpal or fit-to-ink),
//   h_scale / v_scale, shear, per-line and per-character adjustments, and the frame (which
//   excludes per-character adjustments, so moving one glyph moves no other and warps use the
//   frame). One layout serves the renderer's geometry, S81's Glyph Atlas cells, S82's caret,
//   selection and per-glyph gizmo, S85's stagger units and S87's fit.
// - Faces: each FontFace once per document (AssetResolver::loadFont), and per (face,
//   `variations`) a HarfBuzz font of that face owned by this object ("sub-font": hb_font_create
//   on the face's hb_face_t, since a HarfBuzz sub-font would take its advances and outlines from
//   its parent's coordinates), so the shared FontFace stays immutable and a variable font's
//   advances match its outlines (GlyphOutlines.h draws with the same fonts).
// - Missing glyphs (R23): a cluster no face of the chain covers keeps the primary face's
//   .notdef advance (the layout does not jump) and is reported as `glyph_missing` with the code
//   points and the project fonts that cover them.
// - Output is in layer space: document units, y up, the layer's position at the origin (its
//   transform is the renderer's to apply).
// - Caching: by (font set, text, layout members) hash; cheap to call again with an unchanged layer.
// - Thread-safety: one instance per thread.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"
#include "titleart/geometry/Path.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct hb_font_t;

namespace jasperengine {
    class FontFace;
}

namespace jasperengine::titleart {

    class AssetResolver;

    struct LaidOutGlyph {
        uint32_t glyphIndex = 0;
        uint16_t face = 0;              // index into LaidOutText::faces
        TitleVec2 position;             // layer space, the glyph origin (y up), after every adjustment
        double scaleX = 1.0, scaleY = 1.0;  // layer units per font unit (size, line / char scale, stretch)
        double rotation = 0.0;          // deg clockwise about the em-cell centre (CharAdjust)
        bool upright = false;           // vertical: set upright (tate-chu-yoko, CJK) rather than rotated
        geometry::Affine transform;     // font units (y up) -> layer space: scale, stretch, per-character
                                        // rotation and offset, shear, frame centring
        geometry::Path outline;         // the glyph's outline in layer space (before warp, distort, weight)
    };

    // one grapheme cluster
    struct LaidOutCluster {
        uint32_t index = 0;             // first code point index in `text` (newlines counted; CharAdjust keys)
        std::vector<uint32_t> codepoints;
        uint32_t line = 0;              // line (column when vertical)
        uint16_t face = 0;
        std::vector<LaidOutGlyph> glyphs;
        TitleVec2 pen;                  // pen position (baseline origin; vertical: the cell's top centre),
                                        // layer space, without the character's own adjustments (the caret)
        double advance = 0.0;           // along the line (vertical: down the column), layer units
        TitleRect inkBox;               // layer space, with the character's adjustments
        TitleRect emCell;               // layer space, with its offset and scale (rotation is about its centre)
        bool missing = false;           // no face of the chain covers it (drawn as .notdef)
    };

    struct LaidOutLine {
        uint32_t firstCluster = 0, clusterCount = 0;
        TitleRect box;                  // layer space
    };

    struct LaidOutText {
        std::vector<std::shared_ptr<FontFace>> faces;   // the chain: font, fallback fonts, built-in
        std::vector<LaidOutCluster> clusters;
        std::vector<LaidOutLine> lines;
        TitleRect frame;                // layer space, without per-character adjustments
        TitleRect inkBounds;            // layer space, every glyph's ink with its adjustments
        bool vertical = false;
        std::vector<TitleProblem> problems;     // glyph_missing, variation_axis_unknown, asset_missing
        uint64_t hash = 0;                      // of the inputs (fonts, text, layout members)
    };

    class JASPERENGINE_API TitleTextLayout {
    public:
        explicit TitleTextLayout(const AssetResolver* assets);
        ~TitleTextLayout();
        TitleTextLayout(const TitleTextLayout&) = delete;
        TitleTextLayout& operator=(const TitleTextLayout&) = delete;

        // the layout of a Text layer (a normalized layer object), with `overrides` of that layer
        // applied first (FieldOverride::layerId names it); the reference stays valid until the
        // next call or clear()
        const LaidOutText& layout(const Json& layer, const std::vector<FieldOverride>& overrides = {});

        // the HarfBuzz sub-font of a face with a layer's `variations` (shared with GlyphOutlines);
        // `face` is one of a layout's LaidOutText::faces (null for any other face)
        ::hb_font_t* subFont(const FontFace& face, const Json& variations);

        // drops every cached layout and sub-font (a font file changed)
        void clear();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}
