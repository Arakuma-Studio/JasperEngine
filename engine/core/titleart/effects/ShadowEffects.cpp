// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Drop Shadow and Inner Shadow (docs/specs/S80-title-art-core.md §6.5), ported from Pachislop
// src/effects/a/shadow.ts (dropShadow, innerShadow, their lit() global light) by SafEight, used
// with the author's permission.

#include "TitleFx.h"
#include "TitleFxParams.h"
#include "TitleFxFamilies.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fxparams;

namespace {

    // the shadow's offset in target px: `distance` along the direction it falls
    TitleVec2 shadowOffset(const EffectContext& ctx, const ParamValues& p) {
        const TitleVec2 dir = fx::dirClockwise(p.number("angle"));
        const double d = p.number("distance") * ctx.pxScale;
        return {dir.x * d, dir.y * d};
    }

    void applyDropShadow(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        // fx_dropShadow reads the blur's alpha only
        fx::Scratch blurred = fx::blurAlpha(ctx, input, (float)std::max(0.0, p.number("size")) * ctx.pxScale);
        if (!blurred) return;
        TitlePass pass = fx::makePass(ctx, TitleProgram::DropShadow, "drop_shadow", output);
        pass.textures[0].target = &input;
        pass.textures[1].target = blurred.get();
        const TitleVec2 off = shadowOffset(ctx, p);
        const TitleColor c = fx::linearOf(p.color("color"));
        pass.set(2, (float)off.x, (float)off.y, (float)p.number("spread"), (float)p.number("opacity"));
        pass.set(3, c.r, c.g, c.b);
        ctx.runner.run(pass);
    }

    EffectDef dropShadow() {
        EffectDef d;
        d.type = "drop_shadow";
        d.label = "Drop Shadow";
        d.category = "Shadow & Depth";
        d.description = "An offset, blurred shadow behind the layer; with Use Global Light it falls away from the document's light";
        d.params = {
            degrees("angle", "Angle", 60, "The direction the shadow falls, clockwise (90 = straight down)"),
            px("distance", "Distance", 10, 0, 500, "How far the shadow is offset"),
            px("size", "Size", 14, 0, 300, "How soft the shadow is (its blur)"),
            number("spread", "Spread", 0.1, 0, 0.95, ParamUnit::None, "How much of the shadow stays solid before it fades"),
            color("color", "Color", "#000000", "The shadow's colour"),
            number("opacity", "Opacity", 0.75, 0, 1, ParamUnit::None, "The shadow's opacity"),
        };
        d.gizmos = {{GizmoKind::Direction, "angle", "distance"}};
        d.reach = [](const ParamValues& p) { return (float)(p.number("distance") + p.number("size")); };
        // the blur's taps run to ~4 sigma = 1.33 x its radius
        d.gather = [](const ParamValues& p) { return (float)(p.number("distance") + 1.5 * p.number("size")); };
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F1;
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"dropShadow", "shadow", "ドロップシャドウ", "影"};
        // S83 §6.12: the shadow split scales per frame
        markLive(d, {"opacity"}, LiveKind::ScaleSplit, (uint32_t)TitleProgram::DropShadow);
        d.apply = applyDropShadow;
        d.alphaFromAlpha = true;
        return withGlobalLight(std::move(d), "angle", LightRelation::Away);
    }

    void applyInnerShadow(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        fx::Scratch blurred = fx::blur(ctx, input, (float)std::max(0.0, p.number("size")) * ctx.pxScale);
        if (!blurred) return;
        TitlePass pass = fx::makePass(ctx, TitleProgram::InnerShadow, "inner_shadow", output);
        pass.textures[0].target = &input;
        pass.textures[1].target = blurred.get();
        const TitleVec2 off = shadowOffset(ctx, p);
        const TitleColor c = fx::linearOf(p.color("color"));
        pass.set(2, (float)off.x, (float)off.y, (float)p.number("choke"), (float)p.number("opacity"));
        pass.set(3, c.r, c.g, c.b);
        ctx.runner.run(pass);
    }

    EffectDef innerShadow() {
        EffectDef d;
        d.type = "inner_shadow";
        d.label = "Inner Shadow";
        d.category = "Shadow & Depth";
        d.description = "Photoshop's inner shadow: the edge facing the light casts a soft shadow into the layer";
        d.params = {
            degrees("angle", "Angle", 60, "The direction the shadow falls, clockwise (90 = downward)"),
            px("distance", "Distance", 6, 0, 300, "How far the shadow is offset"),
            px("size", "Size", 8, 0, 200, "How soft the shadow is (its blur)"),
            number("choke", "Choke", 0, 0, 0.95, ParamUnit::None, "How much of the shadow stays solid before it fades"),
            color("color", "Color", "#000000", "The shadow's colour"),
            number("opacity", "Opacity", 0.6, 0, 1, ParamUnit::None, "The shadow's opacity"),
        };
        d.gizmos = {{GizmoKind::Direction, "angle", "distance"}};
        d.gather = [](const ParamValues& p) { return (float)(p.number("distance") + 1.5 * p.number("size")); };
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F1;
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"innerShadow", "シャドウ (内側)"};
        d.apply = applyInnerShadow;
        return withGlobalLight(std::move(d), "angle", LightRelation::Away);
    }

}

void titleart::registerShadowEffects(TitleEffectRegistry& registry) {
    registry.add(dropShadow());
    registry.add(innerShadow());
}
