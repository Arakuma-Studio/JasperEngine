// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/effects/lib.ts (blur, sdf, bounds), src/effects/a/blur.ts (gauss),
// src/effects/a/util.ts (dirVec) and src/model/gradient.ts (sampleGradientRgba, bakeGradient,
// the OKLCH blend) by SafEight, used with the author's permission.

#include "TitleFx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fx;

// ---- Scratch ----

Scratch::Scratch(Scratch&& other) noexcept : owner(other.owner), ref(other.ref) {
    other.owner = nullptr;
    other.ref = nullptr;
}

Scratch& Scratch::operator=(Scratch&& other) noexcept {
    if (this != &other) {
        if (owner && ref) owner->release(ref);
        owner = other.owner;
        ref = other.ref;
        other.owner = nullptr;
        other.ref = nullptr;
    }
    return *this;
}

Scratch::~Scratch() {
    if (owner && ref) owner->release(ref);
}

// ---- conversions ----

TitleVec2 fx::dirClockwise(double degrees) {
    const double r = degrees * 3.14159265358979323846 / 180.0;
    return {std::cos(r), std::sin(r)};
}

TitleColor fx::linearOf(const TitleColor& srgb) {
    return {srgbToLinear(srgb.r), srgbToLinear(srgb.g), srgbToLinear(srgb.b)};
}

namespace {

    double clamp01(double x) {
        return x < 0.0 ? 0.0 : x > 1.0 ? 1.0 : x;
    }

    // Photoshop's colour midpoint (a CSS colour hint): the half-way colour sits at `mid` of the gap
    double midpointBlend(double x, double mid) {
        const double m = std::min(0.95, std::max(0.05, mid));
        if (std::fabs(m - 0.5) < 1e-4 || x <= 0.0 || x >= 1.0) return x;
        return std::pow(x, std::log(0.5) / std::log(m));
    }

    using Rgb = std::array<double, 3>;

    double toLin(double c) {
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    }

    double toSrgb(double c) {
        return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    }

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

    // OKLab -> linear sRGB (unclamped)
    Rgb labToLinear(double L, double A, double B) {
        const double l = std::pow(L + 0.3963377774 * A + 0.2158037573 * B, 3.0);
        const double m = std::pow(L - 0.1055613458 * A - 0.0638541728 * B, 3.0);
        const double s = std::pow(L - 0.0894841775 * A - 1.291485548 * B, 3.0);
        return {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s, -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                -0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s};
    }

    // OKLCH -> sRGB, the chroma reduced (hue and lightness kept) until it fits the gamut
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

    // below this chroma a colour's hue is meaningless (whites, greys): it takes the other stop's
    const double ACHROMATIC = 0.004;

    // the OKLCH blend along the shorter hue arc
    Rgb mixVivid(const Rgb& a, const Rgb& b, double t) {
        const Rgb A = oklch(a), B = oklch(b);
        const double ha = A[1] < ACHROMATIC ? B[2] : A[2];
        const double hb = B[1] < ACHROMATIC ? A[2] : B[2];
        double dh = hb - ha;
        if (dh > 3.14159265358979323846) dh -= 2.0 * 3.14159265358979323846;
        else if (dh < -3.14159265358979323846) dh += 2.0 * 3.14159265358979323846;
        return fromOklch(A[0] + (B[0] - A[0]) * t, A[1] + (B[1] - A[1]) * t, ha + dh * t);
    }

    Rgb rgbOf(const TitleColor& c) {
        return {c.r, c.g, c.b};
    }

    bool sameColor(const TitleColor& a, const TitleColor& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b;
    }

}

void fx::sampleGradient(const TitleGradient& g, double pos, float out[4]) {
    const auto& s = g.stops;
    auto at = [&](const TitleGradientStop& st) {
        out[0] = st.color.r;
        out[1] = st.color.g;
        out[2] = st.color.b;
        out[3] = (float)st.alpha;
    };
    if (s.empty()) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        return;
    }
    if (pos <= s.front().pos) return at(s.front());
    for (size_t i = 0; i + 1 < s.size(); i++) {
        const TitleGradientStop& a = s[i];
        const TitleGradientStop& b = s[i + 1];
        if (pos > b.pos) continue;
        const double span = b.pos - a.pos;
        const double x = midpointBlend(span > 0.0 ? (pos - a.pos) / span : 0.0, a.mid);
        Rgb c;
        if (g.vivid && !sameColor(a.color, b.color)) {
            c = mixVivid(rgbOf(a.color), rgbOf(b.color), x);
        } else {
            const Rgb ca = rgbOf(a.color), cb = rgbOf(b.color);
            for (int k = 0; k < 3; k++) c[k] = ca[k] + (cb[k] - ca[k]) * x;
        }
        out[0] = (float)c[0];
        out[1] = (float)c[1];
        out[2] = (float)c[2];
        out[3] = (float)(a.alpha + (b.alpha - a.alpha) * x);
        return;
    }
    at(s.back());
}

TitleLutData fx::gradientLut(const TitleGradient& gradient) {
    TitleLutData data;
    data.format = ColorFormat::RGBA;
    data.bytes.resize(TITLE_LUT_SIZE * 4);
    for (int i = 0; i < TITLE_LUT_SIZE; i++) {
        float c[4];
        sampleGradient(gradient, (double)i / (TITLE_LUT_SIZE - 1), c);
        for (int k = 0; k < 4; k++) data.bytes[(size_t)i * 4 + k] = (uint8_t)std::lround(clamp01(c[k]) * 255.0);
    }
    return data;
}

// ---- passes and targets ----

namespace {

    void setFrame(TitlePass& pass, const TitleRectI& frame, int width, int height) {
        if (frame.empty() || width <= 0 || height <= 0) return;
        pass.frameUv[0] = (float)frame.x / (float)width;
        pass.frameUv[1] = (float)frame.y / (float)height;
        pass.frameUv[2] = (float)(frame.x + frame.width) / (float)width;
        pass.frameUv[3] = (float)(frame.y + frame.height) / (float)height;
    }

}

TitlePass fx::makePass(const EffectContext& ctx, TitleProgram program, const char* label, TargetRef& output) {
    TitlePass pass;
    pass.program = program;
    pass.variant = titleProgramVariant(program, ctx.runner.caps());
    pass.label = label;
    pass.output = &output;
    pass.pxScale = ctx.pxScale;
    setFrame(pass, ctx.frame, ctx.width, ctx.height);
    return pass;
}

TitlePass fx::makePass(const GeneratorContext& ctx, TitleProgram program, const char* label, TargetRef& output) {
    TitlePass pass;
    pass.program = program;
    pass.variant = titleProgramVariant(program, ctx.runner.caps());
    pass.label = label;
    pass.output = &output;
    pass.pxScale = ctx.pxScale;
    setFrame(pass, ctx.frame, ctx.width, ctx.height);
    return pass;
}

Scratch fx::scratchOf(TitlePassRunner& runner, TitleBuffer buffer, int width, int height) {
    const TitleBufferFormat& f = runner.caps().format(buffer);
    TargetKey key;
    key.width = std::max(1, width);
    key.height = std::max(1, height);
    key.format = f.format;
    key.filter = f.filter;
    return Scratch(runner, runner.acquire(key));
}

Scratch fx::scratch(EffectContext& ctx, int width, int height) {
    return scratchOf(ctx.runner, TitleBuffer::Accumulator, width > 0 ? width : ctx.width, height > 0 ? height : ctx.height);
}

bool fx::copy(EffectContext& ctx, const TargetRef& src, TargetRef& dst, float opacity) {
    TitlePass pass = makePass(ctx, TitleProgram::Copy, "copy", dst);
    pass.textures[0].target = &src;
    pass.set(2, opacity);
    return ctx.runner.run(pass);
}

namespace {

    // one 1-D Gaussian pass of src into dst: `along` 0 = x, 1 = y, sigma in dst texels
    bool gaussPass(EffectContext& ctx, const TargetRef& src, TargetRef& dst, int along, float sigma, bool repeatEdges, int maxTap, const char* label) {
        TitlePass pass = makePass(ctx, TitleProgram::Gauss, label, dst);
        pass.textures[0].target = &src;
        const TargetKey& k = dst.key();
        pass.set(2, along == 0 ? 1.0f / (float)k.width : 0.0f, along == 1 ? 1.0f / (float)k.height : 0.0f, sigma, repeatEdges ? 1.0f : 0.0f);
        pass.set(3, (float)maxTap);
        return ctx.runner.run(pass);
    }

    int halvings(float sigma, float cap, int maxLevels) {
        int l = 0;
        while (sigma / std::ldexp(1.0f, l) > cap && l < maxLevels) l++;
        return l;
    }

    int ceilDiv(int v, int d) {
        return std::max(1, (v + d - 1) / d);
    }

}

Scratch fx::blur(EffectContext& ctx, const TargetRef& src, float radiusPx) {
    Scratch out = scratch(ctx);
    if (!out) return {};
    const float sigma = std::max(0.0f, radiusPx / 3.0f);
    if (sigma < 0.3f) {
        if (!copy(ctx, src, *out)) return {};
        return out;
    }
    // a downsample level keeps the kernel within ~ +-18 taps
    const int level = halvings(sigma, 6.0f, 5);
    const int f = 1 << level;
    const int w = ceilDiv(ctx.width, f), h = ceilDiv(ctx.height, f);
    // progressive bilinear halvings, so the downsample does not alias
    Scratch levels[6];
    const TargetRef* cur = &src;
    for (int l = 1; l <= level; l++) {
        levels[l] = scratch(ctx, ceilDiv(ctx.width, 1 << l), ceilDiv(ctx.height, 1 << l));
        if (!levels[l] || !copy(ctx, *cur, *levels[l])) return {};
        cur = levels[l].get();
    }
    const float s = sigma / (float)f;
    Scratch a = scratch(ctx, w, h), b = scratch(ctx, w, h);
    if (!a || !b) return {};
    if (!gaussPass(ctx, *cur, *a, 0, s, true, 24, "blur x") || !gaussPass(ctx, *a, *b, 1, s, true, 24, "blur y")) return {};
    if (!copy(ctx, *b, *out)) return {};
    return out;
}

bool fx::gauss(EffectContext& ctx, const TargetRef& src, float sigmaX, float sigmaY, bool repeatEdges, TargetRef& output) {
    const int lx = halvings(sigmaX, 6.0f, 10), ly = halvings(sigmaY, 6.0f, 10);
    Scratch held[3];
    const TargetRef* cur = &src;
    int w = ctx.width, h = ctx.height;
    for (int i = 1; i <= std::max(lx, ly); i++) {
        if (i <= lx) w = ceilDiv(w, 2);
        if (i <= ly) h = ceilDiv(h, 2);
        Scratch next = scratch(ctx, w, h);
        if (!next || !copy(ctx, *cur, *next)) return false;
        held[i % 2] = std::move(next);
        cur = held[i % 2].get();
    }
    const float sx = sigmaX * (float)w / (float)ctx.width;
    const float sy = sigmaY * (float)h / (float)ctx.height;
    if (sx >= 0.3f) {
        Scratch t = scratch(ctx, w, h);
        if (!t || !gaussPass(ctx, *cur, *t, 0, sx, repeatEdges, 32, "gaussian blur x")) return false;
        held[2] = std::move(t);
        cur = held[2].get();
    }
    if (sy >= 0.3f) {
        Scratch t = scratch(ctx, w, h);
        if (!t || !gaussPass(ctx, *cur, *t, 1, sy, repeatEdges, 32, "gaussian blur y")) return false;
        held[2] = std::move(t);
        cur = held[2].get();
    }
    return copy(ctx, *cur, output);
}

int fx::floodPasses(float reachPx, int width, int height) {
    const int side = std::max(1, std::max(width, height));
    const int unbounded = (int)std::ceil(std::log2((double)side));
    int k = unbounded;
    if (reachPx > 0.0f) k = std::min(unbounded, (int)std::ceil(std::log2((double)reachPx + 2.0)));
    return std::max(1, k) + 1;
}

Scratch fx::distanceField(EffectContext& ctx, const TargetRef& src, float threshold, float reachPx) {
    Scratch ping = scratchOf(ctx.runner, TitleBuffer::Seeds, ctx.width, ctx.height);
    Scratch pong = scratchOf(ctx.runner, TitleBuffer::Seeds, ctx.width, ctx.height);
    Scratch out = scratchOf(ctx.runner, TitleBuffer::Distance, ctx.width, ctx.height);
    if (!ping || !pong || !out) return {};
    TitlePass seed = makePass(ctx, TitleProgram::Seed, "distance seeds", *ping);
    seed.textures[0].target = &src;
    seed.set(2, threshold);
    if (!ctx.runner.run(seed)) return {};
    // steps 2^(k-1) .. 1, then one more step of 1 (JFA+1 cleans up the residual errors)
    const int passes = floodPasses(reachPx, ctx.width, ctx.height);
    TargetRef* a = ping.get();
    TargetRef* b = pong.get();
    for (int i = 0; i < passes; i++) {
        const int stepPx = i < passes - 1 ? 1 << (passes - 2 - i) : 1;
        TitlePass flood = makePass(ctx, TitleProgram::Jfa, "distance flood", *b);
        flood.raw[0].target = a;
        flood.set(2, (float)stepPx);
        if (!ctx.runner.run(flood)) return {};
        std::swap(a, b);
    }
    TitlePass resolve = makePass(ctx, TitleProgram::Distance, "distance resolve", *out);
    resolve.raw[0].target = a;
    resolve.textures[0].target = &src;
    resolve.set(2, threshold);
    if (!ctx.runner.run(resolve)) return {};
    return out;
}

Scratch fx::layerBounds(EffectContext& ctx, const TargetRef& src) {
    int w = ctx.width, h = ctx.height;
    Scratch cur;
    bool first = true;
    do {
        w = ceilDiv(w, 4);
        h = ceilDiv(h, 4);
        Scratch next = scratchOf(ctx.runner, TitleBuffer::Seeds, w * 2, h);
        if (!next) return {};
        TitlePass pass = makePass(ctx, TitleProgram::Bounds, "layer box", *next);
        if (first) pass.textures[0].target = &src;
        else pass.raw[0].target = cur.get();
        pass.set(2, first ? 1.0f : 0.0f);
        if (!ctx.runner.run(pass)) return {};
        cur = std::move(next);
        first = false;
    } while (w > 1 || h > 1);
    return cur;
}

int fx::patternKindId(std::string_view kind) {
    static const char* const KINDS[] = {"none", "zebra", "tiger", "giraffe", "leopard", "checker", "hazard", "stripes",
                                        "dots", "sakura", "seigaiha", "asanoha", "shippo", "kikko", "uroko", "custom",
                                        // S83 §6.6.28
                                        "wood", "marble", "granite", "concrete", "brushed_metal", "diamond_plate", "carbon_fiber",
                                        "rust", "leather", "brick", "camo", "circuit"};
    for (int i = 0; i < (int)(sizeof(KINDS) / sizeof(KINDS[0])); i++) {
        if (kind == KINDS[i]) return i;
    }
    return 0;
}
