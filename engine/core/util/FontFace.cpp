// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FontFace.h"

using namespace jasperengine;

std::shared_ptr<FontFace> FontFace::loadFile(const std::string& path, int faceIndex){
    // HTMLUI_SKELETON: p0-font
    (void)path;
    (void)faceIndex;
    return nullptr;
}

std::shared_ptr<FontFace> FontFace::loadMemory(const unsigned char* data, size_t length, int faceIndex,
                                               bool copy, const std::string& label){
    // HTMLUI_SKELETON: p0-font
    (void)data;
    (void)length;
    (void)faceIndex;
    (void)copy;
    (void)label;
    return nullptr;
}

FontFace::~FontFace(){
    // HTMLUI_SKELETON: p0-font
}

const std::string& FontFace::getLabel() const{
    // HTMLUI_SKELETON: p0-font
    return label;
}

const std::string& FontFace::getFamilyName() const{
    // HTMLUI_SKELETON: p0-font
    return familyName;
}

int FontFace::getWeight() const{
    // HTMLUI_SKELETON: p0-font
    return 400;
}

bool FontFace::isItalic() const{
    // HTMLUI_SKELETON: p0-font
    return false;
}

int FontFace::getUnitsPerEm() const{
    // HTMLUI_SKELETON: p0-font
    return 0;
}

int FontFace::getAscender() const{
    // HTMLUI_SKELETON: p0-font
    return 0;
}

int FontFace::getDescender() const{
    // HTMLUI_SKELETON: p0-font
    return 0;
}

int FontFace::getLineGap() const{
    // HTMLUI_SKELETON: p0-font
    return 0;
}

int FontFace::getXHeight() const{
    // HTMLUI_SKELETON: p0-font
    return 0;
}

int FontFace::getUnderlinePosition() const{
    // HTMLUI_SKELETON: p0-font
    return 0;
}

int FontFace::getUnderlineThickness() const{
    // HTMLUI_SKELETON: p0-font
    return 0;
}

bool FontFace::hasPostTable() const{
    // HTMLUI_SKELETON: p0-font
    return false;
}

uint32_t FontFace::getGlyphIndex(uint32_t codepoint) const{
    // HTMLUI_SKELETON: p0-font
    (void)codepoint;
    return 0;
}

bool FontFace::hasCodepoint(uint32_t codepoint) const{
    // HTMLUI_SKELETON: p0-font
    (void)codepoint;
    return false;
}

float FontFace::scaleForPixelHeight(float pixels) const{
    // HTMLUI_SKELETON: p0-font
    (void)pixels;
    return 0.0f;
}

float FontFace::scaleForEmSize(float pixels) const{
    // HTMLUI_SKELETON: p0-font
    (void)pixels;
    return 0.0f;
}

bool FontFace::rasterizeGlyph(uint32_t glyphIndex, float scale, std::vector<unsigned char>& pixels,
                              int& width, int& height, int& xoff, int& yoff) const{
    // HTMLUI_SKELETON: p0-font
    (void)glyphIndex;
    (void)scale;
    pixels.clear();
    width = height = xoff = yoff = 0;
    return false;
}

float FontFace::getGlyphAdvance(uint32_t glyphIndex, float scale) const{
    // HTMLUI_SKELETON: p0-font
    (void)glyphIndex;
    (void)scale;
    return 0.0f;
}

const stbtt_fontinfo& FontFace::getInfo() const{
    // HTMLUI_SKELETON: p0-font
    return info;
}

hb_font_t* FontFace::getHbFont() const{
    // HTMLUI_SKELETON: p0-font
    return nullptr;
}
