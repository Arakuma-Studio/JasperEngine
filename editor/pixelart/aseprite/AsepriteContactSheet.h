// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A contact sheet of a sprite sheet's frames as the game draws them (docs/specs/S44 R35-R36):
// each frame in a cell of its source size (trimmed pixels at their offset), on a checkerboard,
// scaled up by an integer, its index under it in a 3x5 pixel font, and a band in each tag's
// colour under the frames the tag spans. Pure (any thread): view_asset and
// set_sprite_import_settings' include_preview use it.

#pragma once

#include "pixelart/PixelImage.h"

#include "sprite/SpriteSheet.h"

#include <vector>

namespace jasperengine::editor::pixelart::aseprite {

    // `pages` in the sheet's page order; the result's longest side is at most maxSize when the
    // frames fit at 1x (else 1x, larger)
    PixelImage contactSheet(const SpriteSheetData& sheet, const std::vector<PixelImage>& pages, int maxSize);

}
