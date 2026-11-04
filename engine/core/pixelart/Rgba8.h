// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_RGBA8_H
#define PIXELART_RGBA8_H

#include <compare>
#include <cstdint>

namespace jasperengine::pixelart {

    // The pixel-art family's one 8-bit colour: exact sRGB-encoded bytes, straight alpha. It is
    // also PixelImage's pixel (S44), so its content is fixed by the specs (S43 §6.4).
    struct Rgba8 { uint8_t r = 0, g = 0, b = 0, a = 255; auto operator<=>(const Rgba8&) const = default; };

}

#endif // PIXELART_RGBA8_H
