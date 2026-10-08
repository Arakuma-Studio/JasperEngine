// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LunaSVGRuntime.h"

#include "DefaultFont.h"
#include "DefaultFontArabic.h"
#include "Log.h"

#include <lunasvg.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

using namespace jasperengine;

namespace {

    bool fontsRegistered = false;

    // The generic families, and the concrete names LunaSVG substitutes for them (or for an
    // empty font-family) when the family itself is unknown, all on the built-in Roboto
    const char* const robotoFamilies[] = {
        "sans-serif", "serif", "monospace", "system-ui", "cursive", "fantasy", "Roboto",
        "Arial", "Helvetica", "Times New Roman", "Courier New", "Comic Sans MS", "Impact",
        "DejaVu Sans", "DejaVu Serif", "DejaVu Sans Mono",
    };

}

void LunaSVGRuntime::ensureFonts(){
    if (fontsRegistered)
        return;
    fontsRegistered = true;

    // the font arrays are static: no destroy callback, LunaSVG borrows them
    for (const char* family : robotoFamilies){
        if (!lunasvg_add_font_face_from_data(family, false, false, roboto_regular_ttf, roboto_regular_ttf_len, nullptr, nullptr)){
            Log::warn("[HTML UI] LunaSVG could not register the built-in font as '%s'", family);
        }
    }
    if (!lunasvg_add_font_face_from_data("Noto Sans Arabic", false, false, noto_sans_arabic_ttf, noto_sans_arabic_ttf_len, nullptr, nullptr)){
        Log::warn("[HTML UI] LunaSVG could not register the built-in Arabic font");
    }
}

bool LunaSVGRuntime::rasterize(const char* data, size_t length, float scale, bool premultiplied,
                               std::vector<uint8_t>& pixels, int& width, int& height, float maxDimension){
    if (!data || length == 0)
        return false;

    ensureFonts();

    std::unique_ptr<lunasvg::Document> document = lunasvg::Document::loadFromData(data, length);
    if (!document || !(document->width() > 0.0f) || !(document->height() > 0.0f)){
        Log::error("SVG could not be parsed, or has no size");
        return false;
    }

    // A non-positive or non-finite scale falls back to the SVG's intrinsic size
    if (!(scale > 0.0f) || !std::isfinite(scale)){
        scale = 1.0f;
    }

    // Rounded up, as nanosvg-era textures were; the drawing keeps the exact scale, so the
    // partial last row/column stays transparent. The cap also keeps the int casts in range.
    const float scaledWidth = std::ceil(document->width() * scale);
    const float scaledHeight = std::ceil(document->height() * scale);
    if (scaledWidth > maxDimension || scaledHeight > maxDimension){
        Log::error("SVG rasterization size too large (%.0f x %.0f) for scale %.2f", scaledWidth, scaledHeight, scale);
        return false;
    }

    width = std::max(1, static_cast<int>(scaledWidth));
    height = std::max(1, static_cast<int>(scaledHeight));

    lunasvg::Bitmap bitmap(width, height);
    if (!bitmap.valid()){
        Log::error("SVG rasterization: could not allocate %i x %i pixels", width, height);
        return false;
    }
    bitmap.clear(0x00000000);
    document->render(bitmap, lunasvg::Matrix::scaled(scale, scale));

    // LunaSVG gives premultiplied ARGB32 (B, G, R, A bytes on little endian)
    if (!premultiplied){
        bitmap.convertToRGBA();   // straight-alpha R, G, B, A bytes
    }

    const size_t rowBytes = static_cast<size_t>(width) * 4;
    pixels.resize(rowBytes * static_cast<size_t>(height));
    const uint8_t* source = bitmap.data();
    for (int y = 0; y < height; y++){
        const uint8_t* row = source + static_cast<size_t>(y) * static_cast<size_t>(bitmap.stride());
        uint8_t* out = pixels.data() + static_cast<size_t>(y) * rowBytes;
        if (premultiplied){
            for (size_t x = 0; x < rowBytes; x += 4){
                out[x + 0] = row[x + 2];
                out[x + 1] = row[x + 1];
                out[x + 2] = row[x + 0];
                out[x + 3] = row[x + 3];
            }
        }else{
            memcpy(out, row, rowBytes);
        }
    }

    return true;
}
