// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RMLFONTMSDF_H
#define RMLFONTMSDF_H

#include "Export.h"

#include <cstdint>
#include <vector>

namespace jasperengine {

    class FontFace;

    // One glyph of the HTML UI's distance-field atlas (S16 R16): a multi-channel signed distance
    // field with the true distance in alpha (MTSDF), made by msdfgen (engine/libs/msdfgen, MIT)
    // from the glyph's stb_truetype outline. Size-independent: drawn at any size, scale or 3D
    // transform; its distances also give the GPU text effects (outline, glow, blur, blurred
    // shadows) within distanceRange of the outline.
    struct JASPERENGINE_INTERNAL RmlDistanceGlyph {
        std::vector<uint8_t> rgba;          // MTSDF, rows top first, 0.5 on the (dilated) outline
        int width = 0, height = 0;          // texels
        // the bitmap's rectangle relative to the pen on the baseline, in font units, y up
        float left = 0.0f, bottom = 0.0f, right = 0.0f, top = 0.0f;
    };

    class JASPERENGINE_INTERNAL RmlFontMsdf {
    public:
        // atlas texels per em, and texels of distance each side of the outline: 0.3 em, what the
        // effects can reach (R16, R17)
        static constexpr float emTexels = 40.0f;
        static constexpr float distanceRange = 12.0f;
        static constexpr float reachEm = distanceRange / emTexels;

        // Safe on any thread (the face is immutable; msdfgen is pure). boldEm dilates the outline
        // by that share of the em (synthetic bold). False for a glyph without an outline (a space).
        static bool generate(const FontFace& face, uint32_t glyphIndex, float boldEm, RmlDistanceGlyph& out);
    };

}

#endif //RMLFONTMSDF_H
