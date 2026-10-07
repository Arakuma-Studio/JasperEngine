// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef GLYPHATLAS_H
#define GLYPHATLAS_H

#include "Export.h"

#include <vector>

namespace jasperengine {

    // Shelf packer over an R8 pixel buffer (moved out of STBText)
    class JASPERENGINE_API GlyphAtlas {
    public:
        GlyphAtlas(unsigned int width, unsigned int height, int padding);
        void reset(unsigned int width, unsigned int height);
        bool pack(int width, int height, int& x, int& y);      // best-height shelf fit
        void blit(const unsigned char* pixels, int width, int height, int x, int y);
        const std::vector<unsigned char>& getPixels() const;
        unsigned int getWidth() const; unsigned int getHeight() const;

    private:
        std::vector<unsigned char> pixels;
    };

}

#endif //GLYPHATLAS_H
