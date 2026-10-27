// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_TEST_READBACK_H
#define HTMLUI_TEST_READBACK_H

#include "render/TextureRender.h"

#include <cstdint>
#include <vector>

namespace jasperengine { namespace test {

    // Synchronous GPU readback of an RGBA8 texture (a render target's color attachment),
    // width * height * 4 bytes, rows top first: offscreen targets are top-left origin on
    // every backend (the `orientation` golden checks it).
    bool readTexture(TextureRender& texture, int width, int height, std::vector<uint8_t>& rgba);
    // the same for a sampled RGBA8 (bytesPerPixel 4) or R8 (1) texture, e.g. a dynamic one:
    // width * height * bytesPerPixel bytes, rows top first, tightly packed
    bool readTexturePixels(TextureRender& texture, int width, int height, int bytesPerPixel, std::vector<uint8_t>& pixels);
    // level 0 of a 2D texture in its own format, bit for bit: width * height texels of
    // SokolTexture::getDataPixelFormat's bytesPerTexel (RGBA8, R8, R16, RGBA16F as IEEE halves,
    // RGBA32F, R32F, RG16F, R16F), rows in the order they were uploaded (or rendered: row 0 is a
    // render target's first row), tightly packed
    bool readTextureRaw(TextureRender& texture, int width, int height, ColorFormat format, std::vector<uint8_t>& bytes);

} }

#endif //HTMLUI_TEST_READBACK_H
