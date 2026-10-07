// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GlyphAtlas.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine;

GlyphAtlas::GlyphAtlas(unsigned int width, unsigned int height, int padding){
    this->padding = std::max(0, padding);
    reset(width, height);
}

void GlyphAtlas::reset(unsigned int width, unsigned int height){
    this->width = width;
    this->height = height;

    pixels.assign((size_t)width * (size_t)height, 0);
    shelves.clear();
}

bool GlyphAtlas::pack(int width, int height, int& x, int& y){
    x = 0;
    y = 0;

    if (width <= 0 || height <= 0){
        return true;
    }

    width += padding;
    height += padding;

    //shortest shelf that still fits, to not waste the taller ones
    Shelf* best = nullptr;
    for (Shelf& shelf : shelves){
        if (shelf.h >= height && (shelf.x + width) <= (int)this->width){
            if (!best || shelf.h < best->h)
                best = &shelf;
        }
    }

    if (best){
        x = best->x;
        y = best->y;
        best->x += width;

        return true;
    }

    int shelfY = 0;
    if (!shelves.empty()){
        shelfY = shelves.back().y + shelves.back().h;
    }

    if (width > (int)this->width || (shelfY + height) > (int)this->height){
        return false;
    }

    shelves.push_back({width, shelfY, height});

    y = shelfY;

    return true;
}

void GlyphAtlas::blit(const unsigned char* source, int width, int height, int x, int y){
    if (!source || width <= 0 || height <= 0 || x < 0 || y < 0)
        return;
    if (x >= (int)this->width || y >= (int)this->height)
        return;

    int copyWidth = std::min(width, (int)this->width - x);
    int copyHeight = std::min(height, (int)this->height - y);

    for (int row = 0; row < copyHeight; row++){
        std::memcpy(&pixels[(size_t)(y + row) * this->width + (size_t)x], source + (size_t)row * (size_t)width, (size_t)copyWidth);
    }
}

const std::vector<unsigned char>& GlyphAtlas::getPixels() const{
    return pixels;
}

unsigned int GlyphAtlas::getWidth() const{
    return width;
}

unsigned int GlyphAtlas::getHeight() const{
    return height;
}

int GlyphAtlas::getPadding() const{
    return padding;
}
