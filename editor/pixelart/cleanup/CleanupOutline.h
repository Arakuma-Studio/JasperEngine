// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/outline.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.
// After ToPixel (https://github.com/daisuqe/ToPixel), whose outline mode ritmo's follows.

// One-pixel outlines (docs/specs/S46-pixel-art-cleanup.md R11): the dark threshold from a
// luminance histogram valley, whether the strokes are about one cell wide, and giving a stroke the
// grid split between two pixels back to the pixel holding more of it.

#pragma once

#include "pixelart/cleanup/CleanupTypes.h"

#include <cstdint>
#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    // The picture's outline pixels: opaque (alpha >= 128) and at or below the dark threshold
    OutlineMask outlineMask(const RgbaImage& image);

    // Which palette colours are outline colours, by outlineMask's rule on the fixed picture (1: yes)
    std::vector<uint8_t> outlineColorsOfArt(const PixelArt& art);

    // Whether the dark strokes are about one cell wide (the median dark run 0.6-1.5 cells over at
    // least 40 runs); never for cells under 2 px
    bool hasOutlines(const OutlineMask& mask, double cell);

    // The palette colours as dark as the source's outline pixels (1: yes)
    std::vector<uint8_t> outlineColors(const PixelArt& art, const OutlineMask& mask);

    // Gives a one-cell stroke split into two pixels back to the pixel holding more of it
    PixelArt thinOutlines(const PixelArt& art, const Grid& grid, const OutlineMask& mask);

}
