// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The pixel-art rotations and scalers of edit_sprite (docs/specs/S47-agent-sprite-drawing.md R21,
// R22, D9): CPU reference ports of Pixelorama's rotations (cleanEdge by torcado, OmniScale by Lior
// Halphon with deakcor's no-AA version, rotxel and nearest from DrawingAlgos.gd) and the published
// AdvMAME2x/AdvMAME3x rules. Pure functions on a plain RGBA8 buffer, any thread, deterministic:
// `double` maths with half-away-from-zero rounding and nothing but std::sin/cos/atan2/hypot/sqrt.
//
// Every function returns a *source map*: for each output pixel the index (y * w + x) of the
// source pixel it copies, or -1 for transparent. An output is therefore always made of source
// colours (an indexed sprite stays in its palette, AC5), and the caller applies the map to the
// RGBA and to the index plane alike.

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace jasperengine::editor::pixelart::draw {

    enum class RotateMethod : uint8_t { CleanEdge, Rotxel, OmniScale, Nearest };

    // `rgba` is w x h straight-alpha RGBA8, rows top first. The image turns by `degrees`
    // (positive: clockwise on screen, y pointing down) about (pivotX, pivotY), given in pixel-index
    // coordinates: pixel i's centre is i, so the centre of an even side 32 is 15.5. The output keeps
    // the w x h frame: what turns out of it is dropped, what nothing covers is -1.
    // Multiples of 90 degrees are exact pixel permutations (rounded to the pixel grid about the
    // pivot) whatever the method.
    std::vector<int32_t> rotateMap(int w, int h, std::span<const uint8_t> rgba, double degrees, double pivotX, double pivotY,
                                   RotateMethod method);

    // Two pixels (source indices) equal for the scalers: index equality in indexed mode, an RGBA
    // tolerance otherwise (the caller decides)
    using PixelEqual = std::function<bool(int32_t a, int32_t b)>;

    // AdvMAME2x (factor 2) / AdvMAME3x (factor 3) of a w x h image: a (w * factor) x (h * factor)
    // map, rows top first, edges clamped. Factor 4 is AdvMAME2x applied twice.
    std::vector<int32_t> scaleMap(int w, int h, int factor, const PixelEqual& equal);

}
