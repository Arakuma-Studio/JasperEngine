// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// diff_sprite (docs/specs/S47-agent-sprite-drawing.md R29, R30): two states of a sprite, frame by
// frame. Each frame's `rows` cover only its changed bbox, '-' for unchanged and the new value
// elsewhere, so paste {rows, at: bbox.xy} re-applies the change.

#pragma once

#include "SpriteDoc.h"

#include "json.hpp"

#include <optional>
#include <vector>

namespace jasperengine::editor::pixelart::draw {

    struct DiffOptions {
        std::vector<int> frames;       // from 0, in the current sprite
        int layer = -1;                // -1: the composite
        std::optional<IntRect> rect;   // only here
        int maxCells = 64;
        bool grid = true;
        // {frame: n}: every frame is compared with this frame of the same sprite (from 0)
        int againstFrame = -1;
    };

    struct DiffImages {
        PixelImage before, after;
        IntRect bbox;
    };

    // False with size_mismatch. `images` receives the first changed frame's two sides.
    bool diffSprites(const SpriteDoc& current, const SpriteDoc& against, const DiffOptions& options, nlohmann::json& out, DiffImages* images, PixelProblem& problem);

}
