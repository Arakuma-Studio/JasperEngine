// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Motion Blur, Lens Blur, Tilt-Shift Blur and Radial Blur (docs/specs/S83-title-effects-generators.md
// §6.6.3-§6.6.6), ported from Pachislop src/effects/a/blur.ts (motionBlur, lensBlur, tiltShift) and
// src/effects/a/distort.ts (radialBlur) by SafEight, used with the author's permission.

#include "TfxCommon.h"
#include "TfxFamilies.h"
#include "TfxPrograms.h"
#include "TitleFx.h"
#include "TitleFxParams.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fxparams;

namespace {

    constexpr double DEG = 3.14159265358979323846 / 180.0;

    int ceilDiv(int v, int d) {
        return std::max(1, (v + d - 1) / d);
    }

    // ---- Motion Blur ----

    struct MotionStep {
        int n;
        float spacing, start;
    };

    // a box of length D along the direction built exactly as comb(32 taps, D/32) * ... * box(the
    // last span), so long blurs stay <= 64 taps per pass and the last pass samples <= 1 px apart
    std::vector<MotionStep> motionSteps(float d, bool centred) {
        std::vector<MotionStep> steps;
        float span = d;
        float start = centred ? -d / 2.0f : 0.0f;
        while (span > 64.0f) {
            steps.push_back({32, span / 32.0f, start});
            span /= 32.0f;
            start = 0.0f;
        }
        const int n = std::max(1, (int)std::ceil(span));
        steps.push_back({n, span / (float)n, start + span / (float)n / 2.0f});
        return steps;
    }

    void applyMotionBlur(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float d = (float)std::max(0.0, p.number("distance")) * ctx.pxScale;
        if (d < 0.5f) {
            fx::copy(ctx, input, output);
            return;
        }
        // the trail lies toward the angle, so a pixel gathers from the opposite side
        const TitleVec2 dir = fx::dirClockwise(p.number("angle"));
        const std::vector<MotionStep> steps = motionSteps(d, p.string("mode") != "trail");
        fx::Scratch tmp[2];
        if (steps.size() > 1) {
            tmp[0] = fx::scratch(ctx);
            tmp[1] = fx::scratch(ctx);
            if (!tmp[0] || !tmp[1]) return;
        }
        const TargetRef* src = &input;
        for (size_t i = 0; i < steps.size(); i++) {
            TargetRef& target = i + 1 == steps.size() ? output : *tmp[i % 2];
            TitlePass pass = tfx::makePass(ctx, tfx::Program::MotionBlur, "motion_blur", target);
            pass.textures[0].target = src;
            pass.set(2, (float)(-dir.x / ctx.width), (float)(-dir.y / ctx.height), steps[i].start, steps[i].spacing);
            pass.set(3, (float)steps[i].n);
            if (!ctx.runner.run(pass)) return;
            src = &target;
        }
    }

    FxCost estimateMotionBlur(const ParamValues& p, const FxSize& size, FxTier) {
        tfx::CostBuilder b;
        const float d = (float)std::max(0.0, p.number("distance")) * size.pxScale;
        if (d < 0.5f) return b.pass(1.0, 1).cost;
        for (const MotionStep& s : motionSteps(d, true)) b.pass(1.0, s.n);
        return b.cost;
    }

    EffectDef motionBlur() {
        EffectDef d;
        d.type = "motion_blur";
        d.label = "Motion Blur";
        d.category = "Blur";
        d.description = "Smears the layer evenly along a direction over Distance, centred on it, or one-sided as a trail left behind the motion";
        d.params = {
            degrees("angle", "Angle", 0, "The motion's direction, clockwise (0 = right)"),
            px("distance", "Distance", 40, 0, 2000, "How far the layer smears"),
            choice("mode", "Type", "centered", {{"centered", "Centered"}, {"trail", "Trail"}}, "Centred on the layer, or a one-sided trail"),
        };
        d.gizmos = {{GizmoKind::Direction, "angle", "distance"}};
        d.reach = [](const ParamValues& p) { return (float)(p.number("distance") * (p.string("mode") == "trail" ? 1.0 : 0.5)); };
        d.gather = [](const ParamValues& p) { return (float)p.number("distance"); };
        d.costClass = CostClass::C2;
        d.fallback = FallbackClass::F1;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"motionBlur", "speed blur", "directional blur", "streak", "動き", "モーションブラー"};
        d.estimate = estimateMotionBlur;
        d.presetFile = "editor/resources/titleart/fx-presets/motion_blur.yaml";
        d.apply = applyMotionBlur;
        return d;
    }

    // ---- Lens Blur ----

    const int BLADES[] = {0, 6, 8};

    void applyLensBlur(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float r = (float)std::max(0.0, p.number("radius")) * ctx.pxScale;
        const float thr = (float)p.number("threshold") / 255.0f;
        const float gain = (float)std::pow(std::max(0.0, p.number("brightness")) / 100.0, 2.0) * 400.0f;
        const bool mobile = ctx.tier == FxTier::Mobile;
        fx::Scratch levels[11];
        fx::Scratch gathered;
        const TargetRef* blurred = &input;
        if (r >= 0.5f) {
            // halve until the radius is <= 10 level px (the mobile tier: <= 5, a quarter-resolution
            // 64-tap gather, D12), then gather ~1.6 taps per level px^2
            const float cap = mobile ? 5.0f : 10.0f;
            const TargetRef* cur = &input;
            int w = ctx.width, h = ctx.height;
            for (int i = 1; (r / std::ldexp(1.0f, i - 1) > cap || (mobile && i <= 2)) && i <= 10; i++) {
                const int nw = ceilDiv(w, 2), nh = ceilDiv(h, 2);
                levels[i] = fx::scratch(ctx, nw, nh);
                if (!levels[i]) return;
                TitlePass down = tfx::makePass(ctx, tfx::Program::LensDown, "lens_blur down", *levels[i]);
                down.textures[0].target = cur;
                down.set(2, 1.0f / (float)w, 1.0f / (float)h, thr, gain);
                if (!ctx.runner.run(down)) return;
                cur = levels[i].get();
                w = nw;
                h = nh;
            }
            const float rl = r * (float)w / (float)ctx.width;
            const int taps = (int)std::lround(std::min(mobile ? 64.0 : 512.0, std::max(16.0, 1.6 * 3.14159265358979 * rl * rl)));
            gathered = fx::scratch(ctx, w, h);
            if (!gathered) return;
            TitlePass g = tfx::makePass(ctx, tfx::Program::LensGather, "lens_blur gather", *gathered);
            g.textures[0].target = cur;
            g.set(2, r / (float)ctx.width, r / (float)ctx.height, (float)taps, (float)BLADES[optionIndex(p, "shape", {"circle", "hexagon", "octagon"})]);
            g.set(3, (float)(-p.number("rotation") * DEG), thr, gain);
            if (!ctx.runner.run(g)) return;
            blurred = gathered.get();
        }
        TitlePass f = tfx::makePass(ctx, tfx::Program::LensFinish, "lens_blur finish", output);
        f.textures[0].target = blurred;
        f.set(2, (float)std::max(0.0, p.number("noise")) / 255.0f, p.string("distribution") == "gaussian" ? 1.0f : 0.0f, p.flag("mono") ? 1.0f : 0.0f,
              tfx::seedOf(ctx, p));
        ctx.runner.run(f);
    }

    FxCost estimateLensBlur(const ParamValues& p, const FxSize& size, FxTier tier) {
        tfx::CostBuilder b;
        const float r = (float)std::max(0.0, p.number("radius")) * size.pxScale;
        const bool mobile = tier == FxTier::Mobile;
        if (r >= 0.5f) {
            const float cap = mobile ? 5.0f : 10.0f;
            int l = 0;
            double frac = 1.0;
            for (int i = 1; (r / std::ldexp(1.0f, i - 1) > cap || (mobile && i <= 2)) && i <= 10; i++) {
                frac /= 4.0;
                b.pass(frac, 4);
                l = i;
            }
            const double rl = r / std::ldexp(1.0, l);
            const int taps = (int)std::lround(std::min(mobile ? 64.0 : 512.0, std::max(16.0, 1.6 * 3.14159265358979 * rl * rl)));
            b.pass(frac, taps);
        }
        return b.pass(1.0, 1).cost;
    }

    EffectDef lensBlur() {
        EffectDef d;
        d.type = "lens_blur";
        d.label = "Lens Blur";
        d.category = "Blur";
        d.description = "An out-of-focus camera blur through an iris (circle, hexagon or octagon) in which bright points bloom into hard-edged "
                        "bokeh discs; Noise adds grain back so the smooth blur does not band";
        d.params = {
            px("radius", "Radius", 20, 0, 300, "How out of focus the layer is"),
            choice("shape", "Iris Shape", "hexagon", {{"circle", "Circle"}, {"hexagon", "Hexagon"}, {"octagon", "Octagon"}}, "The shape of the bokeh"),
            degrees("rotation", "Blade Rotation", 0, "Turns the iris", 0, 360),
            number("brightness", "Highlight Brightness", 15, 0, 100, ParamUnit::None, "How much the highlights take over their disc", 1),
            number("threshold", "Highlight Threshold", 235, 0, 255, ParamUnit::None, "The brightness (0-255) a highlight starts at", 1),
            number("noise", "Noise Amount", 2, 0, 100, ParamUnit::None, "Grain added back, in 8-bit levels", 1),
            choice("distribution", "Noise Distribution", "uniform", {{"uniform", "Uniform"}, {"gaussian", "Gaussian"}}, "The grain's distribution"),
            flag("mono", "Monochromatic", true, "Grey grain instead of coloured"),
            seed("seed", "Seed", "The grain's random pattern"),
        };
        d.gizmos = {{GizmoKind::Radius, "radius", nullptr}};
        d.reach = [](const ParamValues& p) { return (float)p.number("radius"); };
        d.gather = [](const ParamValues& p) { return (float)p.number("radius"); };
        d.costClass = CostClass::C4;
        d.fallback = FallbackClass::F1;
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"lensBlur", "bokeh", "defocus", "depth of field", "ぼかし (レンズ)", "ボケ"};
        d.estimate = estimateLensBlur;
        d.presetFile = "editor/resources/titleart/fx-presets/lens_blur.yaml";
        d.apply = applyLensBlur;
        return d;
    }

    // ---- Tilt-Shift Blur ----

    void applyTiltShift(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float s = ctx.pxScale;
        const float sigma = (float)std::max(0.0, p.number("blur")) * s / 2.0f;
        if (sigma < 0.3f) {
            fx::copy(ctx, input, output);
            return;
        }
        fx::Scratch levels[4];
        const float factors[4] = {1.0f / 8.0f, 1.0f / 4.0f, 1.0f / 2.0f, 1.0f};
        for (int i = 0; i < 4; i++) {
            levels[i] = fx::scratch(ctx);
            if (!levels[i] || !fx::gauss(ctx, input, sigma * factors[i], sigma * factors[i], true, *levels[i])) return;
        }
        const TitleVec2 dir = fx::dirClockwise(p.number("angle"));
        // the focus centre is a canvas position
        const TitleVec2 c = tfx::canvasPoint(ctx, p.vec2("center"));
        ctx.usesCanvasPosition = true;
        const float half = (float)std::max(0.0, p.number("width")) * s / 2.0f, fall = (float)std::max(0.0, p.number("falloff")) * s;
        auto setBand = [&](TitlePass& pass) {
            pass.set(2, (float)c.x, (float)c.y, half, fall);
            pass.set(3, (float)-dir.y, (float)dir.x);
        };
        fx::Scratch farMix = fx::scratch(ctx);
        if (!farMix) return;
        TitlePass f = tfx::makePass(ctx, tfx::Program::TiltFar, "tilt_shift far", *farMix);
        f.textures[0].target = levels[2].get();
        f.textures[1].target = levels[3].get();
        setBand(f);
        if (!ctx.runner.run(f)) return;
        TitlePass b = tfx::makePass(ctx, tfx::Program::TiltBlend, "tilt_shift blend", output);
        b.textures[0].target = &input;
        b.textures[1].target = levels[0].get();
        b.textures[2].target = levels[1].get();
        b.textures[3].target = farMix.get();
        setBand(b);
        ctx.runner.run(b);
    }

    FxCost estimateTiltShift(const ParamValues& p, const FxSize& size, FxTier) {
        tfx::CostBuilder b;
        const double sigma = std::max(0.0, p.number("blur")) * size.pxScale / 2.0;
        if (sigma < 0.3) return b.pass(1.0, 1).cost;
        for (double f : {0.125, 0.25, 0.5, 1.0}) {
            double sg = sigma * f, frac = 1.0;
            while (sg > 6.0) {
                sg /= 2.0;
                frac /= 4.0;
                b.pass(frac, 1);
            }
            b.pass(frac, 2 * (int)std::ceil(3.0 * sg) + 1, 2);
            b.pass(1.0, 1);
        }
        return b.pass(1.0, 2).pass(1.0, 4).cost;
    }

    EffectDef tiltShift() {
        using namespace tfx;
        EffectDef d;
        d.type = "tilt_shift";
        d.label = "Tilt-Shift Blur";
        d.category = "Blur";
        d.description = "A sharp focus band through a centre point, blurring smoothly more and more beyond it up to Blur: the miniature look";
        d.params = {
            px("blur", "Blur", 16, 0, 200, "The most blur, at the far edges"),
            live(vec2("center", "Center", 0, 0, "The focus band's centre, canvas px from the canvas centre")),
            live(degrees("angle", "Angle", 0, "Tilts the band, clockwise (0 = horizontal)")),
            live(px("width", "Sharp Width", 160, 0, 4000, "How wide the sharp band is")),
            live(px("falloff", "Falloff", 240, 0, 4000, "How far the blur takes to reach its most")),
        };
        d.gizmos = {{GizmoKind::Point, "center", nullptr}, {GizmoKind::Direction, "angle", nullptr}};
        d.reach = [](const ParamValues& p) { return 1.5f * (float)p.number("blur"); };
        d.gather = [](const ParamValues& p) { return 2.0f * (float)p.number("blur"); };
        d.costClass = CostClass::C3;
        d.fallback = FallbackClass::F1;
        d.live = {LiveKind::Kernel, {"center", "angle", "width", "falloff"}, (uint32_t)Program::TiltBlend};
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"tiltShift", "miniature", "field blur", "focus band", "ミニチュア"};
        d.estimate = estimateTiltShift;
        d.presetFile = "editor/resources/titleart/fx-presets/tilt_shift.yaml";
        d.apply = applyTiltShift;
        return d;
    }

    // ---- Radial Blur ----

    void applyRadialBlur(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const int type = optionIndex(p, "type", {"zoom", "spin"});
        const TitleVec2 c = tfx::canvasPoint(ctx, p.vec2("center"));
        const double diag = std::hypot((double)ctx.frame.width, (double)ctx.frame.height);
        const double span = type == 0 ? -std::log(1.0 - std::max(0.0, std::min(0.95, p.number("amount")))) : p.number("spin") * DEG;
        // the longest tap travel in render px (zoom: ~ span x the distance to the far corner)
        const double travel = std::fabs(span) * diag * (type == 0 ? 0.6 : 0.5);
        const bool keep = p.flag("keep_original");
        ctx.usesCanvasPosition = true;
        auto run = [&](const TargetRef& src, TargetRef& dst, double sp, int n, bool keepOriginal) {
            TitlePass pass = tfx::makePass(ctx, tfx::Program::RadialBlur, "radial_blur", dst);
            pass.textures[0].target = &src;
            pass.textures[1].target = &input;
            pass.set(2, (float)c.x, (float)c.y, (float)sp, (float)n);
            pass.set(3, (float)type, keepOriginal ? 1.0f : 0.0f);
            return ctx.runner.run(pass);
        };
        if (travel <= 48.0) {
            run(input, output, span, std::max(8, (int)std::ceil(travel)), keep);
            return;
        }
        const int n1 = 32;
        fx::Scratch tmp = fx::scratch(ctx);
        if (!tmp || !run(input, *tmp, span, n1, false)) return;
        run(*tmp, output, span / n1, std::min(64, (int)std::ceil(travel / n1) + 1), keep);
    }

    FxCost estimateRadialBlur(const ParamValues& p, const FxSize& size, FxTier) {
        tfx::CostBuilder b;
        const bool zoom = p.string("type") != "spin";
        const double span = zoom ? -std::log(1.0 - std::max(0.0, std::min(0.95, p.number("amount")))) : p.number("spin") * DEG;
        const double travel = std::fabs(span) * std::hypot((double)size.width, (double)size.height) * (zoom ? 0.6 : 0.5);
        if (travel <= 48.0) return b.pass(1.0, std::max(8, (int)std::ceil(travel))).cost;
        return b.pass(1.0, 32).pass(1.0, std::min(64, (int)std::ceil(travel / 32.0) + 1)).cost;
    }

    EffectDef radialBlur() {
        using namespace tfx;
        EffectDef d;
        d.type = "radial_blur";
        d.label = "Radial Blur";
        d.category = "Blur";
        d.description = "A zoom blur streaking toward a centre point (an impact cut-in), or a spin blur around it";
        d.params = {
            choice("type", "Type", "zoom", {{"zoom", "Zoom"}, {"spin", "Spin"}}, "Streaks toward the centre, or around it"),
            when(live(number("amount", "Zoom Amount", 0.25, 0, 0.95, ParamUnit::None, "How long the zoom streaks are")), "type", {"zoom"}),
            when(live(degrees("spin", "Spin Angle", 12, "How far the spin smears", 0, 180)), "type", {"spin"}),
            vec2("center", "Center", 0, 0, "The blur's centre, canvas px from the canvas centre"),
            flag("keep_original", "Original On Top", true, "Keeps the sharp layer over its blur"),
        };
        d.gizmos = {{GizmoKind::Point, "center", nullptr}};
        d.gather = [](const ParamValues&) { return -1.0f; };
        // the streaks run out from (or around) a centre anywhere on the canvas: the whole canvas
        d.fillsCanvas = [](const ParamValues&) { return true; };
        d.costClass = CostClass::C3;
        d.fallback = FallbackClass::F1;
        d.live = {LiveKind::Kernel, {"amount", "spin"}, (uint32_t)Program::RadialBlur};
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"radialBlur", "zoom blur", "spin blur", "impact", "集中ブラー", "放射状ぼかし"};
        d.estimate = estimateRadialBlur;
        d.presetFile = "editor/resources/titleart/fx-presets/radial_blur.yaml";
        d.apply = applyRadialBlur;
        return d;
    }

}

void titleart::registerCatalogueBlurEffects(TitleEffectRegistry& registry) {
    registry.add(motionBlur());
    registry.add(lensBlur());
    registry.add(tiltShift());
    registry.add(radialBlur());
}
