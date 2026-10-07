// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef GLYPHATLAS_H
#define GLYPHATLAS_H

#include "Export.h"

#include <vector>

namespace jasperengine {

    // Shelf packer over an R8 pixel buffer (moved out of STBText). Rectangles never move once
    // packed; a full atlas returns false and the owner decides (STBText grows and repacks,
    // the HTML UI font engine opens another page).
    class JASPERENGINE_API GlyphAtlas {
    public:
        GlyphAtlas(unsigned int width, unsigned int height, int padding);
        // empty atlas of that size, every pixel 0
        void reset(unsigned int width, unsigned int height);
        // best-height shelf fit; the padding is added right and below. A 0-sized rect packs
        // at (0, 0) and takes no space
        bool pack(int width, int height, int& x, int& y);
        // copies a width x height R8 bitmap to (x, y), clipped to the atlas
        void blit(const unsigned char* pixels, int width, int height, int x, int y);
        const std::vector<unsigned char>& getPixels() const;
        unsigned int getWidth() const; unsigned int getHeight() const;
        int getPadding() const;

    private:
        //a row of the atlas, shared by rects of similar height
        struct Shelf {
            int x;
            int y;
            int h;
        };

        std::vector<unsigned char> pixels;
        std::vector<Shelf> shelves;
        unsigned int width = 0;
        unsigned int height = 0;
        int padding = 0;
    };

}

#endif //GLYPHATLAS_H
