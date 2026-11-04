// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/model/gradient.ts (sampleGradientRgba, midpointBlend, the OKLCH
// `vivid` blend, bakeGradient) by SafEight, used with the author's permission.

#include "TitleRenderServices.h"

#include "titleart/TitleArtHash.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    using Rgb = std::array<double, 3>;

    double clamp01(double x) { return x < 0.0 ? 0.0 : x > 1.0 ? 1.0 : x; }

    // Photoshop's colour midpoint: the half-way colour sits at `mid` of the gap
    double midpointBlend(double x, double mid) {
        const double m = std::clamp(mid, 0.05, 0.95);
        if (std::abs(m - 0.5) < 1e-4 || x <= 0.0 || x >= 1.0) return x;
        return std::pow(x, std::log(0.5) / std::log(m));
    }

    double toLin(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
    double toSrgb(double c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; }

    // sRGB 0..1 -> OKLCH (L, C, h radians)
    Rgb oklch(const Rgb& c) {
        const double R = toLin(c[0]), G = toLin(c[1]), B = toLin(c[2]);
        const double l = std::cbrt(0.4122214708 * R + 0.5363325363 * G + 0.0514459929 * B);
        const double m = std::cbrt(0.2119034982 * R + 0.6806995451 * G + 0.1073969566 * B);
        const double s = std::cbrt(0.0883024619 * R + 0.2817188376 * G + 0.6299787005 * B);
        const double L = 0.2104542553 * l + 0.793617785 * m - 0.0040720468 * s;
        const double A = 1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s;
        const double Bb = 0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s;
        return {L, std::hypot(A, Bb), std::atan2(Bb, A)};
    }

    Rgb labToLinear(double L, double A, double B) {
        const double l = std::pow(L + 0.3963377774 * A + 0.2158037573 * B, 3.0);
        const double m = std::pow(L - 0.1055613458 * A - 0.0638541728 * B, 3.0);
        const double s = std::pow(L - 0.0894841775 * A - 1.291485548 * B, 3.0);
        return {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s, -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                -0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s};
    }

    // OKLCH -> sRGB, chroma reduced (hue and lightness kept) until it fits the gamut
    Rgb fromOklch(double L, double C, double h) {
        auto fits = [](const Rgb& c) { return std::all_of(c.begin(), c.end(), [](double v) { return v >= -1e-4 && v <= 1.0 + 1e-4; }); };
        Rgb lin = labToLinear(L, C * std::cos(h), C * std::sin(h));
        if (!fits(lin)) {
            double lo = 0.0, hi = C;
            for (int i = 0; i < 14; i++) {
                const double mid = (lo + hi) / 2.0;
                if (fits(labToLinear(L, mid * std::cos(h), mid * std::sin(h)))) lo = mid;
                else hi = mid;
            }
            lin = labToLinear(L, lo * std::cos(h), lo * std::sin(h));
        }
        return {clamp01(toSrgb(clamp01(lin[0]))), clamp01(toSrgb(clamp01(lin[1]))), clamp01(toSrgb(clamp01(lin[2])))};
    }

    // below this chroma a colour's hue is meaningless: it takes the other stop's
    constexpr double ACHROMATIC = 0.004;
    constexpr double PI = 3.14159265358979323846;

    Rgb mixVivid(const Rgb& a, const Rgb& b, double t) {
        const Rgb A = oklch(a), B = oklch(b);
        const double ha = A[1] < ACHROMATIC ? B[2] : A[2];
        const double hb = B[1] < ACHROMATIC ? A[2] : B[2];
        double dh = hb - ha;
        if (dh > PI) dh -= 2.0 * PI;
        else if (dh < -PI) dh += 2.0 * PI;
        return fromOklch(A[0] + (B[0] - A[0]) * t, A[1] + (B[1] - A[1]) * t, ha + dh * t);
    }

    Rgb rgbOf(const TitleColor& c) { return {c.r, c.g, c.b}; }

    std::array<double, 4> sample(const TitleGradient& g, const std::vector<TitleGradientStop>& s, double pos) {
        if (s.empty()) return {1.0, 1.0, 1.0, 1.0};
        auto at = [](const TitleGradientStop& st) { return std::array<double, 4>{st.color.r, st.color.g, st.color.b, st.alpha}; };
        if (pos <= s.front().pos) return at(s.front());
        for (size_t i = 0; i + 1 < s.size(); i++) {
            const TitleGradientStop& a = s[i];
            const TitleGradientStop& b = s[i + 1];
            if (pos > b.pos) continue;
            const double span = b.pos - a.pos;
            const double x = midpointBlend(span > 0.0 ? (pos - a.pos) / span : 0.0, a.mid);
            const Rgb ca = rgbOf(a.color), cb = rgbOf(b.color);
            const bool sameColor = a.color.r == b.color.r && a.color.g == b.color.g && a.color.b == b.color.b;
            Rgb c;
            if (g.vivid && !sameColor) c = mixVivid(ca, cb, x);
            else c = {ca[0] + (cb[0] - ca[0]) * x, ca[1] + (cb[1] - ca[1]) * x, ca[2] + (cb[2] - ca[2]) * x};
            return {c[0], c[1], c[2], a.alpha + (b.alpha - a.alpha) * x};
        }
        return at(s.back());
    }

}

void titleart::bakeGradient(const TitleGradient& gradient, uint8_t out[256 * 4]) {
    std::vector<TitleGradientStop> stops = gradient.stops;
    std::stable_sort(stops.begin(), stops.end(), [](const TitleGradientStop& a, const TitleGradientStop& b) { return a.pos < b.pos; });
    for (int i = 0; i < 256; i++) {
        const std::array<double, 4> c = sample(gradient, stops, i / 255.0);
        for (int k = 0; k < 4; k++) out[i * 4 + k] = (uint8_t)std::lround(clamp01(c[k]) * 255.0);
    }
}

void titleart::bakeCurve16(const TitleCurve& curve, uint8_t out[256 * 4]) {
    const std::vector<float> samples = curve.sample(256);
    for (int i = 0; i < 256; i++) {
        const double v = clamp01(i < (int)samples.size() ? samples[i] : i / 255.0);
        const uint32_t code = (uint32_t)std::lround(v * 65535.0);
        out[i * 4 + 0] = (uint8_t)(code >> 8);
        out[i * 4 + 1] = (uint8_t)(code & 0xFF);
        out[i * 4 + 2] = 0;
        out[i * 4 + 3] = 255;
    }
}

uint64_t titleart::gradientKey(const TitleGradient& gradient) {
    uint64_t h = fnv1a64(gradient.vivid ? "vivid" : "rgb");
    for (const TitleGradientStop& s : gradient.stops) {
        const double values[7] = {s.pos, s.color.r, s.color.g, s.color.b, s.alpha, s.mid, 0.0};
        h = hashCombine(h, fnv1a64(std::string_view(reinterpret_cast<const char*>(values), sizeof(values))));
    }
    return h;
}

uint64_t titleart::curveKey(const TitleCurve& curve) {
    uint64_t h = fnv1a64("curve");
    for (const TitleCurvePoint& p : curve.points) {
        const double values[3] = {p.x, p.y, p.corner ? 1.0 : 0.0};
        h = hashCombine(h, fnv1a64(std::string_view(reinterpret_cast<const char*>(values), sizeof(values))));
    }
    return h;
}
