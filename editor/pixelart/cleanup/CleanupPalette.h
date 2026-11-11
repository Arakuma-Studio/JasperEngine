// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/palette.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// A palette proposed from the picture itself (docs/specs/S46-pixel-art-cleanup.md R9): the most
// used colours (dominant), k-means in OKLab (balanced) or the farthest colours (distinct). The
// presets and palette files are S43's.

#pragma once

#include "pixelart/cleanup/CleanupTypes.h"

#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    // `count` colours that best represent `colors` (k-means in OKLab seeded by k-means++ with
    // mulberry32(seed), at most 24 iterations). Locked colours stay exact and come first; the
    // extracted ones follow, darkest first.
    std::vector<Rgba8> extractPalette(const std::vector<Rgba8>& colors, int count,
                                      const std::vector<Rgba8>& locked = {}, uint32_t seed = 1);

    // A proposed colour and the share of the colours nearest to it (0 to 1)
    struct Proposal {
        Rgba8 color;
        double share = 0;
    };
    // Up to `count` colours from `colors` (one per cell), biggest share first; never two closer
    // than 0.04 OKLab. `seed` drives balanced's k-means.
    std::vector<Proposal> proposePalette(const std::vector<Rgba8>& colors, int count, ProposeMethod method, uint32_t seed = 1);

}
