// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RmlFontEffects.h"

#include <algorithm>
#include <cstdint>

using namespace jasperengine;

size_t RmlFontEffects::effectId(const Rml::FontEffect& effect){
    const size_t fingerprint = effect.GetFingerprint();
    return fingerprint ? fingerprint : (size_t)(uintptr_t)&effect;
}

Rml::FontGlyph RmlFontEffects::wrapCoverage(const unsigned char* coverage, int width, int height, int xoff, int yoff, int advance){
    Rml::FontGlyph glyph;
    glyph.bearing = Rml::Vector2i(xoff, -yoff);
    glyph.advance = advance;
    const bool sized = width > 0 && height > 0;
    glyph.bitmap_data = sized ? coverage : nullptr;
    glyph.bitmap_dimensions = sized ? Rml::Vector2i(width, height) : Rml::Vector2i(0, 0);
    glyph.color_format = Rml::ColorFormat::A8;
    return glyph;
}

bool RmlFontEffects::generate(const Rml::FontEffect& effect, const Rml::FontGlyph& glyph, Bitmap& out){
    Rml::Vector2i origin(0, 0);
    Rml::Vector2i dimensions = glyph.bitmap_dimensions;
    if (!effect.GetGlyphMetrics(origin, dimensions, glyph))
        return false;
    if (dimensions.x <= 0 || dimensions.y <= 0)
        return false;

    out.width = dimensions.x;
    out.height = dimensions.y;
    // the box RmlUi's FontFaceLayer gives the glyph: effect origin plus bearing, y down
    out.xoff = origin.x + glyph.bearing.x;
    out.yoff = origin.y - glyph.bearing.y;
    // the effects write every pixel they cover, the rest stays transparent
    out.rgba.assign((size_t)dimensions.x * (size_t)dimensions.y * 4, 0);
    effect.GenerateGlyphTexture(out.rgba.data(), dimensions, dimensions.x * 4, glyph);

    return true;
}

bool RmlFontEffects::offset(const Rml::FontEffect& effect, const Rml::FontGlyph& glyph, Rml::Vector2i& origin){
    origin = Rml::Vector2i(0, 0);
    Rml::Vector2i dimensions = glyph.bitmap_dimensions;
    return effect.GetGlyphMetrics(origin, dimensions, glyph);
}

float RmlFontEffects::boldStrength(int size){
    return std::max(1.0f, (float)size / 24.0f);
}

void RmlFontEffects::embolden(std::vector<unsigned char>& coverage, int& width, int& height, int& yoff, int strength){
    if (strength <= 0 || width <= 0 || height <= 0 || coverage.size() < (size_t)width * (size_t)height)
        return;

    const int outWidth = width + strength;
    const int outHeight = height + strength;

    // separable max filter: right, then up. Input row y is output row y + strength.
    std::vector<unsigned char> wide((size_t)outWidth * (size_t)height, 0);
    for (int y = 0; y < height; y++){
        const unsigned char* src = &coverage[(size_t)y * (size_t)width];
        unsigned char* dst = &wide[(size_t)y * (size_t)outWidth];
        for (int x = 0; x < outWidth; x++){
            unsigned char value = 0;
            for (int k = std::max(0, x - width + 1); k <= std::min(strength, x); k++){
                value = std::max(value, src[x - k]);
            }
            dst[x] = value;
        }
    }

    std::vector<unsigned char> out((size_t)outWidth * (size_t)outHeight, 0);
    for (int y = 0; y < outHeight; y++){
        // output row y takes input rows y - strength .. y (each spreads up to `strength` rows above it)
        const int first = std::max(0, y - strength);
        const int last = std::min(height - 1, y);
        unsigned char* dst = &out[(size_t)y * (size_t)outWidth];
        for (int row = first; row <= last; row++){
            const unsigned char* src = &wide[(size_t)row * (size_t)outWidth];
            for (int x = 0; x < outWidth; x++){
                dst[x] = std::max(dst[x], src[x]);
            }
        }
    }

    coverage.swap(out);
    width = outWidth;
    height = outHeight;
    yoff -= strength;
}
