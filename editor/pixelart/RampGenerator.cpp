// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Hue Shift is ported from Pixelorama's src/Tools/DesignTools/Shading.gd (LightenDarkenOp,
// HUE_SHIFTING), MIT, Copyright (c) 2019-present Orama Interactive and contributors, with
// Godot 4's Color semantics (core/math/color.cpp: float32 channels, h/s/v getters and set_hsv,
// MIT, Copyright (c) 2014-present Godot Engine contributors): GDScript computes in double,
// Color stores float, and the shaded colour is written into an RGBA8 image by Image::set_pixel,
// which clamps and truncates.

#include "RampGenerator.h"

#include "pixelart/PixelColor.h"

#include <algorithm>
#include <cmath>

// file-local, in a namespace of their own: a unity build merges this file with the other pixel-art
// sources, whose helpers live in jasperengine::editor::pixelart
namespace jasperengine::editor::pixelart::rampio {
namespace {

    namespace px = jasperengine::pixelart;

    // Godot's Color, as far as the shading rule uses it. One operation per statement: no
    // compiler may fuse a multiply and an add, which would change the float results.
    struct GodotColor {
        float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;

        float getH() const {
            const float minC = std::min(std::min(r, g), b);
            const float maxC = std::max(std::max(r, g), b);
            const float delta = maxC - minC;
            if (delta == 0.0f) return 0.0f;
            float h;
            if (r == maxC) {
                const float diff = g - b;
                h = diff / delta;
            } else if (g == maxC) {
                const float diff = b - r;
                const float part = diff / delta;
                h = 2.0f + part;
            } else {
                const float diff = r - g;
                const float part = diff / delta;
                h = 4.0f + part;
            }
            h /= 6.0f;
            if (h < 0.0f) h += 1.0f;
            return h;
        }

        float getS() const {
            const float minC = std::min(std::min(r, g), b);
            const float maxC = std::max(std::max(r, g), b);
            const float delta = maxC - minC;
            return maxC != 0.0f ? delta / maxC : 0.0f;
        }

        float getV() const {
            return std::max(std::max(r, g), b);
        }

        void setHsv(float h, float s, float v, float alpha) {
            a = alpha;
            if (s == 0.0f) {
                r = g = b = v;
                return;
            }
            h *= 6.0f;
            h = std::fmod(h, 6.0f);
            const int i = (int)std::floor(h);
            const float f = h - (float)i;
            const float oneMinusS = 1.0f - s;
            const float p = v * oneMinusS;
            const float sf = s * f;
            const float oneMinusSf = 1.0f - sf;
            const float q = v * oneMinusSf;
            const float oneMinusF = 1.0f - f;
            const float sOneMinusF = s * oneMinusF;
            const float oneMinusSOneMinusF = 1.0f - sOneMinusF;
            const float t = v * oneMinusSOneMinusF;
            switch (i) {
                case 0: r = v; g = t; b = p; break;
                case 1: r = q; g = v; b = p; break;
                case 2: r = p; g = v; b = t; break;
                case 3: r = p; g = q; b = v; break;
                case 4: r = t; g = p; b = v; break;
                default: r = v; g = p; b = q; break;
            }
        }

        // the property setters of GDScript's dst.h = x, dst.s = x, dst.v = x
        void setH(float h) { setHsv(h, getS(), getV(), a); }
        void setS(float s) { setHsv(getH(), s, getV(), a); }
        void setV(float v) { setHsv(getH(), getS(), v, a); }
    };

    // Image::get_pixel of an RGBA8 image
    GodotColor fromBytes(px::Rgba8 c) {
        return GodotColor{(float)(c.r / 255.0), (float)(c.g / 255.0), (float)(c.b / 255.0), (float)(c.a / 255.0)};
    }

    // Image::set_pixel into an RGBA8 image
    uint8_t toByte(float channel) {
        const double scaled = (double)channel * 255.0;
        return (uint8_t)std::clamp(scaled, 0.0, 255.0);
    }

    // the alpha is the base's: the rule never changes it, and truncating its float could wear it
    // down by one per step
    px::Rgba8 toBytes(const GodotColor& c, uint8_t alpha) {
        return px::Rgba8{toByte(c.r), toByte(c.g), toByte(c.b), alpha};
    }

    // Godot's Math::fposmod for doubles
    double fposmod(double x, double y) {
        double value = std::fmod(x, y);
        if ((value < 0.0 && y > 0.0) || (value > 0.0 && y < 0.0)) value += y;
        return value + 0.0;
    }

    double finiteOr(float value, double fallback) {
        return std::isfinite(value) ? (double)value : fallback;
    }

    // LightenDarkenOp's members, in GDScript's doubles
    struct ShadingOp {
        double hueAmount, satAmount, valueAmount;
        double hueLightenLimit, hueDarkenLimit, satLightenLimit, valueDarkenLimit;

        explicit ShadingOp(const RampRecipe& recipe)
            : hueAmount(finiteOr(recipe.hue, 10.0)), satAmount(finiteOr(recipe.saturation, 10.0)), valueAmount(finiteOr(recipe.value, 10.0)),
              hueLightenLimit(finiteOr(recipe.hueLightenLimit, 60.0) / 360.0), hueDarkenLimit(finiteOr(recipe.hueDarkenLimit, 240.0) / 360.0),
              satLightenLimit(finiteOr(recipe.saturationFloor, 10.0) / 100.0), valueDarkenLimit(finiteOr(recipe.valueFloor, 10.0) / 100.0) {}

        // the colours roughly between yellow, green and blue
        bool hueRange(double hue) const {
            return hue > hueLightenLimit && hue < hueDarkenLimit;
        }

        double hueLimitLighten(double hue, double hueShift) const {
            if (hueShift > 0.0) {
                if (hue < hueDarkenLimit) {
                    // red-orange-yellow
                    if (hue + hueShift >= hueLightenLimit) hueShift = hueLightenLimit - hue;
                } else {
                    // blue-purple-red, looping around the colour wheel
                    if (hue + hueShift >= hueLightenLimit + 1.0) hueShift = hueLightenLimit - hue;
                }
            } else if (hueShift < 0.0 && hue + hueShift <= hueLightenLimit) {
                hueShift = hueLightenLimit - hue;
            }
            return hueShift;
        }

        double hueLimitDarken(double hue, double hueShift) const {
            if (hueShift > 0.0) {
                if (hue < hueDarkenLimit) {
                    if (hue - hueShift <= hueDarkenLimit - 1.0) hueShift = hue - hueDarkenLimit;
                } else {
                    if (hue - hueShift <= hueDarkenLimit) hueShift = hue - hueDarkenLimit;
                }
            } else if (hueShift < 0.0 && hue - hueShift >= hueDarkenLimit) {
                hueShift = hue - hueDarkenLimit;
            }
            return hueShift;
        }

        // (a transparent pixel, which the rule leaves alone, never gets here)
        GodotColor apply(GodotColor dst, bool lighten) const {
            double hueShift = hueAmount / 360.0;
            const double satShift = satAmount / 100.0;
            const double valueShift = valueAmount / 100.0;
            // between yellow, green and blue the hue turns the other way
            if (hueRange(dst.getH())) hueShift = -hueShift;
            if (lighten) {
                hueShift = hueLimitLighten(dst.getH(), hueShift);
                dst.setH((float)fposmod((double)dst.getH() + hueShift, 1.0));
                if ((double)dst.getS() > satLightenLimit) {
                    const double s = dst.getS();
                    dst.setS((float)std::max(s - std::min(satShift, s), satLightenLimit));
                }
                dst.setV((float)((double)dst.getV() + valueShift));
            } else {
                hueShift = hueLimitDarken(dst.getH(), hueShift);
                dst.setH((float)fposmod((double)dst.getH() - hueShift, 1.0));
                dst.setS((float)((double)dst.getS() + satShift));
                if ((double)dst.getV() > valueDarkenLimit) {
                    const double v = dst.getV();
                    dst.setV((float)std::max(v - std::min(valueShift, v), valueDarkenLimit));
                }
            }
            return dst;
        }
    };

    std::vector<px::Rgba8> hueShiftRamp(const RampRecipe& recipe, int darker, int lighter) {
        // the rule leaves a transparent pixel alone
        if (recipe.base.a == 0) return std::vector<px::Rgba8>((size_t)(darker + 1 + lighter), recipe.base);
        const ShadingOp op(recipe);
        std::vector<px::Rgba8> ramp((size_t)(darker + 1 + lighter));
        ramp[(size_t)darker] = recipe.base;
        px::Rgba8 color = recipe.base;
        for (int k = 1; k <= darker; k++) {
            color = toBytes(op.apply(fromBytes(color), false), recipe.base.a);
            ramp[(size_t)(darker - k)] = color;
        }
        color = recipe.base;
        for (int k = 1; k <= lighter; k++) {
            color = toBytes(op.apply(fromBytes(color), true), recipe.base.a);
            ramp[(size_t)(darker + k)] = color;
        }
        return ramp;
    }

    // `hue` turned toward `target` by up to `step` degrees, the short way round
    double turnHue(double hue, double target, double step) {
        const double delta = std::fmod(target - hue + 540.0, 360.0) - 180.0;
        if (std::fabs(delta) <= step) return target;
        const double turned = hue + (delta > 0.0 ? step : -step);
        return fposmod(turned, 360.0);
    }

    bool inSrgb(const px::Lab& lab) {
        float rgb[3];
        px::oklabToLinear(lab, rgb);
        constexpr float kSlack = 1e-5f;
        for (float channel : rgb) {
            if (!(channel >= -kSlack && channel <= 1.0f + kSlack)) return false;
        }
        return true;
    }

    // the most chroma up to `chroma` that this lightness and hue hold inside sRGB (bisection)
    px::Lab fitSrgb(float lightness, float chroma, float hue) {
        px::Lab lab = px::fromOklch(px::LCh{lightness, chroma, hue});
        if (inSrgb(lab)) return lab;
        float low = 0.0f, high = chroma;
        for (int i = 0; i < 24; i++) {
            const float middle = 0.5f * (low + high);
            if (inSrgb(px::fromOklch(px::LCh{lightness, middle, hue}))) low = middle;
            else high = middle;
        }
        return px::fromOklch(px::LCh{lightness, low, hue});
    }

    std::vector<px::Rgba8> oklchRamp(const RampRecipe& recipe, int darker, int lighter) {
        const int count = darker + 1 + lighter;
        std::vector<px::Rgba8> ramp((size_t)count);
        ramp[(size_t)darker] = recipe.base;
        if (count == 1) return ramp;

        const px::LCh base = px::toOklch(px::toOklab(recipe.base));
        const double baseL = base.l;
        const double darkest = std::clamp(std::min(finiteOr(recipe.darkestL, 0.2), baseL), 0.0, 1.0);
        const double lightest = std::clamp(std::max(finiteOr(recipe.lightestL, 0.95), baseL), 0.0, 1.0);
        const double step = std::fabs(finiteOr(recipe.hue, 10.0));
        const double peak = std::clamp(finiteOr(recipe.chromaPeak, 0.5), 0.0, 1.0);
        const double width = std::max(peak, 1.0 - peak);
        auto chromaShape = [&](double t) {
            const double x = (t - peak) / width;
            return 1.0 - 0.5 * x * x;
        };
        const double last = (double)(count - 1);
        const double baseShape = chromaShape((double)darker / last);

        for (int i = 0; i < count; i++) {
            if (i == darker) continue;
            const bool lighterStep = i > darker;
            const int k = lighterStep ? i - darker : darker - i;
            const double lightness = lighterStep ? baseL + (lightest - baseL) * (double)k / (double)lighter
                                                 : baseL + (darkest - baseL) * (double)k / (double)darker;
            const double hue = turnHue(base.h, lighterStep ? 60.0 : 240.0, step * (double)k);
            const double chroma = (double)base.c * chromaShape((double)i / last) / baseShape;
            const px::Lab lab = fitSrgb((float)lightness, (float)std::max(0.0, chroma), (float)hue);
            ramp[(size_t)i] = px::fromOklab(lab, recipe.base.a);
        }
        return ramp;
    }

}
}

std::vector<jasperengine::pixelart::Rgba8> jasperengine::editor::pixelart::generateRamp(const RampRecipe& recipe) {
    const int darker = std::clamp(recipe.darker, 0, RampRecipe::kMaxSteps);
    const int lighter = std::clamp(recipe.lighter, 0, RampRecipe::kMaxSteps);
    return recipe.mode == RampMode::Oklch ? rampio::oklchRamp(recipe, darker, lighter) : rampio::hueShiftRamp(recipe, darker, lighter);
}
