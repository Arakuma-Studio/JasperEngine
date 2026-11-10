// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/background.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// Removing the background of the fixed picture (docs/specs/S46-pixel-art-cleanup.md R15): the
// colours covering most of the border (one, or the two of a fake checkerboard) are flooded inward
// through colours within the tolerance, stopping at the outline; optionally enclosed areas, specks
// left alone in the background and a drop shadow go too.

#pragma once

#include "pixelart/cleanup/CleanupTypes.h"

#include <cstdint>
#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    // The largest group of pixels, alone in the background, that counts as a speck (background.ts)
    constexpr int SPECK_SIZE = 3;

    // The palette indices of the border's background: its most common colour and up to two more
    // at least 40 % as common, near-identical colours counted together
    std::vector<int> backgroundColors(const PixelArt& art);

    // Which pixels are background (1); `colors` are backgroundColors' indices
    std::vector<uint8_t> findBackground(const PixelArt& art, const std::vector<int>& colors, const BackgroundOptions& options);

    // The picture with the masked pixels made transparent
    PixelArt clearMasked(const PixelArt& art, const std::vector<uint8_t>& mask);

}
