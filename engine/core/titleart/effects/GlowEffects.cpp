// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Outer Glow and Inner Glow (docs/specs/S80-title-art-core.md §6.5), ported from Pachislop
// src/effects/a/glow.ts (outerGlow, innerGlow) by SafEight, used with the author's permission.

#include "TitleFx.h"
#include "TitleFxParams.h"
#include "TitleFxFamilies.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fxparams;

namespace {

    // Photoshop's Technique: Softer = the blurred alpha (a soft halo that rounds corners);
    // Precise = the exact distance from the edge, solid out to Spread / Choke of Size then a smooth
    // falloff to 0 at Size (it follows corners and serifs)
    ParamDef technique() {
        return choice("technique", "Technique", "softer", {{"softer", "Softer"}, {"precise", "Precise"}},
                      "Softer blurs the silhouette (round halo); Precise follows the exact distance from the edge");
    }

    // pink above, cyan below: the 虹 title halo
    Json glowRamp() {
        return gradientValue({{0.0, "#ff4ad8"}, {1.0, "#30e0ff"}});
    }

    // the Eye Candy gradient glow: white at the edge to red
    Json classicGlow() {
        return gradientValue({{0.0, "#ffffff"}, {0.22, "#fff36b"}, {0.55, "#ff8a00"}, {1.0, "#e01400"}});
    }

    void applyOuterGlow(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const int fill = optionIndex(p, "fill", {"solid", "gradient", "layer", "distance"});
        const bool precise = p.string("technique") == "precise";
        const float size = (float)std::max(0.5, p.number("size")) * ctx.pxScale;
        // Softer reads the blurs; From Layer reads the big blur's colour even when Precise
        fx::Scratch big, small, field, box;
        if (!precise || fill == 2) {
            // only From Layer reads the big blur's colour
            big = fill == 2 ? fx::blur(ctx, input, size) : fx::blurAlpha(ctx, input, size);
            if (!big) return;
        }
        if (!precise && fill != 2) {
            small = fx::blurAlpha(ctx, input, size * 0.3f);
            if (!small) return;
        }
        if (precise || fill == 3) {
            field = fx::distanceField(ctx, input, 0.5f, size + 2.0f);
            if (!field) return;
        }
        if (fill == 1) {
            box = fx::layerBounds(ctx, input);
            if (!box) return;
        }
        const TitleLut* ramp = nullptr;
        if (fill == 1 || fill == 3) {
            ramp = ctx.runner.lut(fx::gradientLut(p.gradient("gradient")));
            if (!ramp) return;
        }
        TitlePass pass = fx::makePass(ctx, TitleProgram::OuterGlow, "outer_glow", output);
        pass.textures[0].target = &input;
        pass.textures[1].target = big.get();
        pass.textures[2].target = small.get();
        pass.textures[3].lut = ramp;
        pass.raw[0].target = field.get();
        pass.raw[1].target = box.get();
        const TitleColor c = fx::linearOf(p.color("color"));
        const TitleVec2 dir = fx::dirClockwise(p.number("angle"));
        pass.set(2, c.r, c.g, c.b, (float)p.number("intensity"));
        pass.set(3, (float)p.number("spread"), (float)p.number("opacity"), (float)p.number("on_face"), size);
        pass.set(4, (float)fill, p.string("mode") == "add" ? 1.0f : 0.0f, precise ? 1.0f : 0.0f);
        pass.set(5, (float)dir.x, (float)dir.y);
        ctx.runner.run(pass);
    }

    EffectDef outerGlow() {
        EffectDef d;
        d.type = "outer_glow";
        d.label = "Outer Glow";
        d.category = "Glow";
        d.description = "A glow around the layer, behind it like Photoshop's or added over it as light; one colour, a gradient across the "
                        "layer, a gradient out from the edge, or the layer's own colours";
        d.params = {
            choice("fill", "Fill", "solid",
                   {{"solid", "Color"}, {"gradient", "Gradient Across"}, {"layer", "From Layer"}, {"distance", "Gradient From Edge"}},
                   "What colours the glow: one colour, a gradient across the layer, the layer's own colours or a gradient out from the edge"),
            when(color("color", "Color", "#ffb000", "The glow's colour", true), "fill", {"solid"}),
            when(gradient("gradient", "Gradient", glowRamp(), "The glow's colours across the layer or out from its edge", true), "fill",
                 {"gradient", "distance"}),
            when(degrees("angle", "Angle", 90, "Direction of the gradient across the layer, clockwise (90 = top to bottom)"), "fill",
                 {"gradient"}),
            technique(),
            px("size", "Size", 40, 1, 400, "How far the glow reaches past the edge"),
            number("intensity", "Intensity", 1.6, 0, 6, ParamUnit::Factor, "How bright the glow is near the edge"),
            number("spread", "Spread", 0.15, 0, 1, ParamUnit::None, "How much of the glow stays solid before it fades"),
            number("opacity", "Opacity", 1, 0, 1, ParamUnit::None, "The glow's opacity"),
            choice("mode", "Mode", "behind", {{"behind", "Behind"}, {"add", "Add (Light)"}},
                   "Behind the layer like Photoshop, or added over it as light"),
            when(number("on_face", "On Face", 0.15, 0, 1, ParamUnit::None, "How much added light falls over the layer itself"), "mode",
                 {"add"}),
        };
        d.gizmos = {{GizmoKind::Radius, "size", nullptr}, {GizmoKind::Direction, "angle", nullptr}};
        d.reach = [](const ParamValues& p) { return (float)p.number("size"); };
        // blurs of radius `size` (taps to ~1.33x) or the edge distance up to it; the gradient
        // across the layer spans the layer's box
        d.gather = [](const ParamValues& p) { return p.string("fill") == "gradient" ? -1.0f : 1.5f * (float)p.number("size"); };
        d.addsLight = [](const ParamValues& p) { return p.string("mode") == "add"; };
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F1;
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"glow", "halo", "outerGlow", "光彩 (外側)", "後光"};
        d.apply = applyOuterGlow;
        d.alphaFromAlpha = true;
        return d;
    }

    void applyInnerGlow(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const bool precise = p.string("technique") == "precise";
        const bool distanceFill = p.string("fill") == "distance";
        const float size = (float)std::max(0.5, p.number("size")) * ctx.pxScale;
        fx::Scratch blurred, field;
        if (!precise) {
            blurred = fx::blur(ctx, input, size);
            if (!blurred) return;
        }
        if (precise || distanceFill) {
            field = fx::distanceField(ctx, input, 0.5f, size + 2.0f);
            if (!field) return;
        }
        const TitleLut* ramp = nullptr;
        if (distanceFill) {
            ramp = ctx.runner.lut(fx::gradientLut(p.gradient("gradient")));
            if (!ramp) return;
        }
        TitlePass pass = fx::makePass(ctx, TitleProgram::InnerGlow, "inner_glow", output);
        pass.textures[0].target = &input;
        pass.textures[1].target = blurred.get();
        pass.textures[3].lut = ramp;
        pass.raw[0].target = field.get();
        const TitleColor c = fx::linearOf(p.color("color"));
        pass.set(2, c.r, c.g, c.b, (float)p.number("intensity"));
        pass.set(3, (float)p.number("opacity"), (float)p.number("choke"), size, (float)optionIndex(p, "mode", {"normal", "screen", "add"}));
        pass.set(4, p.string("source") == "center" ? 1.0f : 0.0f, precise ? 1.0f : 0.0f, distanceFill ? 1.0f : 0.0f);
        ctx.runner.run(pass);
    }

    EffectDef innerGlow() {
        EffectDef d;
        d.type = "inner_glow";
        d.label = "Inner Glow";
        d.category = "Glow";
        d.description = "A glow spreading inward from the layer's edges (or outward from its centre), in one colour or a gradient in from the edge";
        d.params = {
            choice("fill", "Fill", "solid", {{"solid", "Color"}, {"distance", "Gradient From Edge"}},
                   "One colour, or a gradient that runs in from the edge"),
            when(color("color", "Color", "#fff4a0", "The glow's colour", true), "fill", {"solid"}),
            when(gradient("gradient", "Gradient", classicGlow(), "The glow's colours from the edge inward", true), "fill", {"distance"}),
            technique(),
            px("size", "Size", 18, 1, 200, "How far the glow reaches into the layer"),
            number("intensity", "Intensity", 1, 0, 4, ParamUnit::Factor, "How bright the glow is"),
            number("choke", "Choke", 0, 0, 1, ParamUnit::None, "How much of the glow stays solid before it fades"),
            number("opacity", "Opacity", 1, 0, 1, ParamUnit::None, "The glow's opacity"),
            choice("mode", "Mode", "screen", {{"normal", "Normal"}, {"screen", "Screen"}, {"add", "Add (Light)"}},
                   "How the glow combines with the layer's colour"),
            choice("source", "Source", "edge", {{"edge", "Edge"}, {"center", "Center"}}, "Glow in from the edges, or out from the centre"),
        };
        d.gizmos = {{GizmoKind::Radius, "size", nullptr}};
        d.gather = [](const ParamValues& p) { return 1.5f * (float)p.number("size"); };
        d.addsLight = [](const ParamValues& p) { return p.string("mode") == "add"; };
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F1;
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"innerGlow", "inner light", "光彩 (内側)"};
        d.apply = applyInnerGlow;
        return d;
    }

}

void titleart::registerGlowEffects(TitleEffectRegistry& registry) {
    registry.add(outerGlow());
    registry.add(innerGlow());
}
