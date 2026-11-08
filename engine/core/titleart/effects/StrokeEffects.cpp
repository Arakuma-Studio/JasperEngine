// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Outline (docs/specs/S80-title-art-core.md §6.5), ported from Pachislop src/effects/a/finish.ts
// (keyline) by SafEight, used with the author's permission.

#include "TitleFx.h"
#include "TitleFxParams.h"
#include "TitleFxFamilies.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fxparams;

namespace {

    void applyOutline(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const TargetRef& content = ctx.content ? *ctx.content : input;
        const TargetRef& ink = ctx.ink ? *ctx.ink : content;
        const TargetRef& inputInk = ctx.inputInk ? *ctx.inputInk : input;
        // light in the stack (§6.4): the line traces the ink and draws under that light
        const bool lit = &inputInk != &input;
        const bool followLayer = p.string("follow") == "layer";
        const float width = (float)std::max(0.25, p.number("width") * ctx.pxScale);
        // the line reads the edge distance out to its width (+ 1 px antialiasing)
        fx::Scratch field = fx::distanceField(ctx, followLayer ? ink : inputInk, (float)p.number("threshold"), width + 2.0f);
        if (!field) return;
        TitlePass pass = fx::makePass(ctx, TitleProgram::Outline, "outline", output);
        pass.textures[0].target = &input;
        pass.textures[1].target = lit ? &ink : &content;
        pass.textures[2].target = lit ? &inputInk : nullptr;
        pass.raw[0].target = field.get();
        const TitleColor c = fx::linearOf(p.color("color"));
        pass.set(2, c.r, c.g, c.b, width);
        pass.set(3, (float)p.number("softness"), (float)p.number("opacity"), followLayer ? 1.0f : 0.0f, lit ? 1.0f : 0.0f);
        ctx.runner.run(pass);
    }

    EffectDef outline() {
        EffectDef d;
        d.type = "outline";
        d.label = "Outline";
        d.category = "Stroke & Edge";
        d.description = "A line traced from the layer's alpha just outside its silhouette, behind it: the thin dark keyline that separates "
                        "a title's edge from the background, or a stroke; several following the stack make stacked strokes. Light is never "
                        "traced and stays over the line";
        d.params = {
            px("width", "Width", 1.5, 0.5, 100, "How wide the line is", 0.25),
            color("color", "Color", "#0a0604", "The line's colour"),
            number("opacity", "Opacity", 1, 0, 1, ParamUnit::None, "The line's opacity"),
            number("softness", "Softness", 0, 0, 1, ParamUnit::None, "How much of the line's outer part fades"),
            number("threshold", "Threshold", 0.5, 0.05, 0.95, ParamUnit::None, "The alpha level traced as the edge"),
            choice("follow", "Follow", "layer", {{"layer", "Layer"}, {"stack", "Stack Below"}},
                   "Trace the layer's own silhouette, or everything the effects above drew (light left out either way)"),
        };
        d.gizmos = {{GizmoKind::Radius, "width", nullptr}};
        d.reach = [](const ParamValues& p) { return (float)p.number("width"); };
        d.gather = [](const ParamValues& p) { return (float)p.number("width") + 1.0f; };
        d.outline = true;
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F1;
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"keyline", "stroke", "border", "縁取り", "フチ"};
        d.inputs.content = true;
        d.inputs.sdf = true;
        // S83 §6.12: the line's split scales per frame
        markLive(d, {"opacity"}, LiveKind::ScaleSplit, (uint32_t)TitleProgram::Outline);
        d.apply = applyOutline;
        return d;
    }

}

void titleart::registerStrokeEffects(TitleEffectRegistry& registry) {
    registry.add(outline());
}
