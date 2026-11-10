// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The shade op's colour formulas (docs/specs/S47-agent-sprite-drawing.md R20), ported literally
// from Pixelorama's Shading tool (src/Tools/DesignTools/Shading.gd, LightenDarkenOp; MIT, (c)
// 2019-present Orama Interactive and contributors) with Godot's Color HSV getters and setters:
//   simple     lighten: c + (1 - c) * amount; darken: c * (1 - amount) (Color.lightened/darkened)
//   hue_shift  the hue turns towards yellow (60 degrees) to lighten and blue (240 degrees) to
//              darken, never past them; lighten lowers saturation to a floor of 0.1 and raises
//              value, darken raises saturation and lowers value to a floor of 0.1
// in double, rounded half away from zero to 8 bits. Transparent pixels never change (the op's job).

#pragma once

#include "pixelart/PixelImage.h"

namespace jasperengine::editor::pixelart::draw {

    struct ShadeParams {
        bool hueShift = false;     // false: simple
        bool lighten = true;
        double amount = 10.0;      // simple, 0-100
        double hue = 10.0;         // hue_shift, degrees -180-180
        double saturation = 10.0;  // hue_shift, -100-100
        double value = 10.0;       // hue_shift, -100-100
    };

    PixelRgba shadeColor(PixelRgba color, const ShadeParams& params);

    // Godot's Color HSV, in double (h, s, v in 0-1; components may leave 0-1 on the way, as in Godot)
    struct GodotColor {
        double r = 0, g = 0, b = 0;
        double h() const;
        double s() const;
        double v() const;
        void setHsv(double hue, double sat, double val);
    };

    // A 0-1 component to a byte: clamped, rounded half away from zero
    uint8_t toByte(double component);

}
