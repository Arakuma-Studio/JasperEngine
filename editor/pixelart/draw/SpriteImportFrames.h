// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The frames of the image files create_sprite {from} takes (docs/specs/S47-agent-sprite-drawing.md
// R26): a PNG, an animated PNG or a GIF, as RGBA frames of one size with their delays.

#pragma once

#include "pixelart/PixelImage.h"
#include "pixelart/PixelProblem.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace jasperengine::editor::pixelart::draw {

    struct ImportedFrames {
        std::vector<PixelImage> frames;
        std::vector<uint16_t> durations;   // ms, one per frame
    };

    bool readFramesFile(const std::filesystem::path& path, ImportedFrames& out, PixelProblem& problem);

}
