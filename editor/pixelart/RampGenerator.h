// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Generate Ramp (docs/specs/S43-palettes.md R10): a base colour grown into a shading ramp, darker
// steps below it and lighter ones above. Deterministic: the same recipe gives the same bytes.

#include "pixelart/Rgba8.h"

#include <cstdint>
#include <vector>

namespace jasperengine::editor::pixelart {

    enum class RampMode : uint8_t { HueShift, Oklch };

    struct RampRecipe {
        static constexpr int kMaxSteps = 15;

        jasperengine::pixelart::Rgba8 base;
        int darker = 3, lighter = 3;            // 0 to 15 steps each side
        RampMode mode = RampMode::HueShift;
        // per step: the hue in degrees, saturation and value in percent (Hue Shift); OKLCh uses the hue
        float hue = 10, saturation = 10, value = 10;
        // Hue Shift's limits: lightening pulls the hue toward hueLightenLimit (a yellow) and lowers
        // the saturation no further than saturationFloor; darkening pulls it toward hueDarkenLimit
        // (a blue) and lowers the value no further than valueFloor
        float hueLightenLimit = 60, hueDarkenLimit = 240, saturationFloor = 10, valueFloor = 10;
        // OKLCh: the OKLab lightness of the darkest and the lightest step, and where along the ramp
        // (0 the darkest, 1 the lightest) the chroma peaks
        float darkestL = 0.2f, lightestL = 0.95f, chromaPeak = 0.5f;
    };

    // darker + 1 + lighter colours, darkest first; the base itself sits at index `darker`.
    //
    // Hue Shift is Pixelorama's Shading tool rule (LightenDarkenOp HUE_SHIFTING), applied once per
    // step outward from the base: each step lightens or darkens the previous step's 8-bit colour.
    //
    // OKLCh: the darker steps' lightness falls evenly from the base's to darkestL, the lighter
    // ones' rises evenly to lightestL (a base outside that range widens it, so lightness never
    // decreases along the ramp). The hue turns `hue` degrees per step toward 60° going lighter and
    // toward 240° going darker, stopping there. The chroma is the base's, scaled by the parabola
    // 1 - ½((t - chromaPeak) / max(chromaPeak, 1 - chromaPeak))² of the position t in [0, 1]
    // relative to its value at the base, so it falls toward both ends. A colour outside sRGB keeps
    // its lightness and hue and loses chroma until it fits.
    std::vector<jasperengine::pixelart::Rgba8> generateRamp(const RampRecipe& recipe);

}
