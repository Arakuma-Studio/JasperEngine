// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Generate Ramp and the ramp helpers (docs/specs/S43-palettes.md R4, R10, AC6). Hue Shift is
// checked against a transcription of Pixelorama's Shading.gd:35-119 written here line by line in
// Godot's semantics (GDScript doubles, Color floats, Image::set_pixel truncating), and against the
// lists a third transcription (Python, float32 emulated) printed.

#include "pixelart/RampGenerator.h"

#include "pixelart/PixelColor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace rampgenerator_test {

    namespace px = jasperengine::pixelart;
    using namespace jasperengine::editor::pixelart;

    // Godot 4's Color (core/math/color.cpp), one float operation per statement
    struct GdColor {
        float r, g, b, a;

        float h() const {
            float min = std::min(r, g);
            min = std::min(min, b);
            float max = std::max(r, g);
            max = std::max(max, b);
            const float delta = max - min;
            if (delta == 0.0f) return 0.0f;
            float hue;
            if (r == max) {
                const float d = g - b;
                hue = d / delta;
            } else if (g == max) {
                const float d = b - r;
                const float q = d / delta;
                hue = 2 + q;
            } else {
                const float d = r - g;
                const float q = d / delta;
                hue = 4 + q;
            }
            hue /= 6.0f;
            if (hue < 0.0f) hue += 1.0f;
            return hue;
        }
        float s() const {
            float min = std::min(r, g);
            min = std::min(min, b);
            float max = std::max(r, g);
            max = std::max(max, b);
            const float delta = max - min;
            return (max != 0.0f) ? (delta / max) : 0.0f;
        }
        float v() const {
            float max = std::max(r, g);
            return std::max(max, b);
        }
        void set_hsv(float p_h, float p_s, float p_v, float p_alpha) {
            a = p_alpha;
            if (p_s == 0.0f) {
                r = g = b = p_v;
                return;
            }
            p_h *= 6.0f;
            p_h = std::fmod(p_h, 6.0f);
            const int i = (int)std::floor(p_h);
            const float f = p_h - i;
            const float k1 = 1.0f - p_s;
            const float p = p_v * k1;
            const float sf = p_s * f;
            const float k2 = 1.0f - sf;
            const float q = p_v * k2;
            const float omf = 1.0f - f;
            const float so = p_s * omf;
            const float k3 = 1.0f - so;
            const float t = p_v * k3;
            switch (i) {
                case 0: r = p_v; g = t; b = p; break;
                case 1: r = q; g = p_v; b = p; break;
                case 2: r = p; g = p_v; b = t; break;
                case 3: r = p; g = q; b = p_v; break;
                case 4: r = t; g = p; b = p_v; break;
                default: r = p_v; g = p; b = q; break;
            }
        }
        void set_h(float x) { set_hsv(x, s(), v(), a); }
        void set_s(float x) { set_hsv(h(), x, v(), a); }
        void set_v(float x) { set_hsv(h(), s(), x, a); }
    };

    double gd_fposmod(double x, double y) {
        double value = std::fmod(x, y);
        if (((value < 0) && (y > 0)) || ((value > 0) && (y < 0))) value += y;
        value += 0.0;
        return value;
    }

    // LightenDarkenOp with shading_mode HUE_SHIFTING
    struct LightenDarkenOp {
        bool lighten = true;
        double hue_amount = 10.0, sat_amount = 10.0, value_amount = 10.0;
        double hue_lighten_limit = 60.0 / 360.0, hue_darken_limit = 240.0 / 360.0;
        double sat_lighten_limit = 10.0 / 100.0, value_darken_limit = 10.0 / 100.0;

        bool hue_range(double hue) const { return hue > hue_lighten_limit && hue < hue_darken_limit; }

        double hue_limit_lighten(double hue, double hue_shift) const {
            if (hue_shift > 0) {
                if (hue < hue_darken_limit) {
                    if (hue + hue_shift >= hue_lighten_limit) hue_shift = hue_lighten_limit - hue;
                } else {
                    if (hue + hue_shift >= hue_lighten_limit + 1) hue_shift = hue_lighten_limit - hue;
                }
            } else if (hue_shift < 0 && hue + hue_shift <= hue_lighten_limit) {
                hue_shift = hue_lighten_limit - hue;
            }
            return hue_shift;
        }

        double hue_limit_darken(double hue, double hue_shift) const {
            if (hue_shift > 0) {
                if (hue < hue_darken_limit) {
                    if (hue - hue_shift <= hue_darken_limit - 1) hue_shift = hue - hue_darken_limit;
                } else {
                    if (hue - hue_shift <= hue_darken_limit) hue_shift = hue - hue_darken_limit;
                }
            } else if (hue_shift < 0 && hue - hue_shift >= hue_darken_limit) {
                hue_shift = hue - hue_darken_limit;
            }
            return hue_shift;
        }

        GdColor process(GdColor dst) const {
            if (dst.a == 0) return dst;
            double hue_shift = hue_amount / 360.0;
            const double sat_shift = sat_amount / 100.0;
            const double value_shift = value_amount / 100.0;
            if (hue_range(dst.h())) hue_shift = -hue_shift;
            if (lighten) {
                hue_shift = hue_limit_lighten(dst.h(), hue_shift);
                dst.set_h((float)gd_fposmod(dst.h() + hue_shift, 1));
                if (dst.s() > sat_lighten_limit) dst.set_s((float)std::max(dst.s() - std::min(sat_shift, (double)dst.s()), sat_lighten_limit));
                dst.set_v((float)(dst.v() + value_shift));
            } else {
                hue_shift = hue_limit_darken(dst.h(), hue_shift);
                dst.set_h((float)gd_fposmod(dst.h() - hue_shift, 1));
                dst.set_s((float)(dst.s() + sat_shift));
                if (dst.v() > value_darken_limit) dst.set_v((float)std::max(dst.v() - std::min(value_shift, (double)dst.v()), value_darken_limit));
            }
            return dst;
        }
    };

    // Image::get_pixel / set_pixel of an RGBA8 image
    GdColor gd_get_pixel(px::Rgba8 c) {
        return GdColor{(float)(c.r / 255.0), (float)(c.g / 255.0), (float)(c.b / 255.0), (float)(c.a / 255.0)};
    }
    px::Rgba8 gd_set_pixel(const GdColor& c, uint8_t alpha) {
        auto byte = [](float x) { return (uint8_t)std::clamp((double)x * 255.0, 0.0, 255.0); };
        return px::Rgba8{byte(c.r), byte(c.g), byte(c.b), alpha};
    }

    std::vector<px::Rgba8> shadingGdRamp(const RampRecipe& recipe) {
        LightenDarkenOp op;
        op.hue_amount = recipe.hue;
        op.sat_amount = recipe.saturation;
        op.value_amount = recipe.value;
        op.hue_lighten_limit = recipe.hueLightenLimit / 360.0;
        op.hue_darken_limit = recipe.hueDarkenLimit / 360.0;
        op.sat_lighten_limit = recipe.saturationFloor / 100.0;
        op.value_darken_limit = recipe.valueFloor / 100.0;
        std::vector<px::Rgba8> ramp = {recipe.base};
        px::Rgba8 pixel = recipe.base;
        op.lighten = false;
        for (int i = 0; i < recipe.darker; i++) {
            pixel = gd_set_pixel(op.process(gd_get_pixel(pixel)), recipe.base.a);
            ramp.insert(ramp.begin(), pixel);
        }
        pixel = recipe.base;
        op.lighten = true;
        for (int i = 0; i < recipe.lighter; i++) {
            pixel = gd_set_pixel(op.process(gd_get_pixel(pixel)), recipe.base.a);
            ramp.push_back(pixel);
        }
        return ramp;
    }

    px::Rgba8 rampHex(const char* hex) {
        px::Rgba8 color;
        EXPECT_TRUE(px::parseHex(hex, color)) << hex;
        return color;
    }

    std::vector<std::string> rampHexes(const std::vector<px::Rgba8>& colors) {
        std::vector<std::string> hexes;
        for (const px::Rgba8& color : colors) hexes.push_back(px::toHex(color));
        return hexes;
    }

    TEST(RampGenerator, HueShiftMatchesShadingGd) {
        struct Case {
            const char* what;
            RampRecipe recipe;
            std::vector<std::string> golden;
        };
        std::vector<Case> cases;
        RampRecipe red;
        red.base = rampHex("#b13e53");
        cases.push_back({"#b13e53, 3 darker, 3 lighter", red, {"#630445", "#7d1249", "#97254d", "#b13e53", "#ca5b5c", "#e38d7d", "#fcc0a4"}});
        // a green is between yellow and blue: the hue turns the other way
        RampRecipe green;
        green.base = rampHex("#38b764");
        cases.push_back({"yellow-to-blue reversal", green, {"#006959", "#0d835d", "#209d60", "#38b764", "#54d06a", "#75e976", "#abff9b"}});
        // 30 degree steps from an orange (39 degrees) stop at the 60 degree limit
        RampRecipe orange;
        orange.base = rampHex("#ffcd75");
        orange.darker = 2;
        orange.lighter = 4;
        orange.hue = 30;
        cases.push_back({"hue limit", orange, {"#cb346b", "#e56652", "#ffcd75", "#ffff9c", "#ffffc7", "#fffff6", "#ffffff"}});
        RampRecipe limits = red;
        limits.darker = 5;
        limits.lighter = 5;
        limits.hue = 25;
        limits.hueLightenLimit = 40;
        limits.hueDarkenLimit = 200;
        cases.push_back({"moved limits", limits, {"#000c2f", "#070049", "#3a0463", "#7c127d", "#972569", "#b13e53", "#ca755b", "#e3bf7d",
                                                  "#fcdfa4", "#fffed0", "#fffffc"}});
        for (const Case& c : cases) {
            const std::vector<px::Rgba8> ramp = generateRamp(c.recipe);
            EXPECT_EQ(rampHexes(ramp), rampHexes(shadingGdRamp(c.recipe))) << c.what;
            EXPECT_EQ(rampHexes(ramp), c.golden) << c.what;
            ASSERT_EQ((int)ramp.size(), c.recipe.darker + 1 + c.recipe.lighter) << c.what;
            EXPECT_EQ(ramp[(size_t)c.recipe.darker], c.recipe.base) << c.what;
        }
    }

    TEST(RampGenerator, StepsAreClampedAndTransparentStaysPut) {
        RampRecipe recipe;
        recipe.base = rampHex("#3b5dc9");
        recipe.darker = 40;
        recipe.lighter = -2;
        const std::vector<px::Rgba8> ramp = generateRamp(recipe);
        ASSERT_EQ(ramp.size(), 16u);
        EXPECT_EQ(ramp.back(), recipe.base);

        // the shading rule leaves a transparent pixel alone
        recipe.base = px::Rgba8{40, 80, 120, 0};
        recipe.darker = 2;
        recipe.lighter = 2;
        for (const px::Rgba8& color : generateRamp(recipe)) EXPECT_EQ(color, recipe.base);
    }

    double hueDistance(double a, double b) {
        const double d = std::fabs(std::fmod(a - b + 540.0, 360.0) - 180.0);
        return d;
    }

    TEST(RampGenerator, OklchIsDeterministicAndMonotonicInLightness) {
        for (const char* base : {"#b13e53", "#3b5dc9", "#38b764", "#ffcd75", "#808080", "#5d275d"}) {
            RampRecipe recipe;
            recipe.base = rampHex(base);
            recipe.mode = RampMode::Oklch;
            recipe.darker = 4;
            recipe.lighter = 5;
            const std::vector<px::Rgba8> ramp = generateRamp(recipe);
            EXPECT_EQ(ramp, generateRamp(recipe)) << base;
            ASSERT_EQ(ramp.size(), 10u) << base;
            EXPECT_EQ(ramp[4], recipe.base) << base;
            for (size_t i = 1; i < ramp.size(); i++) {
                EXPECT_LE(px::toOklab(ramp[i - 1]).l, px::toOklab(ramp[i]).l + 1e-4f) << base << " step " << i;
            }
            // the ends sit at the chosen lightness (the base widens the range when it lies outside)
            EXPECT_NEAR(px::toOklab(ramp.front()).l, std::min(0.2f, px::toOklab(recipe.base).l), 0.01f) << base;
            EXPECT_NEAR(px::toOklab(ramp.back()).l, std::max(0.95f, px::toOklab(recipe.base).l), 0.01f) << base;
        }
    }

    TEST(RampGenerator, OklchHueTurnsTowardItsTargets) {
        RampRecipe recipe;
        recipe.base = rampHex("#3b5dc9");
        recipe.mode = RampMode::Oklch;
        recipe.darker = 3;
        recipe.lighter = 3;
        recipe.lightestL = 0.8f;
        const std::vector<px::Rgba8> ramp = generateRamp(recipe);
        const double baseHue = px::toOklch(px::toOklab(recipe.base)).h;
        // a blue near 240 degrees: darker steps reach it, lighter ones turn toward 60 degrees
        const double darkest = px::toOklch(px::toOklab(ramp.front())).h;
        const double lightest = px::toOklch(px::toOklab(ramp.back())).h;
        EXPECT_LE(hueDistance(darkest, 240.0), hueDistance(baseHue, 240.0) + 1.0);
        EXPECT_LT(hueDistance(lightest, 60.0), hueDistance(baseHue, 60.0));
    }

    // pixelfx's fit(r, n) and at(r, t) (src/core/color.js:37-48), as indices into an 8-step ramp
    std::vector<px::Rgba8> indexRamp(int length) {
        std::vector<px::Rgba8> ramp;
        for (int i = 0; i < length; i++) ramp.push_back(px::Rgba8{(uint8_t)i, 0, 0, 255});
        return ramp;
    }

    std::vector<int> fitIndices(int length, int steps) {
        std::vector<int> indices;
        for (const px::Rgba8& color : px::fitRamp(indexRamp(length), steps)) indices.push_back(color.r);
        return indices;
    }

    TEST(RampGenerator, FitAndAtMatchPixelfx) {
        EXPECT_EQ(fitIndices(8, 4), (std::vector<int>{0, 2, 5, 7}));
        EXPECT_EQ(fitIndices(8, 3), (std::vector<int>{0, 4, 7}));
        EXPECT_EQ(fitIndices(8, 1), (std::vector<int>{0}));
        EXPECT_EQ(fitIndices(8, 8), (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
        EXPECT_EQ(fitIndices(8, 16), (std::vector<int>{0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7}));
        EXPECT_EQ(fitIndices(7, 5), (std::vector<int>{0, 2, 3, 5, 6}));
        EXPECT_EQ(fitIndices(2, 5), (std::vector<int>{0, 0, 1, 1, 1}));
        EXPECT_EQ(fitIndices(1, 4), (std::vector<int>{0, 0, 0, 0}));
        EXPECT_EQ(fitIndices(6, 2), (std::vector<int>{0, 5}));
        EXPECT_EQ(fitIndices(5, 9), (std::vector<int>{0, 1, 1, 2, 2, 3, 3, 4, 4}));

        const struct { int length; float t; int index; } at[] = {
            {8, 0.0f, 0}, {8, 0.999f, 7}, {8, 1.0f, 7}, {8, 0.5f, 4}, {7, 0.3f, 2},
            {7, -0.2f, 0}, {7, 1.7f, 6}, {4, 0.25f, 1}, {4, 0.2499f, 0}, {3, 0.6667f, 2}};
        for (const auto& c : at) {
            EXPECT_EQ(px::rampAt(indexRamp(c.length), c.t).r, c.index) << "at(" << c.length << ", " << c.t << ")";
        }
    }

}
