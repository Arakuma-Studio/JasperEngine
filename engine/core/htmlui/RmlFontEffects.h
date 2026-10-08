// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RMLFONTEFFECTS_H
#define RMLFONTEFFECTS_H

#include "Export.h"

#include <RmlUi/Core/FontEffect.h>
#include <RmlUi/Core/FontGlyph.h>
#include <RmlUi/Core/Types.h>

#include <cstddef>
#include <vector>

namespace jasperengine {

    // The glyph work of RmlFontEngine's font effects (font-effect: shadow / outline / glow /
    // blur, RmlUi's built-in effects) and of synthetic bold, on coverage bitmaps from
    // FontFace::rasterizeGlyph. Ported from RmlUi's FreeType backend (FontFaceLayer,
    // FontFaceHandleDefault) without FreeType: the effect objects RmlUi instanced from the
    // style sheet size and paint each glyph, RmlFontEngine packs the results into its
    // append-only pages.
    class JASPERENGINE_INTERNAL RmlFontEffects {
    public:
        // A prepared font-effect list (the FontEffectsHandle of PrepareFontEffects), in RmlUi's
        // order (its parser puts back-layer effects first). Holds the instances, so the ids,
        // which fall back to the instance address, stay unique while the set lives.
        struct Set {
            Rml::FontEffectList effects;
            std::vector<size_t> ids;   // effectId of each effect: what its glyphs are cached by
        };

        // one glyph of an effect that owns its texture: premultiplied RGBA8, rows of width * 4
        struct Bitmap {
            std::vector<unsigned char> rgba;
            int width = 0, height = 0;
            int xoff = 0, yoff = 0;   // top-left relative to the pen on the baseline, y down
        };

        // tan 12deg: the shear of synthetic italic (CSS font-synthesis), x += (baseline - y) * it
        static constexpr float italicShear = 0.21255656f;

        // The fingerprint RmlUi's font-effect parser gives every effect (type and every
        // property, colour included), or the instance address for an effect made without it
        static size_t effectId(const Rml::FontEffect& effect);

        // A coverage glyph as RmlUi's effects see it: bearing (x right, y up from the baseline),
        // A8 bitmap (not copied: `coverage` must outlive the returned glyph; null with a size
        // for the metrics-only call of `offset`)
        static Rml::FontGlyph wrapCoverage(const unsigned char* coverage, int width, int height, int xoff, int yoff, int advance);

        // Effects with their own texture (HasUniqueTexture: outline, glow, blur): the region from
        // GetGlyphMetrics, filled by GenerateGlyphTexture. False when the effect draws nothing
        // for the glyph (one without ink).
        static bool generate(const Rml::FontEffect& effect, const Rml::FontGlyph& glyph, Bitmap& out);
        // Effects drawn with the base glyphs (shadow): the offset of the glyph's quad, in
        // document pixels. False when the effect skips the glyph.
        static bool offset(const Rml::FontEffect& effect, const Rml::FontGlyph& glyph, Rml::Vector2i& origin);

        // Synthetic bold strength in document pixels for a font size: max(1, size / 24)
        static float boldStrength(int size);
        // Synthetic bold on a coverage bitmap: every pixel spreads `strength` pixels right and
        // up (the direction of FreeType's FT_Bitmap_Embolden, so the baseline and the left edge
        // stay), the bitmap grows by `strength` both ways and its top moves up (yoff)
        static void embolden(std::vector<unsigned char>& coverage, int& width, int& height, int& yoff, int strength);
    };

}

#endif //RMLFONTEFFECTS_H
