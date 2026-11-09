// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_FONTPIXELREPORT_H
#define PIXELART_FONTPIXELREPORT_H

// The input of S42's `font_antialiasing` rule (S51 §8 "Feeds"): whether a font draws pixel art
// (a bitmap font without partial alpha) and, for a Text, whether its size is a whole multiple of
// the font's native size and which of its fallbacks are vector fonts. S42 turns these into WARN
// (a vector font, partial alpha, a size off the multiples, a vector fallback) and FAIL (a font
// that does not load) with their fixes.

#include "Export.h"
#include "pixelart/BitmapFont.h"

#include <string>
#include <vector>

namespace jasperengine::pixelart {

    struct JASPERENGINE_API FontPixelReport {
        FontKind kind = FontKind::VECTOR;
        bool loads = true;
        bool antialiased = true;            // a vector font always; a bitmap font with partial alpha
        float partialAlphaShare = 0.0f;     // bitmap fonts: texels 0 < a < 255 over opaque texels
        int nativeSize = 0;                 // bitmap: its native size; vector: the detected pixel grid (0 none)
        bool pixelGridDetected = false;     // vector fonts: R4's detection found a pixel size
        std::vector<FontError> errors;      // why it does not load
    };

    JASPERENGINE_API FontPixelReport analyzeFontForPixelArt(const std::string& path);

    struct JASPERENGINE_API TextPixelReport {
        FontPixelReport font;
        int pixelScale = 0;                 // R7's k, 0 for a vector font
        int effectiveFontSize = 0;
        bool sizeIsMultiple = true;         // fontSize is k * nativeSize
        std::vector<std::string> vectorFallbacks;   // skipped in a bitmap chain (R14)
    };

    // a Text's font list (the main font first) and its fontSize
    JASPERENGINE_API TextPixelReport analyzeTextForPixelArt(const std::vector<std::string>& fonts, unsigned int fontSize);

    // R7: the pixel scale of a font size over a native size
    inline int pixelScaleFor(unsigned int fontSize, int nativeSize){
        if (nativeSize <= 0)
            return 1;
        const int k = (int)(((double)fontSize / (double)nativeSize) + 0.5);
        return k < 1 ? 1 : k;
    }

}

#endif //PIXELART_FONTPIXELREPORT_H
