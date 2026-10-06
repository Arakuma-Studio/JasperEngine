// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GlyphAtlas.h"

using namespace jasperengine;

GlyphAtlas::GlyphAtlas(unsigned int width, unsigned int height, int padding){
    // HTMLUI_SKELETON: p0-font
    (void)width;
    (void)height;
    (void)padding;
}

void GlyphAtlas::reset(unsigned int width, unsigned int height){
    // HTMLUI_SKELETON: p0-font
    (void)width;
    (void)height;
}

bool GlyphAtlas::pack(int width, int height, int& x, int& y){
    // HTMLUI_SKELETON: p0-font
    (void)width;
    (void)height;
    x = 0;
    y = 0;
    return false;
}

void GlyphAtlas::blit(const unsigned char* pixels, int width, int height, int x, int y){
    // HTMLUI_SKELETON: p0-font
    (void)pixels;
    (void)width;
    (void)height;
    (void)x;
    (void)y;
}

const std::vector<unsigned char>& GlyphAtlas::getPixels() const{
    // HTMLUI_SKELETON: p0-font
    return pixels;
}

unsigned int GlyphAtlas::getWidth() const{
    // HTMLUI_SKELETON: p0-font
    return 0;
}

unsigned int GlyphAtlas::getHeight() const{
    // HTMLUI_SKELETON: p0-font
    return 0;
}
