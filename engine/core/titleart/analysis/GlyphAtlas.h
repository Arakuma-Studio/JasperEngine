// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/generators/rings/magicCircle.ts (glyphAtlas) by SafEight, used with
// the author's permission.

// Contract (docs/specs/S83-title-effects-generators.md §6.7.6, risk 8)
// - Purpose: Magic Circle's glyph atlas on the CPU: the characters of a glyph set drawn white,
//   one square cell of `cell` px each (at most 8 a row, the em at 84 % of the cell, centred on
//   its advance and on the font's ascender-to-descender height, Pachislop's layout), from the
//   font's outlines (text/GlyphOutlines.h) rasterized by ThorVG with S80's mask rasterizer
//   (geometry::rasterizeElement, non-zero fill), so the GPU samples coverage instead of the
//   canvas text Pachislop drew.
// - Fonts: resolveGlyphFont picks the face: the `font` asset when one is set, else the set's
//   library font (S84 copies the pack fonts into the project at assets/TitleArt/Fonts/<dir>/),
//   else the resolver's built-in face (the engine's built-in Roboto when it has none). A
//   character the face lacks is drawn as its .notdef glyph and listed in `missing`, which the
//   generator reports as S80's glyph_missing problem.
// - Layout of `coverage`: R8, (cols x cell) by (rows x cell), rows top first; character i sits in
//   column i % cols, row i / cols.
// - Thread-safety: main thread (the shared FontFace's HarfBuzz font is not thread-safe).

#pragma once

#include "Export.h"
#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtDoc.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine {
    class FontFace;
}

namespace jasperengine::titleart::analysis {

    constexpr int GLYPH_ATLAS_COLUMNS = 8;
    // Pachislop's largest set is 42 characters; a custom string keeps its first 64 (8 rows of 8,
    // so a 512 px cell stays within a 4096 px texture)
    constexpr int GLYPH_ATLAS_MAX_GLYPHS = 64;

    struct GlyphAtlas {
        int cols = 0, rows = 0, count = 0, cell = 0;
        std::vector<uint8_t> coverage;          // R8, cols * cell by rows * cell, rows top first
        std::vector<uint32_t> missing;          // code points the face has no glyph for (each once)
        bool empty() const { return count == 0; }
    };

    struct GlyphFont {
        std::shared_ptr<FontFace> face;
        std::string key;                        // identifies the face's file and revision (cache keys)
        std::string name;                       // for problems: the asset path, or "the built-in face"
        // the `font` asset could not be had (asset_missing); the face is the built-in one then
        std::string error;
    };

    // the code points of UTF-8 text (an invalid byte reads as U+FFFD)
    JASPERENGINE_API std::vector<uint32_t> utf8Codepoints(std::string_view text);
    // one code point as UTF-8
    JASPERENGINE_API std::string utf8Char(uint32_t codepoint);

    // the face a glyph set draws with: `font` when it is set, else `library` when the resolver
    // finds it, else the built-in face
    JASPERENGINE_API GlyphFont resolveGlyphFont(const AssetResolver* assets, const TitleAssetRef& font, const TitleAssetRef& library);

    // the atlas of `codepoints` (the first GLYPH_ATLAS_MAX_GLYPHS) in `face` at `cell` px a cell
    JASPERENGINE_API GlyphAtlas buildGlyphAtlas(const FontFace& face, const std::vector<uint32_t>& codepoints, int cell);

}
