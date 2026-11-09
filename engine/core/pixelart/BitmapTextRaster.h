// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_BITMAPTEXTRASTER_H
#define PIXELART_BITMAPTEXTRASTER_H

// Bitmap text drawn into an RGBA8 image on the CPU (S51 D4: editor previews, view_asset, S48's
// contact-sheet labels): the layout of BitmapText (R13), each font pixel scale x scale image
// pixels, no filtering. The image's top-left is (0, 0), y down.

#include "Export.h"
#include "pixelart/BitmapFont.h"
#include "pixelart/BitmapTextCache.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::pixelart {

    struct JASPERENGINE_API RasterImage {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;

        RasterImage() = default;
        RasterImage(int width, int height);
        void fill(uint32_t color);
        // a two-tone checker of `cell` pixels
        void checker(uint32_t a, uint32_t b, int cell);
        // straight-alpha "over" of one pixel (outside the image: nothing)
        void blend(int x, int y, const uint8_t* rgba);
    };

    // RGBA bytes, r in the low byte
    inline constexpr uint32_t rgbaColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255){
        return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
    }

    // the text with its first line's top at (x, y); a monochrome font takes `color`, a coloured one
    // is multiplied by it (white: as drawn)
    JASPERENGINE_API void rasterBitmapText(const BitmapFont& font, const std::string& text, int scale, uint32_t color, RasterImage& image, int x, int y);
    // the same with R10's pixel outline and shadow under the glyphs (the cache's composition, at
    // font pixels); (x, y) is where the unstyled text's top-left would be
    JASPERENGINE_API void rasterStyledBitmapText(const BitmapFont& font, const std::string& text, int scale, uint32_t color,
                                                 const BitmapTextStyle& style, RasterImage& image, int x, int y);

}

#endif //PIXELART_BITMAPTEXTRASTER_H
