// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/lost.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// Colours the palette lost (docs/specs/S46-pixel-art-cleanup.md R13): cells whose own colour is far
// from the palette colour they snapped to, grouped by that own colour. Adding a lost colour to the
// palette brings its details back wherever it is the nearest.

#pragma once

#include "pixelart/cleanup/CleanupTypes.h"

#include <cstdint>
#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    // The default distance (OKLab) past which a cell's colour counts as lost (lost.ts)
    constexpr double defaultLostThreshold = 0.06;

    // The lost colours of `art`, snapped from `samples`, most cells first. Cells marked 1 in `skip`
    // (the background) are left out, and so are colours lost in fewer than `minCells` cells.
    // `own` (optional): the same cells quantized without a palette, which already hold each cell's
    // median: a slider re-run then needs no median or OKLab per cell (R39).
    std::vector<LostColor> lostColors(const CellSamples& samples, const PixelArt& art, double threshold,
                                      const std::vector<uint8_t>* skip = nullptr, int minCells = 2,
                                      const PixelArt* own = nullptr);

}
