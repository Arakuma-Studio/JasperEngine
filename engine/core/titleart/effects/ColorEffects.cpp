// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Color Overlay, Gradient Overlay and Adjust (docs/specs/S80-title-art-core.md §6.5). Gradient
// Overlay and Adjust are ported from Pachislop src/effects/a/stylize.ts (gradientOverlay) and
// src/effects/a/color.ts (adjust) by SafEight, used with the author's permission; Color Overlay
// is new (Photoshop's, blending sRGB-encoded values as Gradient Overlay does).

#include "TitleAdjust.h"
#include "TitleFx.h"
#include "TitleFxParams.h"
#include "TitleFxFamilies.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fxparams;

namespace {

    // a blend mode's TitleBlend value (0 for an unknown name)
    int blendIndex(const std::string& name) {
        TitleBlend blend = TitleBlend::Normal;
        return titleBlendFromName(name, blend) ? (int)blend : 0;
    }

    void applyColorOverlay(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        TitlePass pass = fx::makePass(ctx, TitleProgram::ColorOverlay, "color_overlay", output);
        pass.textures[0].target = &input;
        const TitleColor c = p.color("color");
        pass.set(2, c.r, c.g, c.b, (float)p.number("opacity"));
        pass.set(3, (float)blendIndex(p.string("mode")));
        ctx.runner.run(pass);
    }

    EffectDef colorOverlay() {
        EffectDef d;
        d.type = "color_overlay";
        d.label = "Color Overlay";
        d.category = "Color";
        d.description = "Fills the layer with one colour in a blend mode, keeping its alpha (Photoshop's Color Overlay)";
        d.params = {
            color("color", "Color", "#ff0000", "The overlay colour"),
            number("opacity", "Opacity", 1, 0, 1, ParamUnit::None, "How much of the colour covers the layer"),
            choice("mode", "Blend Mode", "normal", blendOptions(), "How the colour combines with the layer's own"),
        };
        d.gather = [](const ParamValues&) { return 0.0f; };
        d.costClass = CostClass::C0;
        d.fallback = FallbackClass::F0;
        d.glyphSafety = GlyphSafety::Safe;
        d.aliases = {"colour overlay", "tint", "fill color", "カラーオーバーレイ"};
        // S83 §6.12: a kernel over the cached input split
        markLive(d, {"opacity", "color"}, LiveKind::Kernel, (uint32_t)TitleProgram::ColorOverlay);
        d.apply = applyColorOverlay;
        return d;
    }

    // the overlay's modes: the 18 blend modes, then Light (add) and Shade (multiply) in linear light
    std::vector<EnumOption> overlayModes() {
        std::vector<EnumOption> modes = blendOptions();
        modes.push_back({"light", "Light (Add)"});
        modes.push_back({"shade", "Shade (Multiply)"});
        return modes;
    }

    int overlayModeIndex(const std::string& name) {
        if (name == "light") return TITLE_BLEND_COUNT;
        if (name == "shade") return TITLE_BLEND_COUNT + 1;
        return blendIndex(name);
    }

    void applyGradientOverlay(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        fx::Scratch box = fx::layerBounds(ctx, input);
        if (!box) return;
        const TitleLut* ramp = ctx.runner.lut(fx::gradientLut(p.gradient("gradient")));
        if (!ramp) return;
        TitlePass pass = fx::makePass(ctx, TitleProgram::GradientOverlay, "gradient_overlay", output);
        pass.textures[0].target = &input;
        pass.textures[1].lut = ramp;
        pass.raw[1].target = box.get();
        const TitleVec2 dir = fx::dirClockwise(p.number("angle"));
        pass.set(2, (float)dir.x, (float)dir.y, (float)p.number("scale"), (float)p.number("offset"));
        pass.set(3, (float)p.number("amount"), (float)std::max(1.0, p.number("length") * ctx.pxScale), (float)(p.number("position") / 100.0),
                 (float)overlayModeIndex(p.string("mode")));
        pass.set(4, (float)optionIndex(p, "wrap", {"clamp", "repeat", "mirror"}), (float)optionIndex(p, "style", {"linear", "radial", "sweep"}));
        ctx.runner.run(pass);
    }

    EffectDef gradientOverlay() {
        EffectDef d;
        d.type = "gradient_overlay";
        d.label = "Gradient Overlay";
        d.category = "Color";
        d.description = "Recolours the layer with a gradient, keeping its alpha: Linear across its bounds (90 = top to bottom), Radial from "
                        "its centre, or Sweep, a band of fixed length moved across by Position (a glint); Light and Shade add or filter in "
                        "linear light";
        const std::vector<const char*> along = {"linear", "sweep"};
        const std::vector<const char*> fit = {"linear", "radial"};
        const std::vector<const char*> sweep = {"sweep"};
        d.params = {
            gradient("gradient", "Gradient", gradientValue({{0.0, "#e8a020"}, {0.2, "#fff6b0"}, {0.4, "#f0a820"}, {0.6, "#ffc840"},
                                                             {0.8, "#c05a08"}, {1.0, "#ffe070"}}),
                     "The colours laid over the layer"),
            choice("style", "Style", "linear", {{"linear", "Linear"}, {"radial", "Radial"}, {"sweep", "Sweep (Band)"}},
                   "Across the layer's box, out from its centre, or a band moved across it"),
            when(degrees("angle", "Angle", 90, "The gradient's direction, clockwise (90 = top to bottom)"), "style", along),
            when(number("scale", "Scale", 1, 0.1, 10, ParamUnit::Factor, "How much of the box the gradient spans"), "style", fit),
            when(number("offset", "Offset", 0, -2, 2, ParamUnit::None, "Moves the gradient along its direction"), "style", fit),
            when(px("length", "Length", 200, 2, 4000, "The band's length"), "style", sweep),
            when(number("position", "Position", 0, -100, 100, ParamUnit::None,
                        "Where the band is, percent of the way across: -100 just off one side, 100 just off the other", 1.0),
                 "style", sweep),
            choice("wrap", "Wrap", "clamp", {{"clamp", "Clamp"}, {"repeat", "Repeat"}, {"mirror", "Mirror"}},
                   "What the gradient does past its ends"),
            number("amount", "Amount", 1, 0, 1, ParamUnit::None, "How much of the gradient covers the layer"),
            choice("mode", "Blend Mode", "normal", overlayModes(), "How the gradient combines with the layer's colour"),
        };
        d.gizmos = {{GizmoKind::Direction, "angle", nullptr}};
        // the box of the whole layer
        d.gather = [](const ParamValues&) { return -1.0f; };
        d.costClass = CostClass::C0;
        d.fallback = FallbackClass::F0;
        // it spans the layer's box, which a glyph cell does not have
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"gradientOverlay", "gradient", "shine", "グラデーションオーバーレイ"};
        // S83 §6.12: the band or gradient from the lookup over the cached input split (the sweep, Color Cycle)
        markLive(d, {"position", "amount", "angle", "offset", "scale"}, LiveKind::Kernel, (uint32_t)TitleProgram::GradientOverlay);
        d.apply = applyGradientOverlay;
        return d;
    }

    void applyAdjust(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const Json& adjustment = p.raw("adjustment");
        if (titleAdjustmentIsIdentity(adjustment)) fx::copy(ctx, input, output);
        else applyTitleAdjustment(ctx, input, adjustment, output);
    }

    EffectDef adjust() {
        EffectDef d;
        d.type = "adjust";
        d.label = "Adjust";
        d.category = "Color";
        d.description = "One adjustment (Levels, Curves, Hue/Saturation, Gradient Map...) on just this layer, group or part: the same "
                        "operation as an adjustment layer";
        ParamDef a;
        a.key = "adjustment";
        a.label = "Adjustment";
        a.kind = ParamKind::Adjustment;
        a.defaultValue = Json{{"type", "hue_saturation"}, {"hue", 0}, {"saturation", 25}, {"lightness", 0}, {"colorize", false}};
        a.description = "The adjustment and its settings (the fourteen types of an adjustment layer)";
        a.anim = FxAnim::Rerender;
        d.params = {a};
        d.gather = [](const ParamValues&) { return 0.0f; };
        d.costClass = CostClass::C0;
        d.fallback = FallbackClass::F0;
        d.glyphSafety = GlyphSafety::Safe;
        d.aliases = {"adjustment", "levels", "curves", "hue saturation", "color correction", "色調補正"};
        d.apply = applyAdjust;
        return d;
    }

}

void titleart::registerColorEffects(TitleEffectRegistry& registry) {
    registry.add(colorOverlay());
    registry.add(gradientOverlay());
    registry.add(adjust());
}
