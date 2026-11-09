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
    // the noise and its gradient per texel: pfxValueNoiseGrad
    JASPERENGINE_API float pfxValueNoiseGrad(float x, float y, float cell, uint32_t seed, float& gradX, float& gradY);

    // library:sprite's DISSOLVE values (the material's dissolve* properties)
    struct JASPERENGINE_API PfxDissolve {
        float dirX = 0.0f, dirY = 1.0f;  // dissolveDirection: texels, y up; (0, 0) noise only
        float noise = 0.35f;             // dissolveNoise
        float cell = 4.0f;               // dissolveNoiseScale
        float jitter = 0.08f;            // dissolveJitter
        float edgeWidth = 2.0f;          // dissolveEdgeWidth (whole texels)
        float scorchWidth = 3.0f;        // dissolveScorchWidth
        uint32_t seed = 0;               // dissolveSeed
    };

    // pfxDissolveKey: the key in [0, 1) of a texel inside a frame (x right, y up, from 0)
    JASPERENGINE_API float pfxDissolveKey(int x, int y, int frameWidth, int frameHeight, const PfxDissolve& dissolve);
    // pfxDissolveFront: progress 0 below every key and its bands, 1 above every key
    JASPERENGINE_API float pfxDissolveFront(float progress, float span);
    // What the shader does with a texel at a progress (pfxDissolveDistance, edge rounded to whole
    // texels as library:sprite does): negative once burnt, [0, edge) in the edge band, [edge, edge +
    // scorch) scorched, above untouched. Progress <= 0 is always untouched, >= 1 always burnt.
    JASPERENGINE_API float pfxDissolveTexel(int x, int y, int frameWidth, int frameHeight, const PfxDissolve& dissolve, float progress);
    // The front a frame's keys (pfxDissolveKey) burn against at a progress: a texel has burnt
    // when its key is below it, as library:sprite's fxBurnt. Below every key at progress <= 0,
    // above every key at progress >= 1. One value per frame and progress (the dissolve embers
    // compare cached keys with it).
    JASPERENGINE_API float pfxDissolveBurnFront(int frameWidth, int frameHeight, const PfxDissolve& dissolve, float progress);

}

#endif // PIXELART_PIXELFXKEY_H
