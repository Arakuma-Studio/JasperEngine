// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// edit_sprite's lints after a draw (docs/specs/S47-agent-sprite-drawing.md R31): S46's lint() on
// the composite of each touched frame, with the sprite's palette. The one call site of the lint
// library in the drawing tools; a lint never blocks a write.

#pragma once

#include "SpriteDoc.h"
#include "SpriteEdit.h"

#include "pixelart/Rgba8.h"

#include "json.hpp"

#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::draw {

    // `lint`: true (every applicable lint) or a list of ids. False with `problem` for an unknown id.
    bool validateLintArg(const nlohmann::json& lint, PixelProblem& problem);

    // {counts: {pass, warn, fail, skip}, results: [non-PASS lints; a SKIP only when asked for by id]}
    // with spots in the sprite's frame numbers
    nlohmann::json lintAfterDraw(const SpriteDoc& sprite, const std::vector<DirtyRect>& changed, const nlohmann::json& lint,
                                 const std::vector<jasperengine::pixelart::Rgba8>& backdrops);

}
