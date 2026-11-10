// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/edits.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// Hand edits on the fixed picture (docs/specs/S46-pixel-art-cleanup.md R16): erase, restore and
// paint one pixel or its 4-connected area of one colour, applied in order. They correct a cleanup
// result; they are not a canvas.

#pragma once

#include "pixelart/cleanup/CleanupTypes.h"

#include <cstdint>
#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    struct Edited {
        PixelArt art;
        std::vector<uint8_t> mask;            // erased pixels (1): the mask given, with the edits applied
        std::vector<int> painted;             // pixels painted and not erased since, ascending
    };

    // Erase and restore set the mask; paint sets the colour (the exact palette colour, else the
    // nearest by `metric`) and shows the pixel again
    Edited applyEdits(const PixelArt& art, const std::vector<uint8_t>& mask, const std::vector<Edit>& edits,
                      jasperengine::pixelart::ColorMetric metric);

}
