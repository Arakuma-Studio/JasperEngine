// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The OKLab matrices are Björn Ottosson's ("A perceptual color space for image processing",
// 2020, public domain/MIT); CIELAB, CIEDE2000 and the cached matcher are ported from ritmo
// (packages/tools/src/pixel/core/color.ts, MIT, Copyright (c) 2026 Sandro Maglione); fitRamp and rampAt
// are pixelfx's fit() and at() (src/core/color.js, MIT, Copyright (c) 2026 Patch). NOTICE.md.

#include "PixelColor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::pixelart;

// file-local, inside the namespace: a unity build's other sources never see these names
namespace jasperengine::pixelart {
namespace {

    constexpr double kPi = 3.14159265358979323846;

    // murmur3's finaliser: every bit of a colour key reaches the low bits the tables index by
    uint32_t mixKey(uint32_t key) {
        key ^= key >> 16;
        key *= 0x85ebca6bu;
        key ^= key >> 13;
        key *= 0xc2b2ae35u;
        key ^= key >> 16;
        return key;
    }

    double powInt(double x, int n) {
        double p = 1.0;
        for (int i = 0; i < n; i++) p *= x;
        return p;
    }

    // ((c + 0.055) / 1.055)^2.4 = t^2 (t^2)^(1/5)
    double decodePiecewise(double c) {
        if (c <= 0.04045) return c / 12.92;
        const double t = (c + 0.055) / 1.055;
        return t * t * rootFixed(t * t, 5);
    }

    // 1.055 x^(1/2.4) - 0.055 = 1.055 (x^5)^(1/12) - 0.055
    double encodePiecewise(double x) {
        if (x <= 0.0031308) return 12.92 * x;
        return 1.055 * rootFixed(powInt(x, 5), 12) - 0.055;
    }

    struct SrgbTables {
        float toLinear[256];
        // the linear value where the rounded byte steps from b to b + 1
        float thresholds[255];

        SrgbTables() {
            for (int b = 0; b < 256; b++) toLinear[b] = (float)decodePiecewise(b / 255.0);
            for (int b = 0; b < 255; b++) thresholds[b] = (float)decodePiecewise((b + 0.5) / 255.0);
        }
    };

    const SrgbTables& srgbTables() {
        static const SrgbTables tables;
        return tables;
    }

    double clamp01(double v) {
        return v > 0.0 ? (v < 1.0 ? v : 1.0) : 0.0;
    }

    uint8_t roundByte(double v01) {
        return (uint8_t)std::floor(clamp01(v01) * 255.0 + 0.5);
    }

    int hexDigit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    Lab oklabOfLinear(double r, double g, double b) {
        const double l = cbrtFixed(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
        const double m = cbrtFixed(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
        const double s = cbrtFixed(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
        return Lab{(float)(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s),
                   (float)(1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s),
                   (float)(0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s)};
    }

    // A dither mix's cube root (ditherPair measures a few per LUT cell): float Newton steps from
    // the exponent's third, the same basic operations everywhere (no libm), within 1e-7 of
    // cbrtFixed over [0, 1]
    float cbrtMix(float x) {
        if (!(x > 0.0f)) return 0.0f;
        uint32_t bits;
        std::memcpy(&bits, &x, sizeof(bits));
        bits = bits / 3u + 709921077u;
        float y;
        std::memcpy(&y, &bits, sizeof(y));
        for (int i = 0; i < 4; i++) y = (2.0f * y + x / (y * y)) / 3.0f;
        return y;
    }

    Lab oklabOfMix(float r, float g, float b) {
        const float l = cbrtMix(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
        const float m = cbrtMix(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
        const float s = cbrtMix(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
        return Lab{0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
                   1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
                   0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
    }

    void linearOfOklab(const Lab& lab, double out[3]) {
        const double l = powInt(lab.l + 0.3963377774 * lab.a + 0.2158037573 * lab.b, 3);
        const double m = powInt(lab.l - 0.1055613458 * lab.a - 0.0638541728 * lab.b, 3);
        const double s = powInt(lab.l - 0.0894841775 * lab.a - 1.2914855480 * lab.b, 3);
        out[0] = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
        out[1] = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
        out[2] = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    }

    double labF(double t) {
        return t > 216.0 / 24389.0 ? cbrtFixed(t) : ((24389.0 / 27.0) * t + 16.0) / 116.0;
    }

    double pow7(double v) {
        return powInt(v, 7);
    }

    double degrees(double radians) { return radians * 180.0 / kPi; }
    double radians(double degreesValue) { return degreesValue * kPi / 180.0; }

    float squaredDistance(const Lab& x, const Lab& y) {
        const float dl = x.l - y.l, da = x.a - y.a, db = x.b - y.b;
        return dl * dl + da * da + db * db;
    }

    // The unique colours of an image (alpha ignored) with their pixel counts, sorted by key: an
    // open-addressed count table, since a picture repeats few colours many times
    struct ColorCount { uint32_t rgb = 0; uint32_t count = 0; Lab lab; };

    std::vector<ColorCount> countColors(std::span<const Rgba8> pixels) {
        size_t capacity = 1024;
        std::vector<uint32_t> keys(capacity, 0);
        std::vector<uint32_t> counts(capacity, 0);
        size_t used = 0;
        auto insert = [&](uint32_t key, uint32_t count) {
            size_t mask = keys.size() - 1;
            size_t at = (size_t)(mixKey(key) & mask);
            while (keys[at] != 0 && keys[at] != key) at = (at + 1) & mask;
            if (keys[at] == 0) {
                keys[at] = key;
                used++;
            }
            counts[at] += count;
        };
        for (const Rgba8& pixel : pixels) {
            if (used * 2 >= keys.size()) {
                std::vector<uint32_t> oldKeys = std::move(keys);
                std::vector<uint32_t> oldCounts = std::move(counts);
                capacity *= 2;
                keys.assign(capacity, 0);
                counts.assign(capacity, 0);
                used = 0;
                for (size_t i = 0; i < oldKeys.size(); i++) {
                    if (oldKeys[i] != 0) insert(oldKeys[i], oldCounts[i]);
                }
            }
            insert(packRgb(pixel) + 1u, 1);
        }
        std::vector<ColorCount> colors;
        colors.reserve(used);
        for (size_t i = 0; i < keys.size(); i++) {
            if (keys[i] != 0) colors.push_back({keys[i] - 1u, counts[i], Lab{}});
        }
        std::sort(colors.begin(), colors.end(), [](const ColorCount& x, const ColorCount& y) { return x.rgb < y.rgb; });
        for (ColorCount& color : colors) color.lab = toOklab(unpackRgb(color.rgb));
        return colors;
    }

    Lab weightedMean(const std::vector<const ColorCount*>& members) {
        double l = 0.0, a = 0.0, b = 0.0, total = 0.0;
        for (const ColorCount* member : members) {
            l += (double)member->lab.l * member->count;
            a += (double)member->lab.a * member->count;
            b += (double)member->lab.b * member->count;
            total += member->count;
        }
        if (total <= 0.0) return Lab{};
        return Lab{(float)(l / total), (float)(a / total), (float)(b / total)};
    }

    float axisValue(const Lab& lab, int axis) {
        return axis == 0 ? lab.l : (axis == 1 ? lab.a : lab.b);
    }

    // appends `color` unless its RGB is there already
    void appendUnique(std::vector<Rgba8>& colors, Rgba8 color) {
        for (const Rgba8& existing : colors) {
            if (packRgb(existing) == packRgb(color)) return;
        }
        colors.push_back(color);
    }

}
}

const char* pixelart::colorMetricName(ColorMetric metric) {
    switch (metric) {
        case ColorMetric::OkLab: return "oklab";
        case ColorMetric::Ciede2000: return "ciede2000";
        case ColorMetric::Rgb: return "rgb";
    }
    return "oklab";
}

bool pixelart::parseColorMetric(std::string_view name, ColorMetric& out) {
    for (ColorMetric metric : {ColorMetric::OkLab, ColorMetric::Ciede2000, ColorMetric::Rgb}) {
        if (name == colorMetricName(metric)) {
            out = metric;
            return true;
        }
    }
    return false;
}

std::string pixelart::toHex(Rgba8 color, bool alpha) {
    char text[16];
    if (alpha) {
        std::snprintf(text, sizeof(text), "#%02x%02x%02x%02x", color.r, color.g, color.b, color.a);
    } else {
        std::snprintf(text, sizeof(text), "#%02x%02x%02x", color.r, color.g, color.b);
    }
    return text;
}

std::string pixelart::toHex(Rgba8 color) {
    return toHex(color, color.a != 255);
}

bool pixelart::parseHex(std::string_view text, Rgba8& out, bool* hasAlpha) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    if (text.size() != 6 && text.size() != 8) return false;
    uint8_t bytes[4] = {0, 0, 0, 255};
    for (size_t i = 0; i < text.size() / 2; i++) {
        const int hi = hexDigit(text[i * 2]);
        const int lo = hexDigit(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes[i] = (uint8_t)(hi * 16 + lo);
    }
    out = Rgba8{bytes[0], bytes[1], bytes[2], bytes[3]};
    if (hasAlpha) *hasAlpha = text.size() == 8;
    return true;
}

double pixelart::rootFixed(double x, int n) {
    if (!(x > 0.0) || n < 2) return 0.0;
    if (std::isinf(x)) return x;
    // x = m 2^e with m in [0.5, 1); x = (m 2^r) 2^(q n) with r in [0, n)
    int e = 0;
    const double m = std::frexp(x, &e);
    int q = e / n;
    int r = e % n;
    if (r < 0) {
        r += n;
        q -= 1;
    }
    const double a = std::ldexp(m, r);
    // a^(1/n) <= 2^(r/n) <= 1 + r/n: starting above the root, Newton descends to it
    // monotonically (y^n is convex), quadratically once close; ten steps reach double
    // precision for every n used here (3, 5, 11, 12)
    double y = 1.0 + (double)r / (double)n;
    for (int i = 0; i < 10; i++) {
        y = ((double)(n - 1) * y + a / powInt(y, n - 1)) / (double)n;
    }
    return std::ldexp(y, q);
}

float pixelart::srgbToLinear(uint8_t c) {
    return srgbTables().toLinear[c];
}

uint8_t pixelart::linearToSrgb(float linear) {
    if (!(linear > 0.0f)) return 0;
    const SrgbTables& tables = srgbTables();
    return (uint8_t)(std::upper_bound(tables.thresholds, tables.thresholds + 255, linear) - tables.thresholds);
}

float pixelart::srgbToLinearf(float c) {
    return c > 0.0f ? (float)decodePiecewise(c) : 0.0f;
}

float pixelart::linearToSrgbf(float linear) {
    return linear > 0.0f ? (float)encodePiecewise(linear) : 0.0f;
}

float pixelart::engineSrgbToLinear(float c) {
    // c^2.2 = c^2 c^(1/5)
    if (!(c > 0.0f)) return 0.0f;
    const double v = c;
    return (float)(v * v * rootFixed(v, 5));
}

float pixelart::engineLinearToSrgb(float linear) {
    // x^(1/2.2) = (x^5)^(1/11)
    if (!(linear > 0.0f)) return 0.0f;
    return (float)rootFixed(powInt(linear, 5), 11);
}

Vector4 pixelart::toEngineLinear(Rgba8 color) {
    return Vector4(engineSrgbToLinear(color.r / 255.0f), engineSrgbToLinear(color.g / 255.0f),
                   engineSrgbToLinear(color.b / 255.0f), color.a / 255.0f);
}

Rgba8 pixelart::fromEngineLinear(const Vector4& linear) {
    return Rgba8{roundByte(engineLinearToSrgb(linear.x)), roundByte(engineLinearToSrgb(linear.y)),
                 roundByte(engineLinearToSrgb(linear.z)), roundByte(linear.w)};
}

Lab pixelart::toOklab(Rgba8 color) {
    const SrgbTables& tables = srgbTables();
    return oklabOfLinear(tables.toLinear[color.r], tables.toLinear[color.g], tables.toLinear[color.b]);
}

Lab pixelart::linearToOklab(float r, float g, float b) {
    return oklabOfLinear(r, g, b);
}

void pixelart::oklabToLinear(const Lab& lab, float rgb[3]) {
    double linear[3];
    linearOfOklab(lab, linear);
    for (int i = 0; i < 3; i++) rgb[i] = (float)linear[i];
}

Rgba8 pixelart::fromOklab(const Lab& lab, uint8_t alpha) {
    double linear[3];
    linearOfOklab(lab, linear);
    return Rgba8{linearToSrgb((float)linear[0]), linearToSrgb((float)linear[1]), linearToSrgb((float)linear[2]), alpha};
}

LCh pixelart::toOklch(const Lab& lab) {
    const double c = std::sqrt((double)lab.a * lab.a + (double)lab.b * lab.b);
    double h = 0.0;
    if (c > 0.0) {
        h = degrees(std::atan2((double)lab.b, (double)lab.a));
        if (h < 0.0) h += 360.0;
        if (h >= 360.0) h -= 360.0;
    }
    return LCh{lab.l, (float)c, (float)h};
}

Lab pixelart::fromOklch(const LCh& lch) {
    const double h = radians(lch.h);
    return Lab{lch.l, (float)(lch.c * std::cos(h)), (float)(lch.c * std::sin(h))};
}

namespace jasperengine::pixelart {
namespace {

    Lab cielabOfLinear(double r, double g, double b) {
        const double x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047;
        const double y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
        const double z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883;
        const double fx = labF(x), fy = labF(y), fz = labF(z);
        return Lab{(float)(116.0 * fy - 16.0), (float)(500.0 * (fx - fy)), (float)(200.0 * (fy - fz))};
    }

}
}

Lab pixelart::toCielab(Rgba8 color) {
    const SrgbTables& tables = srgbTables();
    return cielabOfLinear(tables.toLinear[color.r], tables.toLinear[color.g], tables.toLinear[color.b]);
}

float pixelart::ciede2000(const Lab& first, const Lab& second) {
    const double POW25_7 = pow7(25.0);
    const double l1 = first.l, a1 = first.a, b1 = first.b;
    const double l2 = second.l, a2 = second.a, b2 = second.b;
    const double c1 = std::sqrt(a1 * a1 + b1 * b1);
    const double c2 = std::sqrt(a2 * a2 + b2 * b2);
    const double cMean = (c1 + c2) / 2.0;
    const double g = 0.5 * (1.0 - std::sqrt(pow7(cMean) / (pow7(cMean) + POW25_7)));
    const double a1p = (1.0 + g) * a1;
    const double a2p = (1.0 + g) * a2;
    const double c1p = std::sqrt(a1p * a1p + b1 * b1);
    const double c2p = std::sqrt(a2p * a2p + b2 * b2);
    auto hue = [](double b, double a) {
        if (a == 0.0 && b == 0.0) return 0.0;
        const double h = degrees(std::atan2(b, a));
        return h < 0.0 ? h + 360.0 : h;
    };
    const double h1 = hue(b1, a1p);
    const double h2 = hue(b2, a2p);

    const double dL = l2 - l1;
    const double dC = c2p - c1p;
    // Hues exactly half a turn apart (Sharma's pair 10) sit on the formula's discontinuity:
    // rounding noise of a few ulps must not move them across it, so the boundary tests allow it
    const double halfTurn = 180.0 + 1e-6;
    double dh = 0.0;
    if (c1p * c2p != 0.0) {
        dh = h2 - h1;
        if (dh > halfTurn) dh -= 360.0;
        else if (dh < -halfTurn) dh += 360.0;
    }
    const double dH = 2.0 * std::sqrt(c1p * c2p) * std::sin(radians(dh / 2.0));

    const double lMean = (l1 + l2) / 2.0;
    const double cpMean = (c1p + c2p) / 2.0;
    double hMean = h1 + h2;
    if (c1p * c2p != 0.0) {
        if (std::fabs(h1 - h2) <= halfTurn) hMean = (h1 + h2) / 2.0;
        else if (h1 + h2 < 360.0) hMean = (h1 + h2 + 360.0) / 2.0;
        else hMean = (h1 + h2 - 360.0) / 2.0;
    }
    const double t = 1.0 - 0.17 * std::cos(radians(hMean - 30.0)) + 0.24 * std::cos(radians(2.0 * hMean)) +
                     0.32 * std::cos(radians(3.0 * hMean + 6.0)) - 0.20 * std::cos(radians(4.0 * hMean - 63.0));
    const double theta = (hMean - 275.0) / 25.0;
    const double dTheta = 30.0 * std::exp(-(theta * theta));
    const double rc = 2.0 * std::sqrt(pow7(cpMean) / (pow7(cpMean) + POW25_7));
    const double lm50 = (lMean - 50.0) * (lMean - 50.0);
    const double sl = 1.0 + (0.015 * lm50) / std::sqrt(20.0 + lm50);
    const double sc = 1.0 + 0.045 * cpMean;
    const double sh = 1.0 + 0.015 * cpMean * t;
    const double rt = -std::sin(radians(2.0 * dTheta)) * rc;
    const double lTerm = dL / sl;
    const double cTerm = dC / sc;
    const double hTerm = dH / sh;
    return (float)std::sqrt(lTerm * lTerm + cTerm * cTerm + hTerm * hTerm + rt * cTerm * hTerm);
}

Hsv pixelart::toHsv(float r, float g, float b) {
    const float maxC = std::max({r, g, b});
    const float minC = std::min({r, g, b});
    const float delta = maxC - minC;
    float h = 0.0f;
    if (delta > 0.0f) {
        if (maxC == r) h = std::fmod((g - b) / delta, 6.0f);
        else if (maxC == g) h = (b - r) / delta + 2.0f;
        else h = (r - g) / delta + 4.0f;
        h /= 6.0f;
        if (h < 0.0f) h += 1.0f;
    }
    const float s = maxC > 0.0f ? delta / maxC : 0.0f;
    return Hsv{h, s, maxC};
}

void pixelart::fromHsv(const Hsv& hsv, float rgb[3]) {
    const float h = hsv.h - std::floor(hsv.h);
    const float s = std::isfinite(hsv.s) ? std::clamp(hsv.s, 0.0f, 1.0f) : 0.0f;
    const float v = hsv.v;
    const float c = v * s;
    const float hp = h * 6.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (hp < 1.0f) { r = c; g = x; }
    else if (hp < 2.0f) { r = x; g = c; }
    else if (hp < 3.0f) { g = c; b = x; }
    else if (hp < 4.0f) { g = x; b = c; }
    else if (hp < 5.0f) { r = x; b = c; }
    else { r = c; b = x; }
    const float m = v - c;
    rgb[0] = r + m;
    rgb[1] = g + m;
    rgb[2] = b + m;
}

Hsv pixelart::toHsv(Rgba8 color) {
    return toHsv(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f);
}

Rgba8 pixelart::fromHsv(const Hsv& hsv, uint8_t alpha) {
    float rgb[3];
    fromHsv(hsv, rgb);
    return Rgba8{roundByte(rgb[0]), roundByte(rgb[1]), roundByte(rgb[2]), alpha};
}

float pixelart::distance(Rgba8 x, Rgba8 y, ColorMetric metric) {
    switch (metric) {
        case ColorMetric::OkLab:
            return std::sqrt(squaredDistance(toOklab(x), toOklab(y)));
        case ColorMetric::Ciede2000:
            return ciede2000(toCielab(x), toCielab(y));
        case ColorMetric::Rgb: {
            const float dr = (x.r - y.r) / 255.0f, dg = (x.g - y.g) / 255.0f, db = (x.b - y.b) / 255.0f;
            return std::sqrt(dr * dr + dg * dg + db * db);
        }
    }
    return 0.0f;
}

PaletteMatcher::PaletteMatcher(std::span<const Rgba8> colors, ColorMetric metric, std::span<const uint16_t> only)
    : palette(colors.begin(), colors.end()), colorMetric(metric) {
    labs.reserve(palette.size());
    linears.reserve(palette.size());
    const SrgbTables& tables = srgbTables();
    for (const Rgba8& color : palette) {
        labs.push_back(space(color));
        linears.push_back({tables.toLinear[color.r], tables.toLinear[color.g], tables.toLinear[color.b]});
    }
    if (!only.empty()) {
        for (uint16_t index : only) {
            if (index < palette.size() && std::find(candidateIndices.begin(), candidateIndices.end(), index) == candidateIndices.end()) {
                candidateIndices.push_back(index);
            }
        }
        std::sort(candidateIndices.begin(), candidateIndices.end());
    } else {
        for (size_t i = 0; i < palette.size(); i++) {
            if (palette[i].a != 0) candidateIndices.push_back((uint16_t)i);
        }
        if (candidateIndices.empty()) {
            for (size_t i = 0; i < palette.size(); i++) candidateIndices.push_back((uint16_t)i);
        }
    }
}

Lab PaletteMatcher::space(Rgba8 color) const {
    switch (colorMetric) {
        case ColorMetric::OkLab: return toOklab(color);
        case ColorMetric::Ciede2000: return toCielab(color);
        case ColorMetric::Rgb: return Lab{color.r / 255.0f, color.g / 255.0f, color.b / 255.0f};
    }
    return toOklab(color);
}

Lab PaletteMatcher::spaceOfLinear(const float rgb[3]) const {
    switch (colorMetric) {
        case ColorMetric::OkLab: return oklabOfMix(rgb[0], rgb[1], rgb[2]);
        case ColorMetric::Ciede2000: return cielabOfLinear(rgb[0], rgb[1], rgb[2]);
        case ColorMetric::Rgb: return Lab{linearToSrgbf(rgb[0]), linearToSrgbf(rgb[1]), linearToSrgbf(rgb[2])};
    }
    return oklabOfMix(rgb[0], rgb[1], rgb[2]);
}

// what the search compares: squared for the Euclidean metrics (the root is taken at the end)
float PaletteMatcher::metricDistance(const Lab& x, const Lab& y) const {
    return colorMetric == ColorMetric::Ciede2000 ? ciede2000(x, y) : squaredDistance(x, y);
}

std::pair<int, float> PaletteMatcher::nearestUncached(Rgba8 color) const {
    if (candidateIndices.empty()) return {-1, std::numeric_limits<float>::infinity()};
    const Lab lab = space(color);
    int best = -1;
    float bestDistance = std::numeric_limits<float>::infinity();
    for (uint16_t index : candidateIndices) {
        const float d = metricDistance(lab, labs[index]);
        if (best < 0 || d < bestDistance) {
            best = index;
            bestDistance = d;
        }
    }
    if (colorMetric != ColorMetric::Ciede2000) bestDistance = std::sqrt(bestDistance);
    return {best, bestDistance};
}

std::pair<int, float> PaletteMatcher::nearestWithDistance(Rgba8 color) {
    const uint32_t key = packRgb(color) + 1u;
    if (cache.empty()) cache.assign(1024, Slot{});
    size_t mask = cache.size() - 1;
    size_t at = (size_t)(mixKey(key) & mask);
    while (cache[at].key != 0) {
        if (cache[at].key == key) return {cache[at].index, cache[at].distance};
        at = (at + 1) & mask;
    }
    const std::pair<int, float> found = nearestUncached(color);
    if (found.first < 0) return found;
    if ((cacheUsed + 1) * 2 > cache.size()) {
        std::vector<Slot> old = std::move(cache);
        cache.assign(old.size() * 2, Slot{});
        mask = cache.size() - 1;
        for (const Slot& slot : old) {
            if (slot.key == 0) continue;
            size_t to = (size_t)(mixKey(slot.key) & mask);
            while (cache[to].key != 0) to = (to + 1) & mask;
            cache[to] = slot;
        }
        at = (size_t)(mixKey(key) & mask);
        while (cache[at].key != 0) at = (at + 1) & mask;
    }
    cache[at] = Slot{key, (uint16_t)found.first, found.second};
    cacheUsed++;
    return found;
}

int PaletteMatcher::nearest(Rgba8 color) {
    return nearestWithDistance(color).first;
}

PaletteMatcher::Mix PaletteMatcher::ditherPair(Rgba8 color, float strength, int steps) const {
    Mix mix;
    if (candidateIndices.empty()) return mix;
    const Lab lab = space(color);

    // the 8 nearest, in order (ties to the lower index)
    constexpr int kNearest = 8;
    std::pair<float, uint16_t> nearestList[kNearest];
    int found = 0;
    for (uint16_t index : candidateIndices) {
        const std::pair<float, uint16_t> entry{metricDistance(lab, labs[index]), index};
        int at = found;
        while (at > 0 && entry.first < nearestList[at - 1].first) at--;
        if (at >= kNearest) continue;
        const int last = std::min(found, kNearest - 1);
        for (int i = last; i > at; i--) nearestList[i] = nearestList[i - 1];
        nearestList[at] = entry;
        if (found < kNearest) found++;
    }

    mix.a = mix.b = nearestList[0].second;
    float bestCost = metricDistance(lab, labs[mix.a]);
    if (bestCost == 0.0f) return mix;   // a palette colour stays itself
    steps = std::clamp(steps, 1, 64);
    const float stepCount = (float)steps;
    const int maxSteps = (int)std::floor(std::clamp(std::isfinite(strength) ? strength : 0.0f, 0.0f, 1.0f) * stepCount + 1e-4f);
    if (maxSteps <= 0) return mix;

    // A dither shows (1 - r) A + r B as the eye averages it: in linear light. Each ordered pair of
    // the nearest gets its best r in linear RGB (the target's projection on the segment); the
    // three pairs that come closest there are measured in the chosen metric at the steps around
    // their r, with Yliluoma's penalty against mixing far colours, 0.01 r (1 - r) |A - B| (his
    // 0.1 outweighs a near-white grey's whole error in OKLab: it would never be dithered).
    const SrgbTables& tables = srgbTables();
    const float target[3] = {tables.toLinear[color.r], tables.toLinear[color.g], tables.toLinear[color.b]};
    struct PairFit { float residual; int i; int j; float r; };
    constexpr int kKept = 3;
    PairFit kept[kKept];
    int keptCount = 0;
    for (int i = 0; i < found; i++) {
        for (int j = 0; j < found; j++) {
            if (i == j) continue;
            const std::array<float, 3>& a = linears[nearestList[i].second];
            const std::array<float, 3>& b = linears[nearestList[j].second];
            const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
            const float e[3] = {target[0] - a[0], target[1] - a[1], target[2] - a[2]};
            const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (dd <= 0.0f) continue;
            const float r = std::clamp((e[0] * d[0] + e[1] * d[1] + e[2] * d[2]) / dd, 0.0f, 1.0f);
            const float off[3] = {e[0] - r * d[0], e[1] - r * d[1], e[2] - r * d[2]};
            const PairFit fit{off[0] * off[0] + off[1] * off[1] + off[2] * off[2], i, j, r};
            int at = keptCount;
            while (at > 0 && fit.residual < kept[at - 1].residual) at--;
            if (at >= kKept) continue;
            for (int k = std::min(keptCount, kKept - 1); k > at; k--) kept[k] = kept[k - 1];
            kept[at] = fit;
            if (keptCount < kKept) keptCount++;
        }
    }
    for (int k = 0; k < keptCount; k++) {
        const uint16_t ia = nearestList[kept[k].i].second, ib = nearestList[kept[k].j].second;
        const std::array<float, 3>& a = linears[ia];
        const std::array<float, 3>& b = linears[ib];
        const float spread = metricDistance(labs[ia], labs[ib]);
        const int centre = (int)std::lround(kept[k].r * stepCount);
        for (int step = centre - 1; step <= centre + 1; step++) {
            if (step < 1 || step > maxSteps) continue;
            const float r = step / stepCount;
            const float mixed[3] = {a[0] + r * (b[0] - a[0]), a[1] + r * (b[1] - a[1]), a[2] + r * (b[2] - a[2])};
            const float cost = metricDistance(lab, spaceOfLinear(mixed)) + 0.01f * r * (1.0f - r) * spread;
            if (cost < bestCost) {
                bestCost = cost;
                mix.a = ia;
                mix.b = ib;
                mix.ratio16 = (uint8_t)step;
            }
        }
    }
    if (mix.ratio16 == steps) {
        mix.a = mix.b;
        mix.ratio16 = 0;
    }
    return mix;
}

std::vector<Rgba8> pixelart::medianCut(std::span<const Rgba8> pixels, int count, std::span<const Rgba8> keep) {
    std::vector<Rgba8> result;
    for (const Rgba8& color : keep) appendUnique(result, Rgba8{color.r, color.g, color.b, 255});
    if (count <= 0) return {};
    if ((int)result.size() >= count) {
        result.resize(count);
        return result;
    }

    std::vector<ColorCount> colors = countColors(pixels);
    // the kept colours stand for their own pixels
    colors.erase(std::remove_if(colors.begin(), colors.end(), [&](const ColorCount& color) {
        for (const Rgba8& kept : result) {
            if (packRgb(kept) == color.rgb) return true;
        }
        return false;
    }), colors.end());
    const int boxesWanted = count - (int)result.size();
    if ((int)colors.size() <= boxesWanted) {
        // few enough colours: every one exactly, the most used first
        std::stable_sort(colors.begin(), colors.end(), [](const ColorCount& x, const ColorCount& y) { return x.count > y.count; });
        for (const ColorCount& color : colors) result.push_back(unpackRgb(color.rgb));
        return result;
    }

    std::vector<std::vector<const ColorCount*>> boxes(1);
    for (const ColorCount& color : colors) boxes[0].push_back(&color);
    while ((int)boxes.size() < boxesWanted) {
        // split the box with the widest OKLab axis (ties to the earlier box)
        int splitBox = -1, splitAxis = 0;
        float widest = -1.0f;
        for (size_t i = 0; i < boxes.size(); i++) {
            if (boxes[i].size() < 2) continue;
            for (int axis = 0; axis < 3; axis++) {
                float low = std::numeric_limits<float>::infinity(), high = -low;
                for (const ColorCount* member : boxes[i]) {
                    const float v = axisValue(member->lab, axis);
                    low = std::min(low, v);
                    high = std::max(high, v);
                }
                if (high - low > widest) {
                    widest = high - low;
                    splitBox = (int)i;
                    splitAxis = axis;
                }
            }
        }
        if (splitBox < 0) break;
        std::vector<const ColorCount*>& box = boxes[splitBox];
        std::sort(box.begin(), box.end(), [splitAxis](const ColorCount* x, const ColorCount* y) {
            const float vx = axisValue(x->lab, splitAxis), vy = axisValue(y->lab, splitAxis);
            return vx != vy ? vx < vy : x->rgb < y->rgb;
        });
        uint64_t total = 0;
        for (const ColorCount* member : box) total += member->count;
        // the weighted median, keeping both halves non-empty
        uint64_t running = 0;
        size_t cut = 1;
        for (size_t i = 0; i + 1 < box.size(); i++) {
            running += box[i]->count;
            cut = i + 1;
            if (running * 2 >= total) break;
        }
        std::vector<const ColorCount*> upper(box.begin() + (std::ptrdiff_t)cut, box.end());
        box.resize(cut);
        boxes.push_back(std::move(upper));
    }
    for (const std::vector<const ColorCount*>& box : boxes) appendUnique(result, fromOklab(weightedMean(box)));
    return result;
}

std::vector<Rgba8> pixelart::kMeansRefine(std::span<const Rgba8> pixels, std::vector<Rgba8> centres, int fixed, int iterations) {
    fixed = std::clamp(fixed, 0, (int)centres.size());
    if (centres.empty()) return centres;
    const std::vector<ColorCount> colors = countColors(pixels);
    std::vector<Lab> labs;
    labs.reserve(centres.size());
    for (const Rgba8& centre : centres) labs.push_back(toOklab(centre));
    std::vector<int> assignment(colors.size(), -1);
    for (int pass = 0; pass < iterations; pass++) {
        bool changed = false;
        for (size_t i = 0; i < colors.size(); i++) {
            int best = 0;
            float bestDistance = squaredDistance(colors[i].lab, labs[0]);
            for (size_t c = 1; c < labs.size(); c++) {
                const float d = squaredDistance(colors[i].lab, labs[c]);
                if (d < bestDistance) {
                    bestDistance = d;
                    best = (int)c;
                }
            }
            if (assignment[i] != best) {
                assignment[i] = best;
                changed = true;
            }
        }
        if (!changed) break;
        std::vector<std::vector<const ColorCount*>> members(labs.size());
        for (size_t i = 0; i < colors.size(); i++) members[assignment[i]].push_back(&colors[i]);
        for (size_t c = (size_t)fixed; c < labs.size(); c++) {
            if (!members[c].empty()) labs[c] = weightedMean(members[c]);
        }
    }
    std::vector<Rgba8> result;
    for (size_t c = 0; c < labs.size(); c++) {
        appendUnique(result, (int)c < fixed ? Rgba8{centres[c].r, centres[c].g, centres[c].b, 255} : fromOklab(labs[c]));
    }
    return result;
}

float pixelart::bayerThreshold(int size, int x, int y) {
    // M(2n) = [4M + 0, 4M + 2; 4M + 3, 4M + 1], built bit by bit from the coordinates
    const int n = size >= 8 ? 8 : (size >= 4 ? 4 : 2);
    const int bits = n == 8 ? 3 : (n == 4 ? 2 : 1);
    const int px = ((x % n) + n) % n;
    const int py = ((y % n) + n) % n;
    int m = 0;
    for (int bit = 0; bit < bits; bit++) {
        const int bx = (px >> bit) & 1;
        const int by = (py >> bit) & 1;
        // the 2x2 base [0, 2; 3, 1] at this level, most significant at the top level
        const int cell = by == 0 ? (bx == 0 ? 0 : 2) : (bx == 0 ? 3 : 1);
        m += cell << (2 * (bits - 1 - bit));
    }
    return ((float)m + 0.5f) / (float)(n * n);
}

std::vector<Rgba8> pixelart::fitRamp(std::span<const Rgba8> ramp, int steps) {
    if (ramp.empty() || steps <= 0) return {};
    if ((int)ramp.size() == steps) return std::vector<Rgba8>(ramp.begin(), ramp.end());
    std::vector<Rgba8> fitted;
    fitted.reserve(steps);
    const double span = (double)(ramp.size() - 1);
    const double divisor = (double)std::max(1, steps - 1);
    for (int i = 0; i < steps; i++) {
        // JavaScript's Math.round: halves go up
        const size_t index = (size_t)std::floor((double)i * span / divisor + 0.5);
        fitted.push_back(ramp[std::min(index, ramp.size() - 1)]);
    }
    return fitted;
}

Rgba8 pixelart::rampAt(std::span<const Rgba8> ramp, float t) {
    if (ramp.empty()) return Rgba8{0, 0, 0, 0};
    const double position = std::floor((double)(std::isfinite(t) ? t : 0.0f) * (double)ramp.size());
    const double last = (double)(ramp.size() - 1);
    return ramp[(size_t)std::clamp(position, 0.0, last)];
}
