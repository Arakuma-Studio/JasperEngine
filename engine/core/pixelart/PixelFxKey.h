// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_PIXELFXKEY_H
#define PIXELART_PIXELFXKEY_H

// The CPU mirror of engine/shaderlib/includes/pixel_fx.glsl (docs/specs/S48-pixel-fx.md D4): the
// same hash and value noise, bit for bit on the integer parts, so dissolve embers (S49) spawn
// where the GPU burns and the editor's lints can predict an effect without rendering it.

#include "Export.h"

#include <cstdint>

namespace jasperengine::pixelart {

    // PCG2D of a texel (non-negative coordinates) and a seed: pfxHash
    JASPERENGINE_API uint32_t pfxHash(uint32_t x, uint32_t y, uint32_t seed);
    // the top 24 bits of the hash as [0, 1): pfxHash01
    JASPERENGINE_API float pfxHash01(uint32_t x, uint32_t y, uint32_t seed);
    // bilinear value noise in [0, 1) at a texel-space position: pfxValueNoise
    JASPERENGINE_API float pfxValueNoise(float x, float y, float cell, uint32_t seed);

}

#endif // PIXELART_PIXELFXKEY_H
