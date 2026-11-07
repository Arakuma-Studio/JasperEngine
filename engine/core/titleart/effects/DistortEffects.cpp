// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Wave Warp, Motion Trail, Chromatic Aberration and Glitch (docs/specs/S83-title-effects-generators.md
// §6.6.8, §6.6.12, §6.6.13, §6.6.15), ported from Pachislop src/effects/a/distort.ts (wave,
// chromaticAberration), src/effects/a/stylize.ts (motionTrail) and src/effects/a/finish.ts (glitch)
// by SafEight, used with the author's permission. Wave Warp's turbulent type and Glitch's blocks
// mode are S83's.

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

    // ---- Wave Warp ----

    void applyWaveWarp(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const int type = optionIndex(p, "type", {"linear", "radial", "turbulent"});
        const TitleVec2 dir = fx::dirClockwise(p.number("angle"));
        const TitleVec2 centre = tfx::canvasCentre(ctx);
        const TitleVec2 ripple = tfx::canvasPoint(ctx, p.vec2("center"));
        // the wave is laid on the canvas, so the layer's place on it matters
        ctx.usesCanvasPosition = true;
        TitlePass pass = tfx::makePass(ctx, tfx::Program::WaveWarp, "wave_warp", output);
        pass.textures[0].target = &input;
        pass.set(2, (float)dir.x, (float)dir.y, (float)p.number("amplitude") * ctx.pxScale, (float)std::max(1.0, p.number("wavelength")));
        pass.set(3, (float)p.number("phase"), (float)type, (float)std::max(1, p.integer("complexity")), (float)p.number("evolution"));
        pass.set(4, (float)ripple.x, (float)ripple.y, (float)centre.x, (float)centre.y);
        ctx.runner.run(pass);
    }

    FxCost estimateWaveWarp(const ParamValues& p, const FxSize&, FxTier) {
        tfx::CostBuilder b;
        // the turbulent field evaluates its octaves twice: ALU, counted as taps
        return b.pass(1.0, p.string("type") == "turbulent" ? 4 + 2 * std::max(1, p.integer("complexity")) : 1).cost;
    }

    EffectDef waveWarp() {
        using namespace tfx;
        EffectDef d;
        d.type = "wave_warp";
        d.label = "Wave Warp";
        d.category = "Distort";
        d.description = "A sine displacement: a linear wave like a flag, circular ripples from a centre, or a turbulent heat-haze field";
        d.params = {
            choice("type", "Type", "linear", {{"linear", "Linear"}, {"radial", "Radial Ripple"}, {"turbulent", "Turbulent"}}, "The wave's shape"),
            live(px("amplitude", "Amplitude", 8, 0, 200, "How far pixels move", 0.5)),
            px("wavelength", "Wavelength", 140, 4, 3000, "The distance between two crests"),
            live(number("phase", "Phase", 0, 0, 1, ParamUnit::None, "Moves the wave along by a fraction of a wavelength (wraps)")),
            when(degrees("angle", "Direction", 0, "The direction the wave travels, clockwise"), "type", {"linear"}),
            when(vec2("center", "Center", 0, 0, "The ripples' centre, canvas px from the canvas centre"), "type", {"radial"}),
            when(integer("complexity", "Complexity", 3, 1, 6, "Octaves of the turbulent field"), "type", {"turbulent"}),
            when(live(number("evolution", "Evolution", 0, 0, 100, ParamUnit::None, "Drifts the turbulent field")), "type", {"turbulent"}),
        };
        d.gizmos = {{GizmoKind::Direction, "angle", nullptr}, {GizmoKind::Point, "center", nullptr}};
        d.reach = [](const ParamValues& p) { return (float)p.number("amplitude"); };
        d.gather = [](const ParamValues& p) { return (float)p.number("amplitude"); };
        d.costClass = CostClass::C0;
        d.fallback = FallbackClass::F0;
        d.live = {LiveKind::Kernel, {"amplitude", "phase", "evolution"}, (uint32_t)Program::WaveWarp};
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"wave", "ripple", "flag", "heat haze", "warp", "波", "揺らぎ"};
        d.estimate = estimateWaveWarp;
        d.presetFile = "editor/resources/titleart/fx-presets/wave_warp.yaml";
        d.apply = applyWaveWarp;
        return d;
    }

    // ---- Motion Trail ----

    void applyMotionTrail(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        fx::Scratch box = fx::layerBounds(ctx, input);
        if (!box) return;
        const TitleVec2 dir = fx::dirClockwise(p.number("angle"));
        if (p.string("mode") == "echo") {
            const int n = std::min(12, std::max(1, p.integer("copies")));
            // the farthest copy's blur radius, render px
            const float r = (float)std::max(0.0, p.number("echo_blur")) * n * ctx.pxScale;
            const bool blurred = r > 0.5f;
            fx::Scratch b1, b2, b3;
            if (blurred) {
                b1 = fx::blur(ctx, input, r / 3.0f);
                b2 = fx::blur(ctx, input, 2.0f * r / 3.0f);
                b3 = fx::blur(ctx, input, r);
                if (!b1 || !b2 || !b3) return;
            }
            TitlePass pass = tfx::makePass(ctx, tfx::Program::MotionEcho, "motion_trail echo", output);
            pass.textures[0].target = &input;
            pass.textures[1].target = blurred ? b1.get() : &input;
            pass.textures[2].target = blurred ? b2.get() : &input;
            pass.textures[3].target = blurred ? b3.get() : &input;
            pass.raw[1].target = box.get();
            const TitleColor c0 = fx::linearOf(p.color("tint_start")), c1 = fx::linearOf(p.color("tint_end"));
            pass.set(2, (float)dir.x, (float)dir.y, (float)std::max(0.0, p.number("offset")) * ctx.pxScale,
                     (float)std::max(0.05, 1.0 + p.number("scale_step") / 100.0));
            pass.set(3, c0.r, c0.g, c0.b, (float)std::min(1.0, std::max(0.0, p.number("decay"))));
            pass.set(4, c1.r, c1.g, c1.b, (float)p.number("tint_amount"));
            pass.set(5, (float)n, blurred ? 1.0f : 0.0f, (float)p.number("opacity"));
            ctx.runner.run(pass);
            return;
        }
        const float len = std::max(1.0f, (float)p.number("length") * ctx.pxScale);
        TitlePass pass = tfx::makePass(ctx, tfx::Program::MotionSmear, "motion_trail smear", output);
        pass.textures[0].target = &input;
        pass.raw[1].target = box.get();
        const TitleColor t = fx::linearOf(p.color("tint"));
        pass.set(2, (float)dir.x, (float)dir.y, len, std::max(0.75f, len / 1024.0f));
        pass.set(3, t.r, t.g, t.b, (float)p.number("tint_amount"));
        pass.set(4, (float)std::max(0.0, p.number("fade")), (float)p.number("taper"), (float)p.number("opacity"));
        ctx.runner.run(pass);
    }

    FxCost estimateMotionTrail(const ParamValues& p, const FxSize& size, FxTier) {
        tfx::CostBuilder b;
        b.box();
        if (p.string("mode") == "echo") {
            const int n = std::min(12, std::max(1, p.integer("copies")));
            const double r = std::max(0.0, p.number("echo_blur")) * n * size.pxScale;
            if (r > 0.5) b.blur(r / 3.0).blur(2.0 * r / 3.0).blur(r);
            return b.pass(1.0, n * (r > 0.5 ? 4 : 1)).cost;
        }
        const float len = std::max(1.0f, (float)p.number("length") * size.pxScale);
        return b.pass(1.0, (int)std::ceil(len / std::max(0.75f, len / 1024.0f))).cost;
    }

    EffectDef motionTrail() {
        using namespace tfx;
        EffectDef d;
        d.type = "motion_trail";
        d.label = "Motion Trail";
        d.category = "Distort";
        d.description = "A smeared trail of the shape behind the layer, fading and tapering toward its end, or Echo (残像): separate fading "
                        "copies stepped along the direction";
        d.params = {
            choice("mode", "Mode", "smear", {{"smear", "Smear"}, {"echo", "Echo (残像)"}}, "One continuous smear, or separate copies"),
            degrees("angle", "Angle", 180, "The trail's direction, clockwise (180 = to the left)"),
            when(px("length", "Length", 160, 1, 1500, "How long the smear is"), "mode", {"smear"}),
            when(number("fade", "Fade", 1.4, 0, 6, ParamUnit::None, "How quickly the smear fades"), "mode", {"smear"}),
            when(number("taper", "Taper", 0.25, 0, 0.95, ParamUnit::None, "How much the smear narrows toward its end"), "mode", {"smear"}),
            when(integer("copies", "Copies", 4, 1, 12, "How many echoes"), "mode", {"echo"}),
            when(live(px("offset", "Copy Offset", 70, 1, 600, "The distance between two echoes")), "mode", {"echo"}),
            when(number("scale_step", "Scale Step", 0, -30, 30, ParamUnit::None, "Each echo grows (+) or shrinks (-) by this percent", 1), "mode", {"echo"}),
            when(number("decay", "Opacity Decay", 0.4, 0, 0.95, ParamUnit::None, "How much each echo fades"), "mode", {"echo"}),
            when(px("echo_blur", "Blur Per Copy", 0, 0, 40, "Each echo blurs this much more", 0.5), "mode", {"echo"}),
            live(number("opacity", "Opacity", 0.8, 0, 1, ParamUnit::None, "The trail's opacity")),
            when(color("tint", "Tint", "#ff7a00", "The smear's tint"), "mode", {"smear"}),
            when(color("tint_start", "Start Color", "#ff7a00", "The first echo's tint"), "mode", {"echo"}),
            when(color("tint_end", "End Color", "#7a2cff", "The last echo's tint"), "mode", {"echo"}),
            number("tint_amount", "Tint Amount", 0.5, 0, 1, ParamUnit::None, "How much the tint replaces the layer's colours"),
        };
        d.gizmos = {{GizmoKind::Direction, "angle", "length"}};
        d.reach = [](const ParamValues& p) {
            return (float)(p.string("mode") == "echo" ? std::max(1, p.integer("copies")) * (p.number("offset") + p.number("echo_blur")) : p.number("length"));
        };
        d.gather = [](const ParamValues&) { return -1.0f; };
        d.costClass = CostClass::C3;
        d.fallback = FallbackClass::F0;
        d.live = {LiveKind::Kernel, {"offset", "opacity"}, (uint32_t)Program::MotionEcho};
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"motionTrail", "trail", "echo", "afterimage", "smear", "残像"};
        d.estimate = estimateMotionTrail;
        d.presetFile = "editor/resources/titleart/fx-presets/motion_trail.yaml";
        d.apply = applyMotionTrail;
        return d;
    }

    // ---- Chromatic Aberration ----

    int chromaticTaps(const ParamValues& p, float amountPx) {
        return p.flag("smooth") ? std::max(3, std::min(31, 2 * (int)std::ceil(amountPx) + 1)) : 3;
    }

    void applyChromatic(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float amount = (float)p.number("amount") * ctx.pxScale;
        const TitleVec2 dir = fx::dirClockwise(p.number("angle"));
        const TitleVec2 c = tfx::canvasPoint(ctx, p.vec2("center"));
        const bool radial = p.string("type") != "directional";
        if (radial) ctx.usesCanvasPosition = true;
        TitlePass pass = tfx::makePass(ctx, tfx::Program::Chromatic, "chromatic_aberration", output);
        pass.textures[0].target = &input;
        pass.set(2, (float)dir.x, (float)dir.y, amount, (float)chromaticTaps(p, amount));
        pass.set(3, (float)c.x, (float)c.y, radial ? 0.0f : 1.0f);
        ctx.runner.run(pass);
    }

    FxCost estimateChromatic(const ParamValues& p, const FxSize& size, FxTier) {
        tfx::CostBuilder b;
        return b.pass(1.0, chromaticTaps(p, (float)p.number("amount") * size.pxScale)).cost;
    }

    EffectDef chromaticAberration() {
        using namespace tfx;
        EffectDef d;
        d.type = "chromatic_aberration";
        d.label = "Chromatic Aberration";
        d.category = "Stylize";
        d.description = "An RGB split: red and blue pushed apart radially from a centre or along a direction, smeared through the spectrum";
        d.params = {
            live(px("amount", "Amount", 6, 0, 200, "How far the colours split", 0.5)),
            choice("type", "Type", "radial", {{"radial", "Radial"}, {"directional", "Directional"}}, "Split outward from a centre, or along one direction"),
            when(degrees("angle", "Direction", 0, "The split's direction, clockwise"), "type", {"directional"}),
            flag("smooth", "Spectral Smear", true, "Smears through the spectrum instead of three hard copies"),
            when(vec2("center", "Center", 0, 0, "The split's centre, canvas px from the canvas centre"), "type", {"radial"}),
        };
        d.gizmos = {{GizmoKind::Point, "center", nullptr}, {GizmoKind::Direction, "angle", nullptr}};
        d.reach = [](const ParamValues& p) { return (float)(p.number("amount") * (p.string("type") == "directional" ? 1.0 : 2.0)); };
        d.gather = [](const ParamValues& p) { return (float)(p.number("amount") * 2.0); };
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F0;
        d.live = {LiveKind::Kernel, {"amount"}, (uint32_t)Program::Chromatic};
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"chromaticAberration", "rgb split", "color fringe", "prism", "色収差"};
        d.estimate = estimateChromatic;
        d.presetFile = "editor/resources/titleart/fx-presets/chromatic_aberration.yaml";
        d.apply = applyChromatic;
        return d;
    }

    // ---- Glitch ----

    void applyGlitch(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        fx::Scratch box = fx::layerBounds(ctx, input);
        if (!box) return;
        TitlePass pass = tfx::makePass(ctx, tfx::Program::Glitch, "glitch", output);
        pass.textures[0].target = &input;
        pass.raw[1].target = box.get();
        pass.set(2, (float)p.number("amount"), (float)std::max(1, p.integer("slices")), (float)p.number("offset") * ctx.pxScale,
                 (float)p.number("split") * ctx.pxScale);
        // Pachislop's seed: (seed mod 997) x 1.37, with the document's seed added
        pass.set(3, (float)std::fmod(tfx::seedOf(ctx, p), 997.0f) * 1.37f, p.string("mode") == "blocks" ? 1.0f : 0.0f,
                 (float)p.number("block_size") * ctx.pxScale);
        ctx.runner.run(pass);
    }

    FxCost estimateGlitch(const ParamValues&, const FxSize&, FxTier) {
        tfx::CostBuilder b;
        return b.box().pass(1.0, 3).cost;
    }

    EffectDef glitch() {
        using namespace tfx;
        EffectDef d;
        d.type = "glitch";
        d.label = "Glitch";
        d.category = "Stylize";
        d.description = "A digital glitch: horizontal slices (or rectangular blocks) of the layer jump sideways, each with its own RGB split";
        d.params = {
            choice("mode", "Mode", "slices", {{"slices", "Slices"}, {"blocks", "Blocks"}}, "Whole horizontal slices, or rectangular blocks"),
            live(number("amount", "Amount", 0.35, 0, 1, ParamUnit::None, "The share of slices that jump")),
            when(integer("slices", "Slices", 14, 1, 80, "How many slices across the layer's height"), "mode", {"slices"}),
            px("offset", "Max Offset", 28, 0, 600, "How far a slice jumps at most"),
            px("split", "RGB Split", 7, 0, 100, "How far the colour channels split", 0.5),
            when(px("block_size", "Block Size", 40, 4, 400, "The blocks' width"), "mode", {"blocks"}),
            liveStepped(seed("seed", "Seed", "The glitch's random pattern; reseed every few frames to animate it")),
        };
        d.reach = [](const ParamValues& p) { return (float)(p.number("offset") + p.number("split")); };
        d.gather = [](const ParamValues&) { return -1.0f; };
        // C1, not §6.6.15's C0: the layer box is the GPU reduction (Deviations)
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F0;
        d.live = {LiveKind::Kernel, {"amount", "seed"}, (uint32_t)Program::Glitch};
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"digital", "vhs", "datamosh", "corrupt", "グリッチ", "逆押し"};
        d.estimate = estimateGlitch;
        d.presetFile = "editor/resources/titleart/fx-presets/glitch.yaml";
        d.apply = applyGlitch;
        return d;
    }

}

void titleart::registerDistortEffects(TitleEffectRegistry& registry) {
    registry.add(waveWarp());
    registry.add(motionTrail());
    registry.add(chromaticAberration());
    registry.add(glitch());
}
