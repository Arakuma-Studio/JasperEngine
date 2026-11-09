// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The one mip recipe (spec S14 D6): the texture cook and the raw load path both build their
// levels here, so an uncompressed cook and a raw load of the same file give the same bytes.
// Filtering is stb_image_resize2's: sRGB colour in linear light with alpha-weighted RGB, linear
// data per channel, HDR in float; normal maps are renormalized on every level, alpha coverage
// can be preserved and alpha borders bled on every level.

#ifndef MIPCHAIN_H
#define MIPCHAIN_H

#include "Export.h"

#include <cstdint>
#include <vector>

namespace jasperengine {

    // Nearest (appended, S42 R5): no blending, the source texel under each destination texel's
    // centre; pixel art keeps its pixels when it is resized or mipmapped
    enum class MipFilter : uint8_t { Box, Triangle, Mitchell, CatmullRom, Nearest };
    enum class MipPixelType : uint8_t { U8, U16, F32 };

    // one level: rows top first, tightly packed, `channels` values of the chain's type per texel
    struct MipLevel {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;
    };

    struct MipChainOptions {
        int channels = 4;
        MipPixelType type = MipPixelType::U8;
        // U8 colour stored sRGB-encoded (channels 1-3; with 4 the alpha stays linear)
        bool srgb = false;
        // RGB weighted by alpha while filtering (4 channels): transparent texels lend no colour
        bool alphaWeighted = false;
        // F32 XYZ unit vectors (3 channels), renormalized on every level
        bool normalMap = false;
        // edges wrap (tiling textures) or clamp (sprites, UI, skies)
        bool wrap = true;
        MipFilter filter = MipFilter::Mitchell;
        // every level keeps level 0's alpha-test coverage at alphaCutoff (U8, 4 channels)
        bool preserveCoverage = false;
        float alphaCutoff = 0.5f;
        // fully transparent texels next to visible ones take their colour, on every level
        // (U8, 4 channels): bilinear sampling at a cut-out edge shows no dark fringe
        bool bleedAlpha = false;
        // 0: down to 1x1
        int maxLevels = 0;
    };

    class JASPERENGINE_API MipChain {
    public:
        // levels[0] is the base level; the chain is appended after it. Level 0 itself is
        // renormalized (normalMap) and bled (bleedAlpha) in place.
        static void build(std::vector<MipLevel>& levels, const MipChainOptions& options);
        // fraction of texels with alpha >= cutoff (U8, 4 channels)
        static float alphaCoverage(const MipLevel& level, float cutoff);
        // levels a chain of these options has for a base of this size
        static int levelCount(int width, int height, const MipChainOptions& options);
        static size_t texelBytes(const MipChainOptions& options);
    };

    // MipFilter::Nearest on one axis: the source index destination index i reads, the texel under
    // its centre (i + 0.5) * src / dst, a centre on a texel edge taking the lower texel (a 2x
    // downscale keeps the top-left texel of every 2x2 block). Integer maths: the same everywhere.
    JASPERENGINE_API int nearestSourceIndex(int i, int sourceSize, int destinationSize);
    // MipFilter::Nearest on an image of `texelBytes` bytes per texel (any pixel type)
    JASPERENGINE_API void nearestResize(const uint8_t* source, int sourceWidth, int sourceHeight, uint8_t* destination,
                                        int destinationWidth, int destinationHeight, size_t texelBytes);

    // TextureData::fixAlphaBorder's rings, on an RGBA8 image
    JASPERENGINE_API void bleedAlphaBorder(uint8_t* rgba, int width, int height, int rings = 4);

}

#endif // MIPCHAIN_H
