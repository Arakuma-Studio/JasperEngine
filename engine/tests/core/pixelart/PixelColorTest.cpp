// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The pixel-art colour maths (docs/specs/S43-palettes.md R12, AC5).

#include "pixelart/PixelColor.h"
#include "pixelart/PixelColorHash.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::pixelart;

uint64_t pixelColorHashUnoptimised();   // PixelColorO0.cpp

namespace {

    // Sharma, Wu and Dalal, "The CIEDE2000 color-difference formula: implementation notes,
    // supplementary test data, and mathematical observations" (2005), Table 1: all 34 pairs
    struct SharmaPair { float l1, a1, b1, l2, a2, b2, deltaE; };
    const SharmaPair kSharma[] = {
        {50.0000f, 2.6772f, -79.7751f, 50.0000f, 0.0000f, -82.7485f, 2.0425f},
        {50.0000f, 3.1571f, -77.2803f, 50.0000f, 0.0000f, -82.7485f, 2.8615f},
        {50.0000f, 2.8361f, -74.0200f, 50.0000f, 0.0000f, -82.7485f, 3.4412f},
        {50.0000f, -1.3802f, -84.2814f, 50.0000f, 0.0000f, -82.7485f, 1.0000f},
        {50.0000f, -1.1848f, -84.8006f, 50.0000f, 0.0000f, -82.7485f, 1.0000f},
        {50.0000f, -0.9009f, -85.5211f, 50.0000f, 0.0000f, -82.7485f, 1.0000f},
        {50.0000f, 0.0000f, 0.0000f, 50.0000f, -1.0000f, 2.0000f, 2.3669f},
        {50.0000f, -1.0000f, 2.0000f, 50.0000f, 0.0000f, 0.0000f, 2.3669f},
        {50.0000f, 2.4900f, -0.0010f, 50.0000f, -2.4900f, 0.0009f, 7.1792f},
        {50.0000f, 2.4900f, -0.0010f, 50.0000f, -2.4900f, 0.0010f, 7.1792f},
        {50.0000f, 2.4900f, -0.0010f, 50.0000f, -2.4900f, 0.0011f, 7.2195f},
        {50.0000f, 2.4900f, -0.0010f, 50.0000f, -2.4900f, 0.0012f, 7.2195f},
        {50.0000f, -0.0010f, 2.4900f, 50.0000f, 0.0009f, -2.4900f, 4.8045f},
        {50.0000f, -0.0010f, 2.4900f, 50.0000f, 0.0010f, -2.4900f, 4.8045f},
        {50.0000f, -0.0010f, 2.4900f, 50.0000f, 0.0011f, -2.4900f, 4.7461f},
        {50.0000f, 2.5000f, 0.0000f, 50.0000f, 0.0000f, -2.5000f, 4.3065f},
        {50.0000f, 2.5000f, 0.0000f, 73.0000f, 25.0000f, -18.0000f, 27.1492f},
        {50.0000f, 2.5000f, 0.0000f, 61.0000f, -5.0000f, 29.0000f, 22.8977f},
        {50.0000f, 2.5000f, 0.0000f, 56.0000f, -27.0000f, -3.0000f, 31.9030f},
        {50.0000f, 2.5000f, 0.0000f, 58.0000f, 24.0000f, 15.0000f, 19.4535f},
        {50.0000f, 2.5000f, 0.0000f, 50.0000f, 3.1736f, 0.5854f, 1.0000f},
        {50.0000f, 2.5000f, 0.0000f, 50.0000f, 3.2972f, 0.0000f, 1.0000f},
        {50.0000f, 2.5000f, 0.0000f, 50.0000f, 1.8634f, 0.5757f, 1.0000f},
        {50.0000f, 2.5000f, 0.0000f, 50.0000f, 3.2592f, 0.3350f, 1.0000f},
        {60.2574f, -34.0099f, 36.2677f, 60.4626f, -34.1751f, 39.4387f, 1.2644f},
        {63.0109f, -31.0961f, -5.8663f, 62.8187f, -29.7946f, -4.0864f, 1.2630f},
        {61.2901f, 3.7196f, -5.3901f, 61.4292f, 2.2480f, -4.9620f, 1.8731f},
        {35.0831f, -44.1164f, 3.7933f, 35.0232f, -40.0716f, 1.5901f, 1.8645f},
        {22.7233f, 20.0904f, -46.6940f, 23.0331f, 14.9730f, -42.5619f, 2.0373f},
        {36.4612f, 47.8580f, 18.3852f, 36.2715f, 50.5065f, 21.2231f, 1.4146f},
        {90.8027f, -2.0831f, 1.4410f, 91.1528f, -1.6435f, 0.0447f, 1.4441f},
        {90.9257f, -0.5406f, -0.9208f, 88.6381f, -0.8985f, -0.7239f, 1.5381f},
        {6.7747f, -0.2908f, -2.4247f, 5.8714f, -0.0985f, -2.2286f, 0.6377f},
        {2.0776f, 0.0795f, -1.1350f, 0.9033f, -0.0636f, -0.5514f, 0.9082f},
    };

    // OKLab of sRGB bytes in double precision (Ottosson's matrices, the exact IEC curve)
    struct OklabReference { Rgba8 color; double l, a, b; };
    const OklabReference kOklab[] = {
        {{0, 0, 0}, 0.000000000, 0.000000000, 0.000000000},
        {{255, 255, 255}, 0.999999993, 0.000000000, 0.000000037},
        {{255, 0, 0}, 0.627955361, 0.224863061, 0.125846299},
        {{0, 255, 0}, 0.866439612, -0.233887574, 0.179498480},
        {{0, 0, 255}, 0.452013718, -0.032456984, -0.311528148},
        {{255, 255, 0}, 0.967982720, -0.071369080, 0.198569755},
        {{0, 255, 255}, 0.905399230, -0.149443940, -0.039398158},
        {{255, 0, 255}, 0.701673856, 0.274566294, -0.169156059},
        {{128, 128, 128}, 0.599870802, 0.000000000, 0.000000022},
        {{1, 1, 1}, 0.067204615, 0.000000000, 0.000000003},
        {{10, 20, 30}, 0.186989144, -0.008946053, -0.023703953},
        {{177, 62, 83}, 0.530828058, 0.145778889, 0.033453082},
        {{26, 28, 44}, 0.232418442, 0.004426785, -0.030700079},
        {{239, 125, 87}, 0.711288640, 0.116631672, 0.094403703},
        {{255, 205, 117}, 0.874431145, 0.020305284, 0.118613718},
        {{167, 240, 112}, 0.880039941, -0.121587324, 0.128177779},
        {{56, 183, 100}, 0.690999221, -0.142347848, 0.079481001},
        {{41, 54, 111}, 0.354389093, 0.001545555, -0.099484671},
        {{59, 93, 201}, 0.515211299, -0.009134745, -0.172208620},
        {{115, 239, 247}, 0.884352160, -0.103678434, -0.039876549},
        {{250, 1, 128}, 0.636673429, 0.256850958, 0.007404203},
        {{3, 250, 7}, 0.853729602, -0.229848591, 0.176061906},
        {{200, 30, 30}, 0.534915219, 0.179557723, 0.093909312},
        {{64, 32, 16}, 0.283666559, 0.038786752, 0.040374518},
    };

    // the golden of pixelColorHash() (Linux x86-64; the -O0 build and every platform must match)
    constexpr uint64_t kPixelColorHashGolden = 0xe165107b5f7284fcull;

    std::vector<Rgba8> testPalette() {
        // Endesga 32
        const char* hex[] = {"#be4a2f", "#d77643", "#ead4aa", "#e4a672", "#b86f50", "#733e39", "#3e2731", "#a22633",
                             "#e43b44", "#f77622", "#feae34", "#fee761", "#63c74d", "#3e8948", "#265c42", "#193c3e",
                             "#124e89", "#0099db", "#2ce8f5", "#ffffff", "#c0cbdc", "#8b9bb4", "#5a6988", "#3a4466",
                             "#262b44", "#181425", "#ff0044", "#68386c", "#b55088", "#f6757a", "#e8b796", "#c28569"};
        std::vector<Rgba8> colors;
        for (const char* text : hex) {
            Rgba8 color;
            EXPECT_TRUE(parseHex(text, color));
            colors.push_back(color);
        }
        return colors;
    }

}

TEST(PixelColorCiede2000, SharmaPairs) {
    for (size_t i = 0; i < sizeof(kSharma) / sizeof(kSharma[0]); i++) {
        const SharmaPair& p = kSharma[i];
        const float forward = ciede2000(Lab{p.l1, p.a1, p.b1}, Lab{p.l2, p.a2, p.b2});
        const float backward = ciede2000(Lab{p.l2, p.a2, p.b2}, Lab{p.l1, p.a1, p.b1});
        EXPECT_NEAR(forward, p.deltaE, 1e-4f) << "pair " << i + 1;
        EXPECT_NEAR(backward, p.deltaE, 1e-4f) << "pair " << i + 1 << " (swapped)";
    }
}

TEST(PixelColorCiede2000, CielabOfWhiteAndBlack) {
    const Lab white = toCielab(Rgba8{255, 255, 255});
    EXPECT_NEAR(white.l, 100.0f, 1e-3f);
    EXPECT_NEAR(white.a, 0.0f, 1e-3f);
    EXPECT_NEAR(white.b, 0.0f, 1e-3f);
    const Lab black = toCielab(Rgba8{0, 0, 0});
    EXPECT_NEAR(black.l, 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ(ciede2000(white, white), 0.0f);
}

TEST(PixelColorOklab, MatchesTheDoublePrecisionReference) {
    for (const OklabReference& ref : kOklab) {
        const Lab lab = toOklab(ref.color);
        EXPECT_NEAR(lab.l, ref.l, 1e-6) << toHex(ref.color);
        EXPECT_NEAR(lab.a, ref.a, 1e-6) << toHex(ref.color);
        EXPECT_NEAR(lab.b, ref.b, 1e-6) << toHex(ref.color);
    }
}

TEST(PixelColorOklab, OttossonXyzTable) {
    // Ottosson's published table is XYZ (D65) to OKLab, to 3 decimals; through linear sRGB
    struct Row { double x, y, z, l, a, b; };
    const Row rows[] = {
        {0.950, 1.000, 1.089, 1.000, 0.000, 0.000},
        {1.000, 0.000, 0.000, 0.450, 1.236, -0.019},
        {0.000, 1.000, 0.000, 0.922, -0.671, 0.263},
        {0.000, 0.000, 1.000, 0.153, -1.415, -0.449},
    };
    for (const Row& row : rows) {
        const double r = 3.2404542 * row.x - 1.5371385 * row.y - 0.4985314 * row.z;
        const double g = -0.9692660 * row.x + 1.8760108 * row.y + 0.0415560 * row.z;
        const double b = 0.0556434 * row.x - 0.2040259 * row.y + 1.0572252 * row.z;
        const Lab lab = linearToOklab((float)r, (float)g, (float)b);
        EXPECT_NEAR(lab.l, row.l, 1e-3);
        EXPECT_NEAR(lab.a, row.a, 1e-3);
        EXPECT_NEAR(lab.b, row.b, 1e-3);
    }
}

TEST(PixelColorOklab, RoundTripsEveryByteGrey) {
    for (int v = 0; v < 256; v++) {
        const Rgba8 color{(uint8_t)v, (uint8_t)v, (uint8_t)v};
        EXPECT_EQ(fromOklab(toOklab(color)), color) << v;
    }
    // and a spread of colours
    for (uint32_t k = 0; k < 4096; k++) {
        const Rgba8 color = unpackRgb((k * 2654435761u) & 0xFFFFFFu);
        EXPECT_EQ(fromOklab(toOklab(color)), color) << toHex(color);
    }
}

TEST(PixelColorOklab, OklchRoundTrip) {
    const Lab lab = toOklab(Rgba8{177, 62, 83});
    const LCh lch = toOklch(lab);
    EXPECT_GE(lch.h, 0.0f);
    EXPECT_LT(lch.h, 360.0f);
    const Lab back = fromOklch(lch);
    EXPECT_NEAR(back.l, lab.l, 1e-6f);
    EXPECT_NEAR(back.a, lab.a, 1e-6f);
    EXPECT_NEAR(back.b, lab.b, 1e-6f);
    EXPECT_FLOAT_EQ(toOklch(Lab{0.5f, 0.0f, 0.0f}).h, 0.0f);
}

TEST(PixelColorSrgb, EveryByteRoundTripsExactly) {
    for (int b = 0; b < 256; b++) {
        const float linear = srgbToLinear((uint8_t)b);
        EXPECT_EQ(linearToSrgb(linear), b);
        const double c = b / 255.0;
        const double expected = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        EXPECT_NEAR(linear, expected, 1e-7) << b;
        EXPECT_NEAR(srgbToLinearf((float)c), expected, 1e-7) << b;
        EXPECT_NEAR(linearToSrgbf(linear), c, 1e-6) << b;
        if (b > 0) {
            EXPECT_GT(linear, srgbToLinear((uint8_t)(b - 1)));
        }
    }
    EXPECT_EQ(linearToSrgb(-1.0f), 0);
    EXPECT_EQ(linearToSrgb(2.0f), 255);
    EXPECT_EQ(linearToSrgb(std::nanf("")), 0);
}

TEST(PixelColorSrgb, EngineConventionKeepsBytes) {
    // the renderer encodes linear colours with gamma 2.2 (includes/srgb.glsl): every byte through
    // toEngineLinear and back is itself
    for (int b = 0; b < 256; b++) {
        const Rgba8 color{(uint8_t)b, (uint8_t)(255 - b), (uint8_t)(b / 2), (uint8_t)b};
        const Vector4 linear = toEngineLinear(color);
        EXPECT_EQ(fromEngineLinear(linear), color) << b;
        EXPECT_NEAR(linear.x, std::pow(b / 255.0, 2.2), 1e-6) << b;
    }
}

TEST(PixelColorRoots, FixedNewtonMatchesPow) {
    for (int n : {3, 5, 11, 12}) {
        for (double x : {1e-30, 1e-7, 0.001, 0.5, 0.75, 1.0, 2.0, 27.0, 1000.0, 123456.789}) {
            const double expected = std::pow(x, 1.0 / n);
            EXPECT_NEAR(rootFixed(x, n) / expected, 1.0, 1e-14) << n << " " << x;
        }
    }
    EXPECT_EQ(rootFixed(0.0, 3), 0.0);
    EXPECT_EQ(rootFixed(-1.0, 3), 0.0);
    EXPECT_NEAR(cbrtFixed(-8.0), -2.0, 1e-15);
}

TEST(PixelColorDeterminism, UnoptimisedBuildMatches) {
    const uint64_t optimised = pixelColorHash();
    const uint64_t unoptimised = pixelColorHashUnoptimised();
    EXPECT_EQ(optimised, unoptimised);
    char text[32];
    std::snprintf(text, sizeof(text), "0x%016llxull", (unsigned long long)optimised);
    EXPECT_EQ(optimised, kPixelColorHashGolden) << "the colour maths changed its bits: " << text;
}

TEST(PixelColorHex, ParsesAndFormats) {
    Rgba8 color;
    bool alpha = false;
    EXPECT_TRUE(parseHex("#1A1c2C", color, &alpha));
    EXPECT_EQ(color, (Rgba8{0x1a, 0x1c, 0x2c, 255}));
    EXPECT_FALSE(alpha);
    EXPECT_TRUE(parseHex("00000000", color, &alpha));
    EXPECT_EQ(color, (Rgba8{0, 0, 0, 0}));
    EXPECT_TRUE(alpha);
    EXPECT_FALSE(parseHex("#12345", color));
    EXPECT_FALSE(parseHex("#12345g", color));
    EXPECT_EQ(toHex(Rgba8{0x1a, 0x1c, 0x2c, 255}), "#1a1c2c");
    EXPECT_EQ(toHex(Rgba8{0, 0, 0, 0}), "#00000000");
    EXPECT_EQ(toHex(Rgba8{1, 2, 3, 255}, true), "#010203ff");
}

TEST(PixelColorHsv, RoundTripsBytes) {
    for (uint32_t k = 0; k < 4096; k++) {
        const Rgba8 color = unpackRgb((k * 2246822519u) & 0xFFFFFFu);
        const Hsv hsv = toHsv(color);
        EXPECT_GE(hsv.h, 0.0f);
        EXPECT_LT(hsv.h, 1.0f);
        EXPECT_EQ(fromHsv(hsv), color) << toHex(color);
    }
}

TEST(PixelColorMetric, Names) {
    for (ColorMetric metric : {ColorMetric::OkLab, ColorMetric::Ciede2000, ColorMetric::Rgb}) {
        ColorMetric parsed = ColorMetric::Rgb;
        EXPECT_TRUE(parseColorMetric(colorMetricName(metric), parsed));
        EXPECT_EQ(parsed, metric);
    }
    ColorMetric parsed;
    EXPECT_FALSE(parseColorMetric("lab", parsed));
    EXPECT_FLOAT_EQ(distance(Rgba8{0, 0, 0}, Rgba8{255, 255, 255}, ColorMetric::Rgb), std::sqrt(3.0f));
    EXPECT_NEAR(distance(Rgba8{0, 0, 0}, Rgba8{255, 255, 255}, ColorMetric::OkLab), 1.0f, 1e-6f);
    EXPECT_NEAR(distance(Rgba8{0, 0, 0}, Rgba8{255, 255, 255}, ColorMetric::Ciede2000), 100.0f, 1e-2f);
}

TEST(PaletteMatcher, FindsTheNearestByEveryMetric) {
    // ritmo's case
    const std::vector<Rgba8> palette = {{0, 0, 0}, {255, 255, 255}, {200, 30, 30}};
    for (ColorMetric metric : {ColorMetric::OkLab, ColorMetric::Ciede2000, ColorMetric::Rgb}) {
        PaletteMatcher matcher(palette, metric);
        EXPECT_EQ(matcher.nearest(Rgba8{20, 10, 10}), 0);
        EXPECT_EQ(matcher.nearest(Rgba8{240, 230, 235}), 1);
        EXPECT_EQ(matcher.nearest(Rgba8{180, 50, 40}), 2);
        EXPECT_EQ(matcher.nearestWithDistance(Rgba8{200, 30, 30}).second, 0.0f);
    }
}

TEST(PaletteMatcher, TiesGoToTheLowerIndexAndTransparentSlotsAreSkipped) {
    const std::vector<Rgba8> palette = {{0, 0, 0, 0}, {10, 10, 10}, {10, 10, 10}, {250, 250, 250}};
    PaletteMatcher matcher(palette, ColorMetric::OkLab);
    EXPECT_EQ(matcher.nearest(Rgba8{0, 0, 0}), 1);
    EXPECT_EQ(matcher.nearest(Rgba8{10, 10, 10}), 1);
    const std::vector<uint16_t> only = {3};
    PaletteMatcher restricted(palette, ColorMetric::OkLab, only);
    EXPECT_EQ(restricted.nearest(Rgba8{0, 0, 0}), 3);
    PaletteMatcher empty(std::vector<Rgba8>{}, ColorMetric::OkLab);
    EXPECT_EQ(empty.nearest(Rgba8{0, 0, 0}), -1);
}

TEST(PaletteMatcher, CachedAndUncachedAgreeOnSampledColours) {
    const std::vector<Rgba8> palette = testPalette();
    for (ColorMetric metric : {ColorMetric::OkLab, ColorMetric::Rgb, ColorMetric::Ciede2000}) {
        PaletteMatcher matcher(palette, metric);
        // every 64th colour of the 2^24 (every 4096th for CIEDE2000, the slow metric), twice:
        // misses, then hits
        const uint32_t step = metric == ColorMetric::Ciede2000 ? 4096u : 64u;
        int mismatches = 0;
        for (int pass = 0; pass < 2; pass++) {
            for (uint32_t rgb = 0; rgb < (1u << 24); rgb += step) {
                const Rgba8 color = unpackRgb(rgb);
                const std::pair<int, float> cached = matcher.nearestWithDistance(color);
                const std::pair<int, float> uncached = matcher.nearestUncached(color);
                if (cached != uncached) mismatches++;
            }
        }
        EXPECT_EQ(mismatches, 0) << colorMetricName(metric);
    }
}

TEST(PaletteMatcher, DitherPair) {
    const std::vector<Rgba8> palette = {{0, 0, 0}, {255, 255, 255}, {255, 0, 0}};
    PaletteMatcher matcher(palette, ColorMetric::OkLab);
    // a palette colour is itself, with dithering on
    PaletteMatcher::Mix mix = matcher.ditherPair(Rgba8{255, 0, 0}, 1.0f);
    EXPECT_EQ(mix.a, 2);
    EXPECT_EQ(mix.ratio16, 0);
    // a dither averages in linear light: a grey of linear 0.5 (sRGB 188) is half black, half white
    const Rgba8 grey{188, 188, 188};
    mix = matcher.ditherPair(grey, 1.0f);
    ASSERT_TRUE((mix.a == 0 && mix.b == 1) || (mix.a == 1 && mix.b == 0));
    EXPECT_EQ(mix.a == 0 ? mix.ratio16 : 16 - mix.ratio16, 8);
    // no strength: the nearest alone
    mix = matcher.ditherPair(grey, 0.0f);
    EXPECT_EQ(mix.ratio16, 0);
    EXPECT_EQ(mix.a, matcher.nearestUncached(grey).first);
    // the ratio never passes the strength
    for (int r = 0; r < 256; r += 5) {
        mix = matcher.ditherPair(Rgba8{(uint8_t)r, (uint8_t)r, (uint8_t)r}, 0.25f);
        EXPECT_LE(mix.ratio16, 4);
    }
}

// the mix a dither shows is the average of its two colours' light: for every grey, the ratio of
// white over black is the grey's linear value (within a step), in every metric (sRGB 128 is
// linear 0.216: 3 or 4 white cells of 16, not the 8 or more a mix in OKLab or sRGB would give)
TEST(PaletteMatcher, DitherMixesInLinearLight) {
    const std::vector<Rgba8> palette = {{0, 0, 0}, {255, 255, 255}};
    for (ColorMetric metric : {ColorMetric::OkLab, ColorMetric::Ciede2000, ColorMetric::Rgb}) {
        PaletteMatcher matcher(palette, metric);
        for (int value = 16; value < 256; value += 16) {
            const PaletteMatcher::Mix mix = matcher.ditherPair(Rgba8{(uint8_t)value, (uint8_t)value, (uint8_t)value}, 1.0f, 16);
            const int white = mix.ratio16 == 0 ? (mix.a == 1 ? 16 : 0) : (mix.b == 1 ? mix.ratio16 : 16 - mix.ratio16);
            const float wanted = srgbToLinearf(value / 255.0f) * 16.0f;
            EXPECT_LE(std::abs(white - wanted), 1.0f) << colorMetricName(metric) << " grey " << value << ": " << white << " white of 16";
        }
        const PaletteMatcher::Mix mid = matcher.ditherPair(Rgba8{128, 128, 128}, 1.0f, 16);
        const int white = mid.b == 1 ? mid.ratio16 : 16 - mid.ratio16;
        EXPECT_GE(white, 3) << colorMetricName(metric);
        EXPECT_LE(white, 4) << colorMetricName(metric);
    }
}

TEST(PixelColorReduce, MedianCutKeepsFewColoursExactly) {
    std::vector<Rgba8> pixels;
    for (int i = 0; i < 100; i++) pixels.push_back(Rgba8{10, 20, 30});
    for (int i = 0; i < 50; i++) pixels.push_back(Rgba8{200, 100, 50});
    for (int i = 0; i < 10; i++) pixels.push_back(Rgba8{0, 255, 0});
    const std::vector<Rgba8> colors = medianCut(pixels, 8, {});
    ASSERT_EQ(colors.size(), 3u);
    EXPECT_EQ(colors[0], (Rgba8{10, 20, 30}));   // most used first
    EXPECT_EQ(colors[1], (Rgba8{200, 100, 50}));
    EXPECT_EQ(colors[2], (Rgba8{0, 255, 0}));
}

TEST(PixelColorReduce, MedianCutIsDeterministicAndKeepsLockedColours) {
    std::vector<Rgba8> pixels;
    for (uint32_t k = 0; k < 20000; k++) pixels.push_back(unpackRgb((k * 2654435761u) & 0xFFFFFFu));
    const std::vector<Rgba8> keep = {{255, 0, 255}};
    const std::vector<Rgba8> first = medianCut(pixels, 16, keep);
    const std::vector<Rgba8> second = medianCut(pixels, 16, keep);
    EXPECT_EQ(first, second);
    ASSERT_LE(first.size(), 16u);
    EXPECT_GE(first.size(), 12u);
    EXPECT_EQ(first[0], (Rgba8{255, 0, 255}));
    const std::vector<Rgba8> refined = kMeansRefine(pixels, first, 1, 8);
    EXPECT_EQ(refined, kMeansRefine(pixels, first, 1, 8));
    EXPECT_EQ(refined[0], (Rgba8{255, 0, 255}));
    EXPECT_LE(refined.size(), first.size());
    EXPECT_EQ(medianCut(pixels, 1, keep), keep);
    EXPECT_TRUE(medianCut(pixels, 0, keep).empty());
}
