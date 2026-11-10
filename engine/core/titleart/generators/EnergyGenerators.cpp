// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Energy generators (docs/specs/S83-title-effects-generators.md §6.7.11-6.7.14): Fire, Smoke,
// Aura and Lightning, growing from the pixels of the layer they follow before its own effects.
// Ported from Pachislop src/generators/energy/{fire,smoke,corona,lightning,shared}.ts by
// SafEight, used with the author's permission.

#include "titleart/analysis/BoltBuilder.h"
#include "titleart/analysis/ReadGrid.h"
#include "titleart/generators/TitleGen.h"
#include "titleart/generators/TitleGenFamilies.h"
#include "titleart/generators/TitleGenMobile.h"
#include "titleart/gpu/QuadBatch.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fxparams;
using namespace jasperengine::titleart::gen;

namespace {

    constexpr double DEG = 3.14159265358979323846 / 180.0;

    // ---- shared ----

    // a Required generator without its layer draws nothing (check_title reports it); Pachislop
    // clears when the layer has no content
    bool hasFollowed(GeneratorContext& ctx, TargetRef& target) {
        if (ctx.follow && ctx.followContent) return true;
        ctx.runner.clear(target);
        return false;
    }

    // the flow direction of an `angle` param as Pachislop's (cos, sin): ours turns clockwise on
    // screen (D16), its counter-clockwise in the y-up drawing space
    TitleVec2 flowOf(const ParamValues& p) {
        const double a = -p.number("angle") * DEG;
        return {std::cos(a), std::sin(a)};
    }

    // Pachislop's `seed % 97` uniforms
    float noiseSeed(const GeneratorContext& ctx, const ParamValues& p) {
        return (float)std::fmod(seedOf(ctx, p), 97.0);
    }

    // 0% -> only straight up from edges; 100% -> sides burn nearly as tall as tops
    float sideOf(const ParamValues& p) {
        return (float)(1.0 / std::max(0.08, p.number("spread") / 100.0));
    }

    // fx::blur's work as the cost model counts it (§6.4): the downsample copies, two 1-D
    // Gaussians at the level it runs at and the copy back, in full-resolution equivalents
    struct BlurCost {
        int passes = 1;
        double fre = 1.0;
        int taps = 1;
    };

    BlurCost blurCost(double radiusPx) {
        BlurCost c;
        const double sigma = std::max(0.0, radiusPx / 3.0);
        if (sigma < 0.3) return c;
        int level = 0;
        while (sigma / std::ldexp(1.0, level) > 6.0 && level < 5) level++;
        for (int l = 1; l <= level; l++) {
            c.fre += 1.0 / std::ldexp(1.0, 2 * l);
            c.passes++;
        }
        c.taps = std::min(49, 2 * (int)std::ceil(3.0 * sigma / std::ldexp(1.0, level)) + 1);
        c.fre += 2.0 / std::ldexp(1.0, 2 * level) * std::max(1.0, c.taps / 4.0);
        c.passes += 2;
        return c;
    }

    // a fullscreen pass of `taps` taps (a value noise counts two, a distance read one)
    double passFre(int taps) {
        return std::max(1.0, taps / 4.0);
    }

    // the mobile tier (§6.7.11-6.7.13) draws Fire, Smoke and Aura at half resolution
    // (drawReduced): a quarter of the pixels, plus the content's downsample there and the
    // copy back to full size
    constexpr int ENERGY_MOBILE_DIVISOR = 2;
    constexpr double ENERGY_MOBILE_SHARE = 1.0 / (ENERGY_MOBILE_DIVISOR * ENERGY_MOBILE_DIVISOR);
    constexpr int ENERGY_MOBILE_PASSES = 2;
    constexpr double ENERGY_MOBILE_FRE = ENERGY_MOBILE_SHARE + 1.0;

    // the mobile flame and smoke passes' taps: the baked field, three noise fetches (and Fire's
    // gradient and over-edge distance)
    constexpr int FIRE_MOBILE_TAPS = 6, SMOKE_MOBILE_TAPS = 4;
    // the upstream march's distance reads (tfx_upstream.glsl)
    constexpr int UPSTREAM_TAPS = 40;

    // the mobile tier's Fire and Smoke cost: the content's downsample, the baked field (the
    // 40-step march at half resolution), the flame or smoke pass at half resolution and the copy
    // back; the content's distance field is the shared SDF row
    FxCost energyMobileCost(int flameTaps) {
        FxCost c = passCost(2 + ENERGY_MOBILE_PASSES, ENERGY_MOBILE_SHARE * (passFre(UPSTREAM_TAPS) + passFre(flameTaps)) + ENERGY_MOBILE_FRE,
                            UPSTREAM_TAPS);
        c.sdf = true;
        return c;
    }

    // ---- Fire ----

    float fireReach(const ParamValues& p) {
        // the longest tongue = 1.6 x height (turbulent), plus the sideways sway of the flame body
        return (float)(1.6 * p.number("height") + 0.7 * p.number("width") * (p.number("turbulence") / 100.0));
    }

    // the flames from `content` into `out`, both of c's size: the desktop pass marching the
    // upstream distance per pixel, or on the mobile tier (§6.7.11) E baked into a field first
    // and the flames reading it with the tiling noise texture
    void firePasses(GeneratorContext& c, TargetRef& out, const TargetRef& content, const ParamValues& p, bool mobile) {
        const double H = std::max(1.0, p.number("height"));
        const double inside = std::max(0.5, p.number("inside"));
        // the march reads the field out to the longest flame (beyond it a flame is gone at
        // any distance) and the over-edge band inside the shape
        EffectContext e = effectContext(c);
        fx::Scratch field = fx::distanceField(e, content, 0.5f, (float)(std::max(1.6 * H, inside) * c.pxScale + 2.0));
        const TitleLut* ramp = c.runner.lut(fx::gradientLut(p.gradient("gradient")));
        if (!field || !ramp) return;
        const TitleVec2 dir = flowOf(p);
        fx::Scratch baked;
        const TitleLut* noise = nullptr;
        // the longest tongue (fire.glsl): the desktop march's length, the baked field's too
        const double marchPx = 1.6 * H * spaceScale(c.documentToTarget);
        if (mobile) {
            baked = upstreamField(c, *field, dir, marchPx, sideOf(p));
            noise = c.runner.lut(mobileNoiseData());
            if (!baked || !noise) return;
        }
        TitlePass pass = genPass(c, mobile ? GenProgram::FireMobile : GenProgram::Fire, "fire", out, c.documentToTarget);
        pass.textures[0].lut = ramp;
        pass.raw[0].target = field.get();
        pass.set(6, (float)dir.x, (float)dir.y, (float)H, (float)std::max(1.0, p.number("width")));
        pass.set(7, (float)(p.number("turbulence") / 100.0), (float)p.number("phase"), noiseSeed(c, p), sideOf(p));
        pass.set(8, (float)(p.number("intensity") / 100.0), (float)inside, p.flag("over_edge") ? 1.0f : 0.0f, 0.0f);
        if (mobile) {
            pass.textures[1].lut = noise;
            pass.raw[1].target = baked.get();
            pass.set(9, (float)std::max(1.0, marchPx));
        }
        c.runner.run(pass);
    }

    void renderFire(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        if (!hasFollowed(ctx, target)) return;
        const TargetRef& content = *ctx.followContent;
        drawFollowed(ctx, target, [&](TargetRef& out) {
            if (ctx.tier != FxTier::Mobile) {
                firePasses(ctx, out, content, p, false);
                return;
            }
            drawReduced(ctx, out, ENERGY_MOBILE_DIVISOR, [&](GeneratorContext& half, TargetRef& halfOut) {
                firePasses(half, halfOut, *half.followContent, p, true);
            });
        });
    }

    FxCost fireCost(const ParamValues&, const FxSize&, FxTier tier) {
        // desktop: one pass over the target, 40 distance taps of the upstream march and 12 value
        // noises. Mobile (§6.7.11): at half resolution, E baked to a field, the noise from the
        // 256² tiling RG texture (3 fetches)
        if (tier == FxTier::Mobile) return energyMobileCost(FIRE_MOBILE_TAPS);
        FxCost c = passCost(1, passFre(UPSTREAM_TAPS + 12 * 2), UPSTREAM_TAPS + 12 * 2);
        c.sdf = true;
        return c;
    }

    GeneratorDef fire() {
        GeneratorDef d;
        d.type = "fire";
        d.label = "Fire";
        d.category = "Energy";
        d.description = "Flames rising from the edges of the layer it follows (Direction -90 = up), behind it, or over its edges where they "
                        "clear out inside the shape";
        d.params = {
            flag("over_edge", "Over the Edges", false, "The flames also lick over the outer band of the shape and clear out inside it"),
            px("height", "Flame Height", 150, 5, 600, "How tall the flames rise from the edge"),
            px("width", "Flame Width", 22, 3, 200, "The size of the tongues across the flow"),
            percent("turbulence", "Turbulence", 60, "How much the tongues differ in length and the flame body sways, percent", 0, 150),
            degrees("angle", "Direction", -90, "The direction the flames rise, clockwise from right (-90 = up)", -180, 180),
            percent("spread", "Side Burn", 35, "How tall the flames burn up the sides of the shape, percent of the tops"),
            when(px("inside", "Over-Edge Depth", 12, 0, 100, "How far into the shape the flames lick before clearing out"), "over_edge", {"true"}),
            live(number("phase", "Evolution", 0, 0, 10, ParamUnit::None, "Moves the flames through their flicker (key it to animate)", 0.01)),
            multiplies(percent("intensity", "Intensity", 110, "The flames' brightness, percent", 0, 300)),
            light(gradient("gradient", "Colors",
                           gradientValue({{0.0, "#ffffff"}, {0.18, "#fff27a"}, {0.4, "#ffb000"}, {0.65, "#ff4800"}, {0.85, "#c40c00"}, {1.0, "#4a0000"}}),
                           "The flame colours from the core to the tips")),
            seedParam("Reshuffles the tongues, sway and flicker"),
        };
        d.gizmos = {{GizmoKind::Direction, "angle", "height"}};
        d.follow = FollowMode::Required;
        d.followReach = fireReach;
        // the white-hot core and intensity carry the colour past 1
        d.addsLight = [](const ParamValues&) { return true; };
        d.costClass = CostClass::C3;
        d.fallback = FallbackClass::F2;
        d.live.kind = LiveKind::Kernel;
        d.live.params = {"phase"};
        d.live.program = (uint32_t)GenProgram::Fire | (uint32_t)TITLE_VARIANT_GENERATOR;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"fire", "flames", "burning", "炎", "火炎", "ファイア", "激アツ"};
        d.inputs.content = true;
        d.inputs.sdf = true;
        d.estimate = fireCost;
        d.presetFile = "editor/resources/titleart/fx-presets/fire.yaml";
        d.render = renderFire;
        return d;
    }

    // ---- Smoke ----

    float smokeReach(const ParamValues& p) {
        // the tallest plume = 1.35 x height, plus the sideways billow warp
        return (float)(1.35 * p.number("height") + p.number("width"));
    }

    // the smoke from `content` into `out`, both of c's size, as firePasses
    void smokePasses(GeneratorContext& c, TargetRef& out, const TargetRef& content, const ParamValues& p, bool mobile) {
        const double H = std::max(1.0, p.number("height"));
        // the march reads the field out to its own length, past the tallest plume
        EffectContext e = effectContext(c);
        fx::Scratch field = fx::distanceField(e, content, 0.5f, (float)(1.4 * H * c.pxScale + 2.0));
        if (!field) return;
        const TitleVec2 dir = flowOf(p);
        fx::Scratch baked;
        const TitleLut* noise = nullptr;
        // smoke.glsl's march length
        const double marchPx = 1.4 * H * spaceScale(c.documentToTarget);
        if (mobile) {
            baked = upstreamField(c, *field, dir, marchPx, sideOf(p));
            noise = c.runner.lut(mobileNoiseData());
            if (!baked || !noise) return;
        }
        const TitleColor col = fx::linearOf(p.color("color")), shadow = fx::linearOf(p.color("shadow_color"));
        TitlePass pass = genPass(c, mobile ? GenProgram::SmokeMobile : GenProgram::Smoke, "smoke", out, c.documentToTarget);
        pass.raw[0].target = field.get();
        pass.set(6, (float)dir.x, (float)dir.y, (float)H, (float)std::max(1.0, p.number("width")));
        pass.set(7, (float)p.number("phase"), noiseSeed(c, p), sideOf(p), (float)std::max(0.02, p.number("softness") / 100.0));
        pass.set(8, col.r, col.g, col.b, 0.0f);
        pass.set(9, shadow.r, shadow.g, shadow.b, 0.0f);
        if (mobile) {
            pass.textures[1].lut = noise;
            pass.raw[1].target = baked.get();
            pass.set(10, (float)std::max(1.0, marchPx));
        }
        c.runner.run(pass);
    }

    void renderSmoke(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        if (!hasFollowed(ctx, target)) return;
        const TargetRef& content = *ctx.followContent;
        drawFollowed(ctx, target, [&](TargetRef& out) {
            if (ctx.tier != FxTier::Mobile) {
                smokePasses(ctx, out, content, p, false);
                return;
            }
            drawReduced(ctx, out, ENERGY_MOBILE_DIVISOR, [&](GeneratorContext& half, TargetRef& halfOut) {
                smokePasses(half, halfOut, *half.followContent, p, true);
            });
        });
    }

    FxCost smokeCost(const ParamValues&, const FxSize&, FxTier tier) {
        // desktop: one pass, 40 distance taps and 12 value noises (four soft 3-octave sums).
        // Mobile: as Fire's, half resolution
        if (tier == FxTier::Mobile) return energyMobileCost(SMOKE_MOBILE_TAPS);
        FxCost c = passCost(1, passFre(UPSTREAM_TAPS + 12 * 2), UPSTREAM_TAPS + 12 * 2);
        c.sdf = true;
        return c;
    }

    GeneratorDef smoke() {
        GeneratorDef d;
        d.type = "smoke";
        d.label = "Smoke";
        d.category = "Energy";
        d.description = "Soft billowing smoke rising from the edges of the layer it follows (Direction -90 = up); put it below that layer";
        d.params = {
            px("height", "Smoke Height", 220, 10, 800, "How tall the smoke rises from the edge"),
            px("width", "Billow Size", 40, 5, 300, "The size of the billows"),
            degrees("angle", "Direction", -90, "The direction the smoke rises, clockwise from right (-90 = up)", -180, 180),
            percent("spread", "Side Spread", 35, "How tall the smoke rises from the sides of the shape, percent of the tops"),
            percent("softness", "Softness", 60, "How gradually the smoke fades out, percent", 5, 100),
            color("color", "Highlight Color", "#d8d2e6", "The colour of the lit billows"),
            color("shadow_color", "Shadow Color", "#4a4458", "The colour between the billows"),
            live(number("phase", "Evolution", 0, 0, 10, ParamUnit::None, "Drifts and churns the billows (key it to animate)", 0.01)),
            seedParam("Reshuffles the billows and plumes"),
        };
        d.gizmos = {{GizmoKind::Direction, "angle", "height"}};
        d.follow = FollowMode::Required;
        d.followReach = smokeReach;
        d.costClass = CostClass::C3;
        d.fallback = FallbackClass::F0;
        d.live.kind = LiveKind::Kernel;
        d.live.params = {"phase"};
        d.live.program = (uint32_t)GenProgram::Smoke | (uint32_t)TITLE_VARIANT_GENERATOR;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"smoke", "steam", "billowing smoke", "煙", "湯気", "スモーク"};
        d.inputs.content = true;
        d.inputs.sdf = true;
        d.estimate = smokeCost;
        d.presetFile = "editor/resources/titleart/fx-presets/smoke.yaml";
        d.render = renderSmoke;
        return d;
    }

    // ---- Aura ----

    bool auraRising(const ParamValues& p) {
        return p.string("mode") == "rising";
    }

    bool auraStrong(const ParamValues& p) {
        return p.string("strength") != "weak";
    }

    bool auraUp(const ParamValues& p) {
        return p.string("flow") == "up";
    }

    // the rising aura's tongue length: weak ones are shorter, dimmer and more broken (オーラ(弱)
    // beside オーラ(強))
    double auraTongue(const ParamValues& p) {
        return std::max(1.0, p.number("tongue")) * (auraStrong(p) ? 1.0 : 0.6);
    }

    float auraReach(const ParamValues& p) {
        // rising: the longest tongue is 1.5 x its length, rising flames 1.6 x taller
        if (auraRising(p)) return (float)((auraUp(p) ? 2.4 : 1.5) * p.number("tongue"));
        return (float)p.number("length");
    }

    // the distance the field must be exact to, canvas px: the tendrils fade out at their length
    // (the normals' blur reaching 0.3 of it further), a rising tongue at 1.5 x its length (2.4 x
    // rising up, where the march runs that far)
    double auraFieldReach(const ParamValues& p) {
        if (auraRising(p)) return (auraUp(p) ? 2.4 : 1.6) * auraTongue(p);
        return 1.6 * std::max(1.0, p.number("length"));
    }

    // Pachislop's blurred field for outward normals, as its gradient blurred (aura.glsl): the
    // central differences packed by AuraNormals, then fx::blur of radius blurPx
    fx::Scratch auraNormals(GeneratorContext& ctx, EffectContext& e, const TargetRef& field, float blurPx) {
        fx::Scratch diffs = fx::scratchOf(ctx.runner, TitleBuffer::Accumulator, ctx.width, ctx.height);
        if (!diffs) return {};
        TitlePass pass = genPass(ctx, GenProgram::AuraNormals, "aura normals", *diffs, ctx.documentToTarget);
        pass.raw[0].target = &field;
        if (!ctx.runner.run(pass)) return {};
        return fx::blur(e, *diffs, blurPx);
    }

    // the aura from `content` into `out`, both of c's size; `mobile` takes 3-octave fbm
    // (§6.7.13: drawn at half resolution by renderAura)
    void auraPasses(GeneratorContext& c, TargetRef& out, const TargetRef& content, const ParamValues& p, bool mobile) {
        EffectContext e = effectContext(c);
        fx::Scratch field = fx::distanceField(e, content, 0.5f, (float)(auraFieldReach(p) * c.pxScale + 2.0));
        const TitleLut* ramp = c.runner.lut(fx::gradientLut(p.gradient("gradient")));
        if (!field || !ramp) return;
        if (auraRising(p)) {
            const bool strong = auraStrong(p), up = auraUp(p);
            const double len = auraTongue(p);
            // rising up reads the upstream distance, outward the blurred normals
            fx::Scratch normals;
            if (!up) {
                normals = auraNormals(c, e, *field, (float)(len * c.pxScale * 0.3));
                if (!normals) return;
            }
            TitlePass pass = genPass(c, GenProgram::AuraRising, "aura", out, c.documentToTarget);
            pass.textures[0].lut = ramp;
            pass.textures[1].target = normals.get();
            pass.raw[0].target = field.get();
            pass.set(6, (float)len, (float)std::max(1.0, p.number("turbulence_scale")),
                     (float)std::min(1.0, p.number("breakup") / 100.0 + (strong ? 0.0 : 0.3)), (float)p.number("phase"));
            pass.set(7, (float)(p.number("intensity") / 100.0), noiseSeed(c, p), (float)(p.number("glow") / 100.0), strong ? 1.0f : 0.0f);
            pass.set(8, up ? 1.0f : 0.0f, mobile ? 1.0f : 0.0f);
            c.runner.run(pass);
            return;
        }
        fx::Scratch normals = auraNormals(c, e, *field, (float)(p.number("length") * c.pxScale * 0.3));
        if (!normals) return;
        TitlePass pass = genPass(c, GenProgram::AuraTendrils, "aura tendrils", out, c.documentToTarget);
        pass.textures[0].lut = ramp;
        pass.textures[1].target = normals.get();
        pass.raw[0].target = field.get();
        pass.set(6, (float)std::max(1.0, p.number("length")), (float)(p.number("wispiness") / 100.0), (float)p.number("phase"),
                 (float)(p.number("intensity") / 100.0));
        pass.set(7, (float)std::max(0.05, p.number("density") / 100.0), noiseSeed(c, p), (float)(p.number("glow") / 100.0), mobile ? 1.0f : 0.0f);
        c.runner.run(pass);
    }

    void renderAura(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        if (!hasFollowed(ctx, target)) return;
        const TargetRef& content = *ctx.followContent;
        drawFollowed(ctx, target, [&](TargetRef& out) {
            if (ctx.tier != FxTier::Mobile) {
                auraPasses(ctx, out, content, p, false);
                return;
            }
            drawReduced(ctx, out, ENERGY_MOBILE_DIVISOR, [&](GeneratorContext& half, TargetRef& halfOut) {
                auraPasses(half, halfOut, *half.followContent, p, true);
            });
        });
    }

    FxCost auraCost(const ParamValues& p, const FxSize& size, FxTier tier) {
        // tendrils: the normals (4 distance taps), their blur, the light (2 fbm, 2 ridged and the
        // pulse noise, the field, the normals and the ramp); rising up: one pass with the 40-tap
        // march, 3 fbm and a ridged noise; rising outward: the normals, their blur, 4 fbm and a
        // ridged noise. An fbm is 5 value noises; the mobile tier's (§6.7.13) 3, every pass at
        // half resolution (the blur's radius halved) plus the content's downsample and the copy back
        const bool mobile = tier == FxTier::Mobile;
        const int fbmNoises = mobile ? 3 : 5;
        const double share = mobile ? ENERGY_MOBILE_SHARE : 1.0;
        const bool rising = auraRising(p), up = rising && auraUp(p);
        const double blurPx = (rising ? auraTongue(p) : p.number("length")) * size.pxScale * 0.3 / (mobile ? ENERGY_MOBILE_DIVISOR : 1);
        const int noises = up ? 3 * fbmNoises + 1 : rising ? 4 * fbmNoises + 1 : 2 * fbmNoises + 3;
        const int taps = up ? UPSTREAM_TAPS + 1 + noises * 2 : 3 + noises * 2;
        FxCost c = passCost(1, share * passFre(taps), taps);
        if (!up) {
            const BlurCost b = blurCost(blurPx);
            c.passes += 1 + b.passes;
            c.fre += share * (passFre(4) + b.fre);
            c.maxTapsPerPixel = std::max(c.maxTapsPerPixel, b.taps);
        }
        if (mobile) {
            c.passes += ENERGY_MOBILE_PASSES;
            c.fre += ENERGY_MOBILE_FRE;
        }
        c.sdf = true;
        return c;
    }

    GeneratorDef aura() {
        GeneratorDef d;
        d.type = "aura";
        d.label = "Aura";
        d.category = "Energy";
        d.description = "Wispy energy tendrils radiating from the edges of the layer it follows, or an aura (オーラ, strong or weak) of "
                        "flickering tongues hugging its silhouette, drawn as light. Below the layer it glows from behind; above it set to "
                        "Add, it lights the layer too";
        d.params = {
            choice("mode", "Style", "tendrils", {{"tendrils", "Tendrils"}, {"rising", "Aura (オーラ)"}},
                   "Tendrils radiating outward, or an aura of tongues hugging the silhouette"),
            when(px("length", "Tendril Length", 120, 10, 600, "How far the tendrils reach from the edge"), "mode", {"tendrils"}),
            when(percent("wispiness", "Wispiness", 60, "How much the tendrils curl toward their tips, percent", 0, 200), "mode", {"tendrils"}),
            when(percent("density", "Tendril Density", 100, "How many tendrils run along the edge, percent", 20, 400), "mode", {"tendrils"}),
            when(choice("strength", "Strength", "strong", {{"strong", "Strong (強)"}, {"weak", "Weak (弱)"}},
                        "Strong tongues, or weak ones: shorter, dimmer and more broken"),
                 "mode", {"rising"}),
            when(choice("flow", "Flow", "outward", {{"outward", "Outward"}, {"up", "Up"}},
                        "The tongues reach outward along the edge, or rise with the sides licking up too"),
                 "mode", {"rising"}),
            when(px("tongue", "Tongue Length", 90, 5, 600, "How far the tongues reach from the silhouette"), "mode", {"rising"}),
            when(px("turbulence_scale", "Turbulence Scale", 60, 5, 400, "The size of the stretches of edge that share a tongue"), "mode", {"rising"}),
            when(percent("breakup", "Breakup", 50, "How much the outer half of the tongues breaks into holes, percent"), "mode", {"rising"}),
            multiplies(percent("glow", "Edge Glow", 60, "A soft glow hugging the edge, percent", 0, 200)),
            multiplies(percent("intensity", "Intensity", 140, "The light's brightness, percent", 0, 400)),
            light(gradient("gradient", "Colors", gradientValue({{0.0, "#ffffff"}, {0.2, "#c6f4ff"}, {0.45, "#3ab4ff"}, {0.75, "#6a2cff"}, {1.0, "#d01cff"}}),
                           "The light's colours from the edge to the tips")),
            live(number("phase", "Evolution", 0, 0, 10, ParamUnit::None, "Moves the tendrils and tongues through their flicker (key it to animate)",
                        0.01)),
            seedParam("Reshuffles the tendrils and tongues"),
        };
        d.follow = FollowMode::Required;
        d.followReach = auraReach;
        d.addsLight = [](const ParamValues&) { return true; };
        d.costClass = CostClass::C3;
        d.fallback = FallbackClass::F2;
        d.live.kind = LiveKind::Kernel;
        d.live.params = {"phase"};
        d.live.program = (uint32_t)GenProgram::AuraTendrils | (uint32_t)TITLE_VARIANT_GENERATOR;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"aura", "corona", "オーラ", "オーラ強", "オーラ弱", "コロナ", "energy tendrils", "power aura"};
        d.inputs.content = true;
        d.inputs.sdf = true;
        d.estimate = auraCost;
        d.presetFile = "editor/resources/titleart/fx-presets/aura.yaml";
        d.render = renderAura;
        return d;
    }

    // ---- Lightning ----

    bool lightningAlongEdge(const ParamValues& p) {
        return p.string("mode") == "edge";
    }

    float lightningReach(const ParamValues& p) {
        if (lightningAlongEdge(p)) {
            // offset walk (<= 1.5 w steps) + jag (<= 0.45 w steps); forks up to 4 (0.5 + w) steps
            // long with their own jitter (<= 0.62 w of that); a step is 2 cells
            const double w = p.number("wrinkle") / 100.0;
            const double step = 2.0 * analysis::BOLT_EDGE_CELL;
            return (float)(step * 1.95 * w + 4.0 * step * (0.5 + w) * (1.0 + 0.62 * w) + 1.6 * p.number("glow_width") + p.number("core_width"));
        }
        // bolt length + midpoint jitter (<= 0.8 x jag of a chord; crawl chords reach ~2 x length)
        // + the wide glow
        return (float)(p.number("length") * (1.0 + 1.6 * (p.number("jaggedness") / 100.0)) + 1.6 * p.number("glow_size") + p.number("core_width"));
    }

    // The bolts' glows run at half resolution (§6.7.14) once a glow's sigma spans 2 target px:
    // the core box-filtered to half size (a 2 x 2 box) and the glow read back bilinearly (a tent
    // two px wide) blur it by this variance (target px²), which the half-size Gaussian leaves out,
    // so the glow keeps Pachislop's width. Narrower glows (small px_scale, the goldens) run at
    // full resolution, Pachislop's blur exactly
    constexpr double HALF_RES_VARIANCE = 0.25 + 4.0 / 6.0;

    bool halfResGlow(double sigma) {
        return sigma >= 2.0;
    }

    // the half-size context of the glows (the blur reads only the size and the frame)
    EffectContext halfContext(const EffectContext& e) {
        EffectContext h = e;
        h.width = std::max(1, (e.width + 1) / 2);
        h.height = std::max(1, (e.height + 1) / 2);
        h.pxScale = e.pxScale * 0.5f;
        if (!e.frame.empty()) {
            h.frame = {(int)std::floor(e.frame.x / 2.0), (int)std::floor(e.frame.y / 2.0), std::max(1, (e.frame.width + 1) / 2),
                       std::max(1, (e.frame.height + 1) / 2)};
        }
        return h;
    }

    // the core's Gaussian of radius ~3 sigma = radiusPx (lib.ts blur): from `half` (the core at
    // half size) when halfResGlow, else at full resolution
    fx::Scratch glowOf(EffectContext& e, EffectContext& h, const TargetRef& core, const TargetRef* half, double radiusPx) {
        const double sigma = std::max(0.0, radiusPx / 3.0);
        if (!halfResGlow(sigma) || !half) return fx::blur(e, core, (float)radiusPx);
        fx::Scratch out = fx::scratch(h);
        if (!out) return {};
        const float s = (float)(std::sqrt(sigma * sigma - HALF_RES_VARIANCE) / 2.0);
        if (!fx::gauss(h, *half, s, s, true, *out)) return {};
        return out;
    }

    // glowOf's work (the half-size copy of the core is counted once by the caller)
    BlurCost glowCost(double radiusPx) {
        const double sigma = std::max(0.0, radiusPx / 3.0);
        if (!halfResGlow(sigma)) return blurCost(radiusPx);
        const double s = std::sqrt(sigma * sigma - HALF_RES_VARIANCE) / 2.0;
        BlurCost c;
        c.passes = 0;
        c.fre = 0.0;
        // fx::gauss halves further while its sigma passes 6 level px, then two 1-D passes and a copy
        int level = 0;
        double area = 0.25;
        while (s / std::ldexp(1.0, level) > 6.0 && level < 10) {
            level++;
            area *= 0.25;
            c.fre += area;
            c.passes++;
        }
        c.taps = std::min(65, 2 * (int)std::ceil(3.0 * s / std::ldexp(1.0, level)) + 1);
        c.fre += 2.0 * area * passFre(c.taps) + 0.25;
        c.passes += 3;
        return c;
    }

    analysis::BoltParams boltParams(const GeneratorContext& ctx, const ParamValues& p) {
        static const std::initializer_list<const char*> modes = {"mixed", "outward", "crawl", "edge"};
        analysis::BoltParams b;
        b.mode = (analysis::BoltMode)optionIndex(p, "mode", modes);
        b.count = p.integer("count");
        b.arcs = p.integer("arcs");
        b.length = p.number("length") * ctx.pxScale;
        b.jaggedness = p.number("jaggedness") / 100.0;
        b.branches = p.number("branches") / 100.0;
        b.wrinkle = p.number("wrinkle") / 100.0;
        b.branchiness = p.number("branchiness") / 100.0;
        b.seed = seedOf(ctx, p);
        b.cell = analysis::BOLT_EDGE_CELL * ctx.pxScale;
        return b;
    }

    void renderLightning(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        if (!hasFollowed(ctx, target)) return;
        drawFollowed(ctx, target, [&](TargetRef& out) {
            // the edge cells of the content grid, read back once per content (cached by its key)
            const analysis::GridSpec spec = analysis::frameGridSpec(ctx, analysis::BOLT_EDGE_CELL);
            const std::shared_ptr<const analysis::Grid> grid = analysis::readGrid(ctx, *ctx.followContent, ctx.followContentKey, spec);
            if (!grid) {
                ctx.runner.clear(out);
                return;
            }
            // the layout runs in Pachislop's render px of the whole frame (origin at its
            // bottom-left, +y up): cell (0, 0)'s corner is its readGrid px0 / py0 on a whole frame
            const TitleRectI whole = ctx.frame.empty() ? TitleRectI{0, 0, ctx.width, ctx.height} : ctx.frame;
            const double px0 = whole.width / 2.0 - (-grid->x0 / grid->cellComp) * grid->cell;
            const double py0 = whole.height / 2.0 - (-grid->y0 / grid->cellComp) * grid->cell;
            const std::vector<analysis::BoltEdge> edges = analysis::findBoltEdges(grid->alpha, grid->w, grid->h, px0, py0, grid->cell);
            if (edges.empty()) {
                ctx.runner.clear(out);
                return;
            }
            const std::vector<analysis::BoltSegment> segs = analysis::buildBolts(edges, boltParams(ctx, p));

            // the cores: one quad per segment, MAX blended so joints and crossings never double (R21)
            const double coreWidth = p.number("core_width") * ctx.pxScale * 0.5;
            QuadBatch batch;
            for (const analysis::BoltSegment& s : segs) {
                analysis::BoltQuad q;
                if (!analysis::boltQuad(s, coreWidth, q)) continue;
                QuadInstance quad;
                quad.cx = (float)q.cx;
                quad.cy = (float)q.cy;
                quad.w = (float)q.length;
                quad.h = (float)(2.0 * q.halfWidth);
                quad.rotation = (float)q.rotation;
                for (float& c : quad.color) c = (float)s.intensity;
                batch.add(quad);
            }
            fx::Scratch core = fx::scratchOf(ctx.runner, TitleBuffer::Accumulator, ctx.width, ctx.height);
            if (!core || !ctx.runner.clear(*core)) return;
            // whole-frame render px -> target px (rows down): the frame's bottom-left at (whole.x, whole.y + height)
            TitleAffine frameToTarget;
            frameToTarget.d = -1.0;
            frameToTarget.tx = whole.x;
            frameToTarget.ty = whole.y + whole.height;
            // full-size quads: the half width never falls under 0.7 x 1.6 px, so no min_px growth
            if (!batch.empty()) batch.draw(ctx, GenProgram::QuadsLightning, "lightning cores", *core, frameToTarget, QuadBlend::Max, 0.0f);

            // the bolts' own glow: a tight and a wide Gaussian of the core
            EffectContext e = effectContext(ctx);
            EffectContext h = halfContext(e);
            const double gs = p.number(lightningAlongEdge(p) ? "glow_width" : "glow_size") * ctx.pxScale;
            const double r1 = gs * 0.35, r2 = gs * 1.6;
            fx::Scratch half;
            if (halfResGlow(r1 / 3.0) || halfResGlow(r2 / 3.0)) {
                half = fx::scratch(h);
                if (!half || !fx::copy(h, *core, *half)) return;
            }
            fx::Scratch g1 = glowOf(e, h, *core, half.get(), r1);
            fx::Scratch g2 = glowOf(e, h, *core, half.get(), r2);
            if (!g1 || !g2) return;
            const TitleColor col = fx::linearOf(p.color("color"));
            TitlePass pass = genPass(ctx, GenProgram::LightningCompose, "lightning", out, ctx.documentToTarget);
            pass.textures[0].target = core.get();
            pass.textures[1].target = g1.get();
            pass.textures[2].target = g2.get();
            pass.set(6, col.r, col.g, col.b, (float)(p.number("intensity") / 100.0));
            pass.set(7, (float)(p.number("glow") / 100.0), ctx.lightScale);
            ctx.runner.run(pass);
        });
    }

    FxCost lightningCost(const ParamValues& p, const FxSize& size, FxTier) {
        // the cores (one quad a segment: a bolt's 64 plus ~150 x branching of branches; an arc ~4
        // a step plus its forks), the half-size core, the two glows, the compose (3 taps); the
        // grid readback once per content and the CPU layout
        const bool edge = lightningAlongEdge(p);
        int instances = 0;
        if (edge) {
            const double steps = 0.75 * p.number("length") / (2.0 * analysis::BOLT_EDGE_CELL);
            instances = (int)(p.integer("arcs") * steps * (4.0 + 0.3 * (p.number("branchiness") / 100.0) * 16.0));
        } else {
            instances = (int)(p.integer("count") * (64.0 + 154.0 * (p.number("branches") / 100.0)));
        }
        const double gs = p.number(edge ? "glow_width" : "glow_size") * size.pxScale;
        const BlurCost g1 = glowCost(gs * 0.35), g2 = glowCost(gs * 1.6);
        const bool half = halfResGlow(gs * 0.35 / 3.0) || halfResGlow(gs * 1.6 / 3.0);
        FxCost c = passCost(2 + (half ? 1 : 0) + g1.passes + g2.passes, 0.1 + (half ? 0.25 : 0.0) + g1.fre + g2.fre + passFre(3),
                            std::max({3, g1.taps, g2.taps}), instances, 0.5 + 0.0005 * instances);
        c.analysis = true;
        c.readback = true;
        return c;
    }

    GeneratorDef lightning() {
        GeneratorDef d;
        d.type = "lightning";
        d.label = "Lightning";
        d.category = "Energy";
        d.description = "Electric arcs crawling over and shooting out from the edges of the layer it follows, with branching bolts and their "
                        "own glow; Along Edge (Electrify) runs many arcs hugging the whole outline. Set the layer to Add";
        const std::vector<const char*> bolts = {"mixed", "outward", "crawl"};
        d.params = {
            relayout(choice("mode", "Arcs", "mixed",
                            {{"mixed", "Mixed"}, {"outward", "Shoot Outward"}, {"crawl", "Crawl along Edge"}, {"edge", "Along Edge (Electrify)"}},
                            "Bolts shooting out, crawling along the edge, both, or arcs hugging the whole outline")),
            when(relayout(integer("count", "Bolt Count", 12, 1, 40, "How many bolts start from the edge")), "mode", bolts),
            when(relayout(integer("arcs", "Arc Count", 24, 1, 60, "How many arcs walk the outline")), "mode", {"edge"}),
            relayout(px("length", "Bolt Length", 240, 10, 800, "How long each bolt or arc runs")),
            when(relayout(percent("jaggedness", "Jaggedness", 55, "How far the bolts zigzag off their line, percent", 0, 150)), "mode", bolts),
            when(relayout(percent("branches", "Branching", 60, "How often the bolts fork, percent")), "mode", bolts),
            when(relayout(percent("wrinkle", "Wrinkle", 60, "How much the arcs wander off the edge and zigzag, percent", 0, 150)), "mode", {"edge"}),
            when(relayout(percent("branchiness", "Branchiness", 40, "How often the arcs fork outward, percent")), "mode", {"edge"}),
            px("core_width", "Core Width", 3.5, 0.5, 12, "The width of the bright core", 0.1),
            when(px("glow_size", "Glow Size", 18, 0, 80, "How far the bolts' glow spreads"), "mode", bolts),
            when(px("glow_width", "Glow Width", 16, 0, 80, "How far the arcs' glow spreads"), "mode", {"edge"}),
            multiplies(live(percent("glow", "Glow Strength", 100, "The glow's brightness, percent", 0, 300))),
            light(color("color", "Glow Color", "#4fd8ff", "The colour of the glow; the core is whitened toward it")),
            multiplies(live(percent("intensity", "Intensity", 100, "The bolts' brightness, percent", 0, 300))),
            live(seedParam("Reshuffles the bolts (a stepped key strobes them)")),
        };
        d.follow = FollowMode::Required;
        d.followReach = lightningReach;
        d.addsLight = [](const ParamValues&) { return true; };
        d.encodesLight = true;
        d.costClass = CostClass::C2;
        d.fallback = FallbackClass::F1;
        d.live.kind = LiveKind::Kernel;
        d.live.params = {"glow", "intensity", "seed"};
        d.live.program = (uint32_t)GenProgram::LightningCompose | (uint32_t)TITLE_VARIANT_GENERATOR;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"lightning", "electric arcs", "electrify", "bolts", "稲妻", "雷", "電撃", "スパーク"};
        d.inputs.content = true;
        d.inputs.analysis = AnalysisSource::Content;
        d.estimate = lightningCost;
        d.presetFile = "editor/resources/titleart/fx-presets/lightning.yaml";
        d.render = renderLightning;
        return d;
    }

}

void titleart::registerEnergyGenerators(TitleEffectRegistry& registry) {
    registry.add(fire());
    registry.add(smoke());
    registry.add(aura());
    registry.add(lightning());
}
