// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/cleanup.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// Speck cleanup (docs/specs/S46-pixel-art-cleanup.md R10): a small 8-connected group of one colour
// merges into the colour it touches most when its own cells doubted it, or when the two colours
// are alike; a speck drawn clearly in a distinct colour (an eye, a highlight) stays.

#pragma once

#include "pixelart/cleanup/CleanupTypes.h"

#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    struct CleanupOptions {
        double size = 0;                      // the largest speck in pixels; < 1 turns cleaning off
        double strength = 0.5;                // 0-1: 0 merges only coin flips, 1 merges every speck
        jasperengine::pixelart::ColorMetric metric = jasperengine::pixelart::ColorMetric::OkLab;
        std::vector<int> protect;             // palette indices never cleaned away (locked colours)
        std::vector<int> keep;                // cells whose speck stays (brought back by hand)
    };

    struct Cleaned {
        PixelArt art;
        std::vector<std::vector<int>> specks; // the specks merged, each as its cells
    };

    // `samples` are the cells `art` was snapped from (one pixel per cell)
    Cleaned cleanUp(const PixelArt& art, const CellSamples& samples, const CleanupOptions& options);

}
