// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextShaper.h"

#include "FontFace.h"

using namespace jasperengine;

unsigned int TextShaper::selectFace(const std::vector<const FontFace*>& faces, uint32_t codepoint, unsigned int current){
    // HTMLUI_SKELETON: p0-font
    (void)faces;
    (void)codepoint;
    return current;
}

void TextShaper::shapeRange(const std::vector<const FontFace*>& faces, const std::vector<float>& scales,
                            const std::vector<uint32_t>& codepoints, size_t offset, size_t length,
                            unsigned int line, const ShapeOptions& options, std::vector<ShapedGlyph>& out){
    // HTMLUI_SKELETON: p0-font
    (void)faces;
    (void)scales;
    (void)codepoints;
    (void)offset;
    (void)length;
    (void)line;
    (void)options;
    (void)out;
}

float TextShaper::measure(const std::vector<const FontFace*>& faces, const std::vector<float>& scales,
                          const std::vector<uint32_t>& codepoints, size_t offset, size_t length,
                          const ShapeOptions& options){
    // HTMLUI_SKELETON: p0-font
    (void)faces;
    (void)scales;
    (void)codepoints;
    (void)offset;
    (void)length;
    (void)options;
    return 0.0f;
}

void TextShaper::decodeUtf8(const char* text, size_t length, std::vector<uint32_t>& codepoints){
    // HTMLUI_SKELETON: p0-font
    (void)text;
    (void)length;
    codepoints.clear();
}
