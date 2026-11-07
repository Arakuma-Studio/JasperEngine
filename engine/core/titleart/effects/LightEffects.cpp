// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Light Glow and Contour Lines (docs/specs/S83-title-effects-generators.md §6.6.1, §6.6.2), ported
// from Pachislop src/effects/a/lightGlow.ts and src/effects/a/glow.ts (deepGlow, contourLines) by
// SafEight, used with the author's permission; Neon Tube (§6.6.24) is S83's design on the same
// light kernel.

#include "LightEffects.h"
#include "TfxCommon.h"
#include "TfxPrograms.h"
#include "TitleFx.h"
#include "TfxFamilies.h"
#include "TitleFxParams.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fxparams;

namespace {
    const char* PRESETS_LIGHT_GLOW = "editor/resources/titleart/fx-presets/light_glow.yaml";
    const char* PRESETS_CONTOUR = "editor/resources/titleart/fx-presets/contour_lines.yaml";
    const char* PRESETS_NEON = "editor/resources/titleart/fx-presets/neon_tube.yaml";
}

// ---- the power-law point-spread function (lightGlow.ts psfOctaves) ----

double tfx::falloffExponent(double falloffPct) {
    return 1.6 + 1.6 * std::min(1.0, std::max(0.0, falloffPct / 100.0));
}

std::vector<tfx::GlowOctave> tfx::psfOctaves(double radius, double p, double chroma, int maxOctaves, double pxScale) {
    const double sigmaMin = 1.0;
    const double sMax = std::max(sigmaMin, radius / 3.0);
    const double span = std::log2(sMax / sigmaMin);
    int n = std::max(1, (int)std::ceil(span - 1e-6) + 1);
    if (maxOctaves > 0) n = std::min(n, maxOctaves);
    const double d = n > 1 ? span / (n - 1) : 0.0;
    const double k = 0.2 * std::max(-1.0, std::min(1.0, chroma));
    const double scale[3] = {1.0 + k, 1.0, 1.0 - k};
    std::vector<GlowOctave> out;
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        const double s = sigmaMin * std::pow(2.0, i * d);
        GlowOctave o;
        o.weight = std::pow(s, 2.0 - p);
        sum += o.weight;
        for (int c = 0; c < 3; c++) o.sigma[c] = s * pxScale * scale[c];
        o.t = n > 1 ? (double)i / (n - 1) : 0.0;
        out.push_back(o);
    }
    for (GlowOctave& o : out) o.weight /= sum;
    return out;
}

double tfx::glowLevelVariance(int level, bool cascade) {
    if (level == 0) return 0.0;
    // l 2 x 2 box reductions add sum_j 1/4 (2^(j-1))^2 = (4^l - 1) / 12; the bilinear upsample from
    // level l a tent of 4^l / 6, or on the cascade one tent per level it climbs (D11)
    double v = (std::pow(4.0, level) - 1.0) / 12.0;
    if (!cascade) return v + std::pow(4.0, level) / 6.0;
    for (int j = 1; j <= level; j++) v += std::pow(4.0, j) / 6.0;
    return v;
}

int tfx::glowLevel(const GlowOctave& o, int width, int height, bool cascade) {
    const double sMin = std::min(o.sigma[0], std::min(o.sigma[1], o.sigma[2]));
    int l = 0;
    while (sMin / std::ldexp(1.0, l + 1) >= 2.0 && std::ceil(std::min(width, height) / std::ldexp(1.0, l + 1)) >= 4.0) l++;
    // the mobile cascade stops at half resolution
    return cascade ? std::max(l, 1) : l;
}

void tfx::lightResponse(const float in[3], float shift, float white, float out[3]) {
    auto bleed = [white](const float e[3], float b[3]) {
        b[0] = 0.5f * e[1] + 0.5f * white * e[2];
        b[1] = 0.5f * (e[0] + e[2]);
        b[2] = 0.5f * e[1] + 0.5f * white * e[0];
    };
    float e0[3], b0[3], x1[3], e1[3], b1[3];
    for (int c = 0; c < 3; c++) e0[c] = std::max(in[c] - 1.0f, 0.0f);
    bleed(e0, b0);
    for (int c = 0; c < 3; c++) x1[c] = in[c] + shift * b0[c];
    for (int c = 0; c < 3; c++) e1[c] = std::max(x1[c] - 1.0f, 0.0f);
    bleed(e1, b1);
    const float t = std::min(1.0f, std::max(0.0f, shift / 0.25f));
    const float k = t * t * (3.0f - 2.0f * t);
    for (int c = 0; c < 3; c++) {
        const float x2 = in[c] + shift * b1[c];
        const float y = std::min(x2, 1.0f) + 0.5f * (1.0f - std::exp(-2.0f * std::max(x2 - 1.0f, 0.0f)));
        out[c] = in[c] + (y - in[c]) * k;
    }
}

namespace {

    int ceilDiv(int v, int d) {
        return std::max(1, (v + d - 1) / d);
    }

    // the blur left to do at an octave's level so its total variance is sigma^2 per channel, in
    // level texels, and its taps
    void levelSigma(const tfx::GlowOctave& o, int level, bool cascade, float out[3], int& taps) {
        const double f = std::ldexp(1.0, level);
        const double v = tfx::glowLevelVariance(level, cascade);
        double m = 0.0;
        for (int c = 0; c < 3; c++) {
            out[c] = (float)(std::sqrt(std::max(0.0, o.sigma[c] * o.sigma[c] - v)) / f);
            m = std::max(m, (double)out[c]);
        }
        taps = std::min(48, (int)std::ceil(3.0 * m));
    }

}

fx::Scratch tfx::scatterLight(EffectContext& ctx, const TargetRef& input, const ScatterOptions& o) {
    const float H = lightHeadroom(ctx.runner.caps());
    const bool cascade = ctx.tier == FxTier::Mobile;
    fx::Scratch emit = fx::scratch(ctx);
    if (!emit) return {};
    TitlePass e = makePass(ctx, Program::LightEmit, "light_glow emit", *emit);
    e.textures[0].target = &input;
    e.set(2, o.gain, o.threshold, o.knee, o.custom ? 1.0f : 0.0f);
    e.set(3, o.color.r, o.color.g, o.color.b, 1.0f / H);
    if (!ctx.runner.run(e)) return {};

    std::vector<int> levels;
    int top = 0;
    for (const GlowOctave& oc : o.octaves) {
        levels.push_back(glowLevel(oc, ctx.width, ctx.height, cascade));
        top = std::max(top, levels.back());
    }
    // the pyramid: level l is ceil(size / 2^l)
    std::vector<fx::Scratch> pyramid;
    pyramid.push_back(std::move(emit));
    int w = ctx.width, h = ctx.height;
    for (int l = 1; l <= top; l++) {
        const int sw = w, sh = h;
        w = ceilDiv(w, 2);
        h = ceilDiv(h, 2);
        fx::Scratch t = fx::scratch(ctx, w, h);
        if (!t) return {};
        TitlePass down = makePass(ctx, Program::LightDown, "light_glow down", *t);
        down.textures[0].target = pyramid.back().get();
        down.set(2, 1.0f / (float)sw, 1.0f / (float)sh);
        if (!ctx.runner.run(down)) return {};
        pyramid.push_back(std::move(t));
    }
    auto levelSize = [&](int l, int& lw, int& lh) {
        lw = ctx.width;
        lh = ctx.height;
        for (int i = 0; i < l; i++) {
            lw = ceilDiv(lw, 2);
            lh = ceilDiv(lh, 2);
        }
    };
    // one octave blurred at its level (two passes); the result is held in `out`
    auto blurOctave = [&](size_t i, fx::Scratch& out) {
        const int l = levels[i];
        int lw, lh;
        levelSize(l, lw, lh);
        float sigma[3];
        int taps;
        levelSigma(o.octaves[i], l, cascade, sigma, taps);
        fx::Scratch tmp = fx::scratch(ctx, lw, lh);
        out = fx::scratch(ctx, lw, lh);
        if (!tmp || !out) return false;
        TitlePass bx = makePass(ctx, Program::LightBlur, "light_glow blur x", *tmp);
        bx.textures[0].target = pyramid[(size_t)l].get();
        bx.set(2, 1.0f / (float)lw, 0.0f, (float)taps);
        bx.set(3, sigma[0], sigma[1], sigma[2]);
        if (!ctx.runner.run(bx)) return false;
        TitlePass by = makePass(ctx, Program::LightBlur, "light_glow blur y", *out);
        by.textures[0].target = tmp.get();
        by.set(2, 0.0f, 1.0f / (float)lh, (float)taps);
        by.set(3, sigma[0], sigma[1], sigma[2]);
        return ctx.runner.run(by);
    };
    auto tintOf = [&](size_t i) {
        const double t = o.octaves[i].t;
        return TitleColor{(float)(o.tintInner.r + (o.tintOuter.r - o.tintInner.r) * t), (float)(o.tintInner.g + (o.tintOuter.g - o.tintInner.g) * t),
                          (float)(o.tintInner.b + (o.tintOuter.b - o.tintInner.b) * t)};
    };
    // acc' = acc + weight x octave (prev: the accumulation so far, null for the first; coarser:
    // prev is a smaller level, sampled bilinearly)
    auto accumulate = [&](const TargetRef& octave, const TargetRef* prev, bool coarser, TargetRef& out, float weight, const TitleColor& tint,
                          float tintAmount) {
        TitlePass a = makePass(ctx, Program::LightAccum, "light_glow accumulate", out);
        a.textures[0].target = &octave;
        a.textures[1].target = prev;
        a.set(2, tint.r, tint.g, tint.b, weight);
        a.set(3, tintAmount, prev ? 0.0f : 1.0f, coarser ? 1.0f : 0.0f);
        return ctx.runner.run(a);
    };

    if (!cascade) {
        // the editor and desktop path: every octave added at full resolution (Pachislop's)
        fx::Scratch acc[2] = {fx::scratch(ctx), fx::scratch(ctx)};
        if (!acc[0] || !acc[1]) return {};
        int cur = -1;
        for (size_t i = 0; i < o.octaves.size(); i++) {
            fx::Scratch oct;
            if (!blurOctave(i, oct)) return {};
            const int next = cur < 0 ? 0 : 1 - cur;
            if (!accumulate(*oct, cur < 0 ? nullptr : acc[cur].get(), false, *acc[next], (float)(o.octaves[i].weight * o.intensity), tintOf(i),
                            o.tintAmount)) {
                return {};
            }
            cur = next;
        }
        return std::move(acc[cur < 0 ? 0 : cur]);
    }
    // the mobile cascade (D11): from the coarsest level down to half resolution, each level's
    // accumulation starts from the coarser one upsampled, then adds the octaves blurred there
    fx::Scratch carried;
    for (int l = top; l >= 1; l--) {
        int lw, lh;
        levelSize(l, lw, lh);
        fx::Scratch acc[2] = {fx::scratch(ctx, lw, lh), fx::scratch(ctx, lw, lh)};
        if (!acc[0] || !acc[1]) return {};
        int cur = -1;
        const TargetRef* prev = carried ? carried.get() : nullptr;
        bool coarser = (bool)carried;
        for (size_t i = 0; i < o.octaves.size(); i++) {
            if (levels[i] != l) continue;
            fx::Scratch oct;
            if (!blurOctave(i, oct)) return {};
            const int next = cur < 0 ? 0 : 1 - cur;
            if (!accumulate(*oct, prev, coarser, *acc[next], (float)(o.octaves[i].weight * o.intensity), tintOf(i), o.tintAmount)) return {};
            cur = next;
            prev = acc[cur].get();
            coarser = false;
        }
        if (cur < 0 && carried) {
            // no octave at this level: the coarser accumulation is carried down unchanged
            if (!accumulate(*carried, nullptr, false, *acc[0], 1.0f, TitleColor{1.0f, 1.0f, 1.0f}, 0.0f)) return {};
            cur = 0;
        }
        if (cur >= 0) carried = std::move(acc[cur]);
    }
    return carried;
}

FxCost tfx::estimateScatter(const std::vector<GlowOctave>& octaves, const FxSize& size, FxTier tier) {
    const bool cascade = tier == FxTier::Mobile;
    CostBuilder b;
    b.pass(1.0, 1);
    int top = 0;
    std::vector<int> levels;
    for (const GlowOctave& o : octaves) {
        levels.push_back(glowLevel(o, std::max(1, size.width), std::max(1, size.height), cascade));
        top = std::max(top, levels.back());
    }
    for (int l = 1; l <= top; l++) b.pass(std::ldexp(1.0, -2 * l), 4);
    for (size_t i = 0; i < octaves.size(); i++) {
        float sigma[3];
        int taps;
        levelSigma(octaves[i], levels[i], cascade, sigma, taps);
        const double frac = std::ldexp(1.0, -2 * levels[i]);
        b.pass(frac, 2 * taps + 1, 2);
        b.pass(cascade ? frac : 1.0, 1);
    }
    return b.cost;
}

namespace {

    // ---- Light Glow ----

    tfx::ScatterOptions glowOptions(const EffectContext& ctx, const ParamValues& p) {
        tfx::ScatterOptions o;
        o.gain = (float)std::pow(2.0, p.number("exposure"));
        o.threshold = (float)p.number("threshold");
        o.knee = (float)p.number("knee");
        o.custom = p.string("color_mode") == "custom";
        o.color = fx::linearOf(p.color("color"));
        const bool mobile = ctx.tier == FxTier::Mobile;
        // the mobile tier: at most 6 octaves, no chromatic scatter (D11)
        o.octaves = tfx::psfOctaves(std::max(4.0, p.number("radius")), tfx::falloffExponent(p.number("falloff")),
                                    mobile ? 0.0 : p.number("chroma") / 100.0, mobile ? 6 : 0, ctx.pxScale);
        o.intensity = (float)p.number("intensity");
        o.tintInner = fx::linearOf(p.color("tint_inner"));
        o.tintOuter = fx::linearOf(p.color("tint_outer"));
        o.tintAmount = (float)p.number("tint_amount");
        return o;
    }

    void applyLightGlow(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const tfx::ScatterOptions o = glowOptions(ctx, p);
        fx::Scratch glow = tfx::scatterLight(ctx, input, o);
        if (!glow) return;
        TitlePass c = tfx::makePass(ctx, tfx::Program::LightCompose, "light_glow compose", output);
        c.textures[0].target = &input;
        c.textures[1].target = glow.get();
        c.set(2, o.gain, o.threshold, o.knee, (float)p.number("on_face"));
        c.set(3, (float)p.number("highlight_shift") / 100.0f, (float)p.number("core_white") / 100.0f, tfx::lightHeadroom(ctx.runner.caps()));
        ctx.runner.run(c);
    }

    FxCost estimateLightGlow(const ParamValues& p, const FxSize& size, FxTier tier) {
        const bool mobile = tier == FxTier::Mobile;
        const auto octaves = tfx::psfOctaves(std::max(4.0, p.number("radius")), tfx::falloffExponent(p.number("falloff")),
                                             mobile ? 0.0 : p.number("chroma") / 100.0, mobile ? 6 : 0, size.pxScale);
        tfx::CostBuilder b;
        b.cost = tfx::estimateScatter(octaves, size, tier);
        b.pass(1.0, 2);
        return b.cost;
    }

    EffectDef lightGlow() {
        using namespace tfx;
        EffectDef d;
        d.type = "light_glow";
        d.label = "Light Glow";
        d.category = "Glow";
        d.description = "A physically based glow: the layer becomes a light source whose light scatters with a power-law falloff (like lens bloom) "
                        "and burns toward white where it is over-bright; Radius and Falloff reshape the halo without changing how much light there is";
        d.params = {
            number("exposure", "Exposure", 1, -4, 6, ParamUnit::None, "Brightens the light, in stops (each stop doubles it)"),
            number("threshold", "Threshold", 0, 0, 2, ParamUnit::None, "Only light brighter than this luminance glows (0 = all of it)"),
            number("knee", "Threshold Knee", 0.25, 0, 1, ParamUnit::None, "How softly the threshold cuts in"),
            px("radius", "Radius", 200, 4, 1000, "How far the halo reaches"),
            number("falloff", "Falloff", 50, 0, 100, ParamUnit::None, "Long haze (0) to a tight halo (100), in percent; the light's total stays the same", 1),
            multiplies(live(number("intensity", "Intensity", 1, 0, 8, ParamUnit::Factor, "How much of the light scatters into the glow"))),
            number("chroma", "Chromatic Scatter", 0, -100, 100, ParamUnit::None, "Red spreads wider than blue (+) or the reverse (-), in percent", 1),
            choice("color_mode", "Glow Color", "layer", {{"layer", "From Layer"}, {"custom", "Custom"}}, "The glow takes the layer's colours or one colour"),
            when(color("color", "Color", "#ffffff", "The glow's colour", true), "color_mode", {"custom"}),
            number("tint_amount", "Tint Amount", 0, 0, 1, ParamUnit::None, "How much the halo is tinted from Tint Inner to Tint Outer"),
            color("tint_inner", "Tint Inner", "#ffffff", "The tint near the layer", true),
            color("tint_outer", "Tint Outer", "#ff8a2a", "The tint at the halo's edge", true),
            number("highlight_shift", "Highlight Shift", 60, 0, 100, ParamUnit::None,
                   "Over-bright colour bleeds into its neighbours (blue to cyan to white), in percent", 1),
            number("core_white", "Core Whiteness", 20, 0, 100, ParamUnit::None, "How quickly over-bright cores turn white, in percent", 1),
            number("on_face", "Glow Over Face", 0, 0, 1, ParamUnit::None, "How much of the glow falls over the layer itself"),
        };
        d.gizmos = {{GizmoKind::Radius, "radius", nullptr}};
        d.reach = [](const ParamValues& p) { return (float)p.number("radius"); };
        // the widest octave: sigma = radius / 3, x 1.2 at full chromatic scatter, taps to 3 sigma
        d.gather = [](const ParamValues& p) { return 1.25f * (float)p.number("radius"); };
        d.addsLight = [](const ParamValues&) { return true; };
        d.costClass = CostClass::C3;
        d.fallback = FallbackClass::F2;
        d.live = {LiveKind::ScaleSplit, {"intensity"}, (uint32_t)Program::LightCompose};
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"power glow", "bloom", "glare", "light", "deepGlow", "deep glow light", "発光", "光"};
        d.estimate = estimateLightGlow;
        d.presetFile = PRESETS_LIGHT_GLOW;
        d.apply = applyLightGlow;
        return d;
    }

    // ---- Contour Lines ----

    const TargetRef& tracedOf(const EffectContext& ctx, const TargetRef& input, const ParamValues& p) {
        if (p.string("follow") == "stack") return ctx.inputInk ? *ctx.inputInk : input;
        return ctx.ink ? *ctx.ink : input;
    }

    float contourReach(const ParamValues& p) {
        return (float)(p.number("offset") + (std::max(1, p.integer("count")) - 1) * p.number("spacing") + p.number("width") / 2.0);
    }

    void applyContourLines(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float s = ctx.pxScale;
        const TargetRef& traced = tracedOf(ctx, input, p);
        fx::Scratch field = fx::distanceField(ctx, traced, 0.5f, contourReach(p) * s + 2.0f);
        if (!field) return;
        const bool across = p.string("color_by") != "line";
        fx::Scratch box;
        if (across) {
            box = fx::layerBounds(ctx, traced);
            if (!box) return;
        }
        const TitleLut* ramp = ctx.runner.lut(fx::gradientLut(p.gradient("gradient")));
        fx::Scratch lines = fx::scratch(ctx);
        if (!ramp || !lines) return;
        TitlePass l = tfx::makePass(ctx, tfx::Program::ContourLines, "contour_lines lines", *lines);
        l.raw[0].target = field.get();
        l.raw[1].target = box.get();
        l.textures[3].lut = ramp;
        const TitleVec2 dir = fx::dirClockwise(p.number("angle"));
        l.set(2, std::max(1.0f, (float)p.number("spacing") * s), std::max(0.5f, (float)p.number("width") * s), (float)p.number("offset") * s,
              (float)p.number("falloff"));
        l.set(3, (float)std::max(1, p.integer("count")), (float)optionIndex(p, "mode", {"inside", "outside", "both"}), across ? 0.0f : 1.0f);
        l.set(4, (float)dir.x, (float)dir.y);
        if (!ctx.runner.run(l)) return;
        const float glowR = (float)p.number("glow_size") * s;
        fx::Scratch glow;
        if (glowR > 0.5f) {
            glow = fx::blur(ctx, *lines, glowR);
            if (!glow) return;
        }
        TitlePass c = tfx::makePass(ctx, tfx::Program::ContourCompose, "contour_lines compose", output);
        c.textures[0].target = &input;
        c.textures[1].target = lines.get();
        c.textures[2].target = glow ? glow.get() : lines.get();
        c.set(2, (float)p.number("intensity"), glow ? (float)p.number("glow") : 0.0f, (float)p.number("dim"));
        ctx.runner.run(c);
    }

    FxCost estimateContourLines(const ParamValues& p, const FxSize& size, FxTier) {
        tfx::CostBuilder b;
        b.field();
        if (p.string("color_by") != "line") b.box();
        b.pass(1.0, 3);
        if (p.number("glow_size") * size.pxScale > 0.5) b.blur(p.number("glow_size") * size.pxScale);
        b.pass(1.0, 3);
        return b.cost;
    }

    Json rainbowSpectrum() {
        return gradientValue({{0.0, "#ff2a2a"}, {0.17, "#ff9b00"}, {0.33, "#fff200"}, {0.5, "#2bff3a"}, {0.67, "#00c8ff"}, {0.83, "#3a3aff"}, {1.0, "#d22cff"}});
    }

    EffectDef contourLines() {
        using namespace tfx;
        EffectDef d;
        d.type = "contour_lines";
        d.label = "Contour Lines";
        d.category = "Glow";
        d.description = "Light-guide panel lines (疑似導光板): glowing lines that follow the silhouette at even spacing, inside or around it, "
                        "coloured by a gradient and added as light with a glow";
        d.params = {
            choice("mode", "Lines", "inside", {{"inside", "Inside"}, {"outside", "Outside"}, {"both", "Both"}}, "Lines inside the shape, around it, or both"),
            integer("count", "Count", 6, 1, 40, "How many lines"),
            px("spacing", "Spacing", 9, 2, 200, "The distance between two lines", 0.5),
            px("width", "Width", 2.5, 0.5, 40, "How wide each line is", 0.5),
            live(px("offset", "Offset", 4, 0, 200, "How far the first line sits from the edge", 0.5)),
            number("falloff", "Falloff", 0.6, 0, 1, ParamUnit::None, "How much the farther lines fade"),
            gradient("gradient", "Gradient", rainbowSpectrum(), "The lines' colours", true),
            choice("color_by", "Color By", "across", {{"across", "Across the Layer"}, {"line", "Line Number"}},
                   "The gradient runs across the layer, or from the first line to the last"),
            when(degrees("angle", "Angle", -45, "Direction of the gradient across the layer, clockwise"), "color_by", {"across"}),
            multiplies(live(number("intensity", "Intensity", 1.2, 0, 6, ParamUnit::Factor, "How bright the lines are"))),
            px("glow_size", "Glow Size", 12, 0, 200, "How far the lines' glow reaches"),
            multiplies(live(number("glow", "Glow", 0.8, 0, 4, ParamUnit::Factor, "How bright the glow is"))),
            number("dim", "Dim Face", 0, 0, 1, ParamUnit::None, "Darkens the layer under the lines"),
            choice("follow", "Follow", "layer", {{"layer", "Layer"}, {"stack", "Stack Below"}},
                   "Trace the layer's own silhouette, or everything the effects above drew (light left out either way)"),
        };
        d.gizmos = {{GizmoKind::Direction, "angle", nullptr}};
        d.reach = [](const ParamValues& p) { return (p.string("mode") == "inside" ? 0.0f : contourReach(p)) + (float)p.number("glow_size"); };
        d.addsLight = [](const ParamValues&) { return true; };
        d.outline = true;
        d.costClass = CostClass::C2;
        d.fallback = FallbackClass::F2;
        d.live = {LiveKind::Kernel, {"offset", "intensity", "glow"}, (uint32_t)Program::ContourLines};
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"light guide", "導光板", "疑似導光板", "contourLines", "rings", "offset lines", "neon rings"};
        d.inputs.content = true;
        d.inputs.sdf = true;
        d.estimate = estimateContourLines;
        d.presetFile = PRESETS_CONTOUR;
        d.apply = applyContourLines;
        return d;
    }

    // ---- Neon Tube ----

    float neonReach(const ParamValues& p) {
        return (float)(std::max(0.0, p.number("offset")) + 0.5 * p.number("width") + p.number("glow_size"));
    }

    void applyNeonTube(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float s = ctx.pxScale;
        const TargetRef& content = ctx.content ? *ctx.content : input;
        const bool fill = p.string("source") == "fill";
        const float w = std::max(1.0f, (float)p.number("width") * s);
        const float off = (float)p.number("offset") * s;
        fx::Scratch field = fx::distanceField(ctx, content, 0.5f, (fill ? w : std::fabs(off) + 0.5f * w) + 3.0f);
        fx::Scratch tube = fx::scratch(ctx);
        if (!field || !tube) return;
        const float H = tfx::lightHeadroom(ctx.runner.caps());
        TitlePass t = tfx::makePass(ctx, tfx::Program::NeonTube, "neon_tube tube", *tube);
        t.raw[0].target = field.get();
        const TitleColor c = fx::linearOf(p.color("color")), core = fx::linearOf(p.color("core_color"));
        const TitleVec2 light = tfx::lightToward(p.number("light_angle"));
        const TitleVec2 centre = tfx::canvasCentre(ctx);
        t.set(2, w, off, fill ? 1.0f : 0.0f, (float)p.number("core"));
        t.set(3, c.r, c.g, c.b, (float)p.number("flicker"));
        t.set(4, core.r, core.g, core.b, (float)p.number("gaps"));
        t.set(5, (float)light.x, (float)light.y, tfx::seedOf(ctx, p), 1.0f / H);
        t.set(6, (float)centre.x, (float)centre.y);
        if (!ctx.runner.run(t)) return;
        fx::Scratch glow;
        const double glowSize = p.number("glow_size");
        if (glowSize * s > 0.5) {
            tfx::ScatterOptions o;
            // the tube holds light / headroom: the gain gives it back
            o.gain = H;
            o.octaves = tfx::psfOctaves(std::max(4.0, glowSize), tfx::falloffExponent(50.0), 0.0, ctx.tier == FxTier::Mobile ? 4 : 0, s);
            o.intensity = 1.0f;
            glow = tfx::scatterLight(ctx, *tube, o);
            if (!glow) return;
        }
        TitlePass m = tfx::makePass(ctx, tfx::Program::NeonCompose, "neon_tube compose", output);
        m.textures[0].target = &input;
        m.textures[1].target = tube.get();
        m.textures[2].target = glow ? glow.get() : tube.get();
        m.set(2, glow ? (float)p.number("glow") : 0.0f, H, 0.6f, 0.3f);
        ctx.runner.run(m);
    }

    FxCost estimateNeonTube(const ParamValues& p, const FxSize& size, FxTier tier) {
        tfx::CostBuilder b;
        b.field();
        b.pass(1.0, 6);
        const double glowSize = p.number("glow_size");
        if (glowSize * size.pxScale > 0.5) {
            const auto octaves = tfx::psfOctaves(std::max(4.0, glowSize), tfx::falloffExponent(50.0), 0.0, tier == FxTier::Mobile ? 4 : 0, size.pxScale);
            const FxCost s = tfx::estimateScatter(octaves, size, tier);
            b.cost.passes += s.passes;
            b.cost.fre += s.fre;
            b.cost.maxTapsPerPixel = std::max(b.cost.maxTapsPerPixel, s.maxTapsPerPixel);
        }
        b.pass(1.0, 3);
        return b.cost;
    }

    EffectDef neonTube() {
        using namespace tfx;
        EffectDef d;
        d.type = "neon_tube";
        d.label = "Neon Tube";
        d.category = "Glow";
        d.description = "A glowing glass neon tube along the silhouette (or filling a thin layer), with a white-hot core, a light glow, "
                        "broken-tube gaps and flicker";
        d.params = {
            choice("source", "Source", "outline", {{"outline", "Outline"}, {"fill", "Fill"}},
                   "A tube along the edge, or the layer's own thin shape as the tube"),
            px("width", "Width", 8, 1, 100, "The tube's thickness", 0.5),
            when(px("offset", "Offset", 0, -100, 100, "The tube's centre from the edge (+ outside)", 0.5), "source", {"outline"}),
            color("color", "Color", "#ff2bd6", "The tube's colour", true),
            color("core_color", "Core Color", "#ffffff", "The hot core's colour", true),
            multiplies(live(number("core", "Core", 0.6, 0, 1, ParamUnit::None, "How bright the white-hot core is"))),
            px("glow_size", "Glow Size", 40, 0, 400, "How far the glow reaches"),
            multiplies(live(number("glow", "Glow", 1, 0, 4, ParamUnit::Factor, "How bright the glow is"))),
            number("gaps", "Gaps", 0, 0, 1, ParamUnit::None, "Breaks along the tube (a broken sign)"),
            number("flicker", "Flicker", 0, 0, 1, ParamUnit::None, "How much some sections dim; animate Seed to strobe"),
            degrees("light_angle", "Light Angle", 120, "Where the light on the glass comes from (Photoshop's angle)"),
            liveStepped(seed("seed", "Seed", "The gaps' and flicker's random pattern; reseed to flicker")),
        };
        d = withGlobalLight(std::move(d), "light_angle", LightRelation::From);
        d.gizmos = {{GizmoKind::Radius, "width", nullptr}, {GizmoKind::Light, "light_angle", nullptr}};
        d.reach = [](const ParamValues& p) { return neonReach(p); };
        d.gather = [](const ParamValues& p) { return 1.25f * (float)p.number("glow_size") + (float)p.number("width"); };
        d.addsLight = [](const ParamValues&) { return true; };
        d.costClass = CostClass::C2;
        d.fallback = FallbackClass::F2;
        d.live = {LiveKind::Kernel, {"core", "glow", "seed"}, (uint32_t)Program::NeonCompose};
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"neon", "neon sign", "tube", "ネオン", "ネオン管"};
        d.inputs.content = true;
        d.inputs.sdf = true;
        d.estimate = estimateNeonTube;
        d.presetFile = PRESETS_NEON;
        d.apply = applyNeonTube;
        return d;
    }

}

void titleart::registerLightEffects(TitleEffectRegistry& registry) {
    registry.add(lightGlow());
    registry.add(contourLines());
    registry.add(neonTube());
}
