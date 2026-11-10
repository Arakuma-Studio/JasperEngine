// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pixelorama's src/Tools/DesignTools/Shading.gd:
//
//   MIT License
//
//   Copyright (c) 2019-present Orama Interactive and contributors
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//   and associated documentation files (the "Software"), to deal in the Software without
//   restriction, including without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all copies or
//   substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
//   BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
//   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
//   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "ShadeColor.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::editor::pixelart::draw {

    namespace {

        constexpr double kHueLightenLimit = 60.0 / 360.0;   // a yellow
        constexpr double kHueDarkenLimit = 240.0 / 360.0;   // a blue
        constexpr double kSatLightenLimit = 10.0 / 100.0;
        constexpr double kValueDarkenLimit = 10.0 / 100.0;

        double fposmod(double x, double m) {
            double r = std::fmod(x, m);
            if (r < 0) r += m;
            return r;
        }

        // roughly yellow-green-blue: the hue turns the other way there
        bool hueRange(double hue) { return hue > kHueLightenLimit && hue < kHueDarkenLimit; }

        double hueLimitLighten(double hue, double shift) {
            if (shift > 0) {
                if (hue < kHueDarkenLimit) {
                    if (hue + shift >= kHueLightenLimit) shift = kHueLightenLimit - hue;
                } else if (hue + shift >= kHueLightenLimit + 1) {
                    shift = kHueLightenLimit - hue;
                }
            } else if (shift < 0 && hue + shift <= kHueLightenLimit) {
                shift = kHueLightenLimit - hue;
            }
            return shift;
        }

        double hueLimitDarken(double hue, double shift) {
            if (shift > 0) {
                if (hue < kHueDarkenLimit) {
                    if (hue - shift <= kHueDarkenLimit - 1) shift = hue - kHueDarkenLimit;
                } else if (hue - shift <= kHueDarkenLimit) {
                    shift = hue - kHueDarkenLimit;
                }
            } else if (shift < 0 && hue - shift >= kHueDarkenLimit) {
                shift = hue - kHueDarkenLimit;
            }
            return shift;
        }

    }

    uint8_t toByte(double component) {
        const double v = std::clamp(component, 0.0, 1.0) * 255.0;
        return static_cast<uint8_t>(std::floor(v + 0.5));
    }

    double GodotColor::h() const {
        const double mn = std::min({r, g, b}), mx = std::max({r, g, b});
        const double delta = mx - mn;
        if (delta == 0.0) return 0.0;
        double hue;
        if (r == mx) hue = (g - b) / delta;
        else if (g == mx) hue = 2 + (b - r) / delta;
        else hue = 4 + (r - g) / delta;
        hue /= 6.0;
        if (hue < 0.0) hue += 1.0;
        return hue;
    }

    double GodotColor::s() const {
        const double mn = std::min({r, g, b}), mx = std::max({r, g, b});
        return mx != 0.0 ? (mx - mn) / mx : 0.0;
    }

    double GodotColor::v() const { return std::max({r, g, b}); }

    void GodotColor::setHsv(double hue, double sat, double val) {
        if (sat == 0.0) {
            r = g = b = val;
            return;
        }
        hue *= 6.0;
        hue = std::fmod(hue, 6.0);
        const int i = static_cast<int>(std::floor(hue));
        const double f = hue - i;
        const double p = val * (1.0 - sat);
        const double q = val * (1.0 - sat * f);
        const double t = val * (1.0 - sat * (1.0 - f));
        switch (i) {
            case 0: r = val; g = t; b = p; break;
            case 1: r = q; g = val; b = p; break;
            case 2: r = p; g = val; b = t; break;
            case 3: r = p; g = q; b = val; break;
            case 4: r = t; g = p; b = val; break;
            default: r = val; g = p; b = q; break;
        }
    }

    PixelRgba shadeColor(PixelRgba color, const ShadeParams& params) {
        if (color[3] == 0) return color;
        GodotColor c{color[0] / 255.0, color[1] / 255.0, color[2] / 255.0};
        if (!params.hueShift) {
            const double strength = params.amount / 100.0;
            if (params.lighten) {
                c.r += (1.0 - c.r) * strength;
                c.g += (1.0 - c.g) * strength;
                c.b += (1.0 - c.b) * strength;
            } else {
                c.r *= 1.0 - strength;
                c.g *= 1.0 - strength;
                c.b *= 1.0 - strength;
            }
        } else {
            double hueShift = params.hue / 360.0;
            const double satShift = params.saturation / 100.0;
            const double valueShift = params.value / 100.0;
            if (hueRange(c.h())) hueShift = -hueShift;
            // each Godot setter re-reads the other two components from the colour
            if (params.lighten) {
                hueShift = hueLimitLighten(c.h(), hueShift);
                c.setHsv(fposmod(c.h() + hueShift, 1.0), c.s(), c.v());
                if (c.s() > kSatLightenLimit) c.setHsv(c.h(), std::max(c.s() - std::min(satShift, c.s()), kSatLightenLimit), c.v());
                c.setHsv(c.h(), c.s(), c.v() + valueShift);
            } else {
                hueShift = hueLimitDarken(c.h(), hueShift);
                c.setHsv(fposmod(c.h() - hueShift, 1.0), c.s(), c.v());
                c.setHsv(c.h(), c.s() + satShift, c.v());
                if (c.v() > kValueDarkenLimit) c.setHsv(c.h(), c.s(), std::max(c.v() - std::min(valueShift, c.v()), kValueDarkenLimit));
            }
        }
        return {toByte(c.r), toByte(c.g), toByte(c.b), color[3]};
    }

}
