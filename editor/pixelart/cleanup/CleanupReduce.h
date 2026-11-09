// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/reduce.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.
// The stroke handling follows ToPixel's forced resize (https://github.com/daisuqe/ToPixel).

// Making the fixed picture smaller by any ratio (docs/specs/S46-pixel-art-cleanup.md R12): each new
// pixel takes the colour covering most of its block, or with Outlines the strokes are carried over
// as one-pixel lines and their one-pixel steps become diagonals.

#pragma once

#include "pixelart/cleanup/CleanupTypes.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    // The picture at width x height (rounded, at most its own size; the same picture when equal)
    PixelArt reduce(const PixelArt& art, double width, double height, ReduceMode mode);

    // Turns the one-pixel steps of a stroke into diagonals, for up to 4 passes, in place
    void smoothStairs(std::vector<int32_t>& indices, int width, int height, const std::function<bool(int32_t)>& isOutline);

}
