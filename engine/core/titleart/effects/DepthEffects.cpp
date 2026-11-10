// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Extrude, Perspective Shadow and Floor Reflection (docs/specs/S83-title-effects-generators.md
// §6.6.7, §6.6.10, §6.6.11), ported from Pachislop src/effects/a/stylize.ts (extrude2d) and
// src/effects/a/shadow.ts (perspectiveShadow, floorReflection) by SafEight, used with the author's
// permission.

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

    // ---- Extrude ----

    // the march step: max(0.75, length / 1024) render px; the mobile tier steps twice as far (D12)
    float extrudeStep(float len, FxTier tier) {
        return std::max(0.75f, len / 1024.0f) * (tier == FxTier::Mobile ? 2.0f : 1.0f);
    }

    void applyExtrude(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float len = std::max(1.0f, (float)p.number("length") * ctx.pxScale);
        fx::Scratch normals = fx::blur(ctx, input, std::max(1.5f, 3.0f * ctx.pxScale));
        const TitleLut* ramp = ctx.runner.lut(fx::gradientLut(p.gradient("gradient")));
        if (!normals || !ramp) return;
        TitlePass pass = tfx::makePass(ctx, tfx::Program::Extrude, "extrude", output);
        pass.textures[0].target = &input;
        pass.textures[1].target = normals.get();
        pass.textures[2].lut = ramp;
        const TitleVec2 dir = fx::dirClockwise(p.number("angle"));
        const TitleVec2 light = tfx::lightToward(p.number("light_angle"));
        pass.set(2, (float)dir.x, (float)dir.y, len, extrudeStep(len, ctx.tier));
        pass.set(3, (float)light.x, (float)light.y, (float)p.number("shading"), (float)p.number("edge"));
        pass.set(4, (float)p.number("opacity"), (float)p.number("fade"));
        ctx.runner.run(pass);
    }

    FxCost estimateExtrude(const ParamValues& p, const FxSize& size, FxTier tier) {
        tfx::CostBuilder b;
        const float len = std::max(1.0f, (float)p.number("length") * size.pxScale);
        b.blur(std::max(1.5, 3.0 * size.pxScale));
        // each step reads the alpha; the four normal taps run on the steps that hit ink, and the
        // march stops once the copies cover the pixel
        const int steps = (int)std::ceil(len / extrudeStep(len, tier));
        return b.pass(1.0, steps + 4).cost;
    }

    EffectDef extrude() {
        EffectDef d;
        d.type = "extrude";
        d.label = "Extrude";
        d.category = "Shadow & Depth";
        d.description = "A 2D extrusion: the shape stacked toward a direction behind the layer with shaded, gradient sides; with Shading and Edge "
                        "at 0, a one-colour gradient and some Fade it is a flat long shadow";
        d.params = {
            degrees("angle", "Angle", 75, "The direction the stack recedes, clockwise (90 = down)"),
            px("length", "Length", 22, 1, 3000, "How deep the extrusion is"),
            gradient("gradient", "Gradient", gradientValue({{0.0, "#d97a00"}, {1.0, "#4a1600"}}), "The sides' colours from the front to the back"),
            degrees("light_angle", "Light Angle", 120, "Where the light shading the sides comes from (Photoshop's angle)"),
            number("shading", "Shading", 0.7, 0, 1, ParamUnit::None, "How much the light shades the sides"),
            number("edge", "Edge Darkening", 0.55, 0, 1, ParamUnit::None, "A dark contact line at the face and at the back"),
            number("fade", "Fade", 0, 0, 1, ParamUnit::None, "Thins the extrusion toward its back"),
            number("opacity", "Opacity", 1, 0, 1, ParamUnit::None, "The extrusion's opacity"),
        };
        d = withGlobalLight(std::move(d), "light_angle", LightRelation::From);
        d.gizmos = {{GizmoKind::Direction, "angle", "length"}, {GizmoKind::Light, "light_angle", nullptr}};
        d.reach = [](const ParamValues& p) { return (float)p.number("length"); };
        d.gather = [](const ParamValues& p) { return (float)p.number("length"); };
        d.costClass = CostClass::C3;
        d.fallback = FallbackClass::F0;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"extrude2d", "3d text", "block letters", "depth", "long shadow", "立体", "押し出し"};
        d.estimate = estimateExtrude;
        d.presetFile = "editor/resources/titleart/fx-presets/extrude.yaml";
        d.apply = applyExtrude;
        return d;
    }

    // ---- Perspective Shadow ----

    void applyPerspectiveShadow(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float s = ctx.pxScale;
        fx::Scratch nearBlur = fx::blur(ctx, input, (float)std::max(0.0, p.number("blur_near")) * s);
        fx::Scratch farBlur = fx::blur(ctx, input, (float)std::max(0.0, p.number("blur_far")) * s);
        fx::Scratch box = fx::layerBounds(ctx, input);
        if (!nearBlur || !farBlur || !box) return;
        const float len = std::max(1.0f, (float)p.number("length") * s);
        const TitleVec2 dir = tfx::dirUp(p.number("angle"));
        // at least 10 degrees from horizontal, so the floor projection stays finite
        const double vy = (dir.y < 0.0 ? -1.0 : 1.0) * std::max(std::fabs(dir.y), 0.17);
        TitlePass pass = tfx::makePass(ctx, tfx::Program::PerspectiveShadow, "perspective_shadow", output);
        pass.textures[0].target = &input;
        pass.textures[1].target = nearBlur.get();
        pass.textures[2].target = farBlur.get();
        pass.raw[1].target = box.get();
        const TitleColor c = fx::linearOf(p.color("color"));
        pass.set(2, c.r, c.g, c.b, (float)p.number("opacity"));
        pass.set(3, (float)(dir.x * len), (float)(vy * len), (float)p.number("perspective"), (float)p.number("fade"));
        pass.set(4, (float)p.number("offset") * s);
        ctx.runner.run(pass);
    }

    FxCost estimatePerspectiveShadow(const ParamValues& p, const FxSize& size, FxTier) {
        tfx::CostBuilder b;
        b.blur(std::max(0.0, p.number("blur_near")) * size.pxScale).blur(std::max(0.0, p.number("blur_far")) * size.pxScale).box();
        return b.pass(1.0, 4).cost;
    }

    EffectDef perspectiveShadow() {
        using namespace tfx;
        EffectDef d;
        d.type = "perspective_shadow";
        d.label = "Perspective Shadow";
        d.category = "Shadow & Depth";
        d.description = "A shadow cast onto a floor at the base of the layer: sheared, squashed, blurring and fading with distance; up casts "
                        "it behind the layer, down in front";
        d.params = {
            live(degrees("angle", "Angle", -55, "The direction the shadow falls, clockwise (-90 = straight back)", -180, 180)),
            live(px("length", "Length", 230, 5, 2000, "From the base to the shadow's tip")),
            number("perspective", "Perspective", 0.35, 0, 0.95, ParamUnit::None, "How much the far end narrows"),
            px("blur_near", "Blur Near", 2, 0, 100, "The blur at the base"),
            px("blur_far", "Blur Far", 22, 0, 300, "The blur at the tip"),
            number("fade", "Fade", 0.75, 0, 1, ParamUnit::None, "How much the shadow fades toward its tip"),
            px("offset", "Floor Offset", 0, -300, 300, "Moves the floor line up or down"),
            color("color", "Color", "#000000", "The shadow's colour"),
            live(number("opacity", "Opacity", 0.7, 0, 1, ParamUnit::None, "The shadow's opacity")),
        };
        d = withGlobalLight(std::move(d), "angle", LightRelation::Floor);
        d.gizmos = {{GizmoKind::Direction, "angle", "length"}};
        d.reach = [](const ParamValues& p) { return (float)(p.number("length") + std::fabs(p.number("offset")) + p.number("blur_far")); };
        d.gather = [](const ParamValues&) { return -1.0f; };
        d.costClass = CostClass::C2;
        d.fallback = FallbackClass::F0;
        d.live = {LiveKind::Kernel, {"angle", "length", "opacity"}, (uint32_t)Program::PerspectiveShadow};
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"perspectiveShadow", "cast shadow", "floor shadow", "ground shadow", "影 (遠近)"};
        d.estimate = estimatePerspectiveShadow;
        d.presetFile = "editor/resources/titleart/fx-presets/perspective_shadow.yaml";
        d.apply = applyPerspectiveShadow;
        return d;
    }

    // ---- Floor Reflection ----

    void applyFloorReflection(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float s = ctx.pxScale;
        fx::Scratch box = fx::layerBounds(ctx, input);
        fx::Scratch soft = fx::blur(ctx, input, (float)std::max(0.0, p.number("blur")) * s);
        if (!box || !soft) return;
        TitlePass pass = tfx::makePass(ctx, tfx::Program::FloorReflection, "floor_reflection", output);
        pass.textures[0].target = &input;
        pass.textures[1].target = soft.get();
        pass.raw[1].target = box.get();
        pass.set(2, (float)p.number("gap") * s, std::max(1.0f, (float)p.number("length") * s), (float)p.number("opacity"),
                 (float)std::max(0.05, p.number("scale")));
        pass.set(3, (float)std::max(0.05, p.number("falloff")));
        ctx.runner.run(pass);
    }

    FxCost estimateFloorReflection(const ParamValues& p, const FxSize& size, FxTier) {
        tfx::CostBuilder b;
        b.box().blur(std::max(0.0, p.number("blur")) * size.pxScale);
        return b.pass(1.0, 3).cost;
    }

    EffectDef floorReflection() {
        using namespace tfx;
        EffectDef d;
        d.type = "floor_reflection";
        d.label = "Floor Reflection";
        d.category = "Shadow & Depth";
        d.description = "A mirrored copy of the layer below its base, as if standing on a glossy floor, fading and blurring with distance";
        d.params = {
            px("gap", "Gap", 4, -100, 300, "The space between the layer and its reflection"),
            px("length", "Fade Length", 150, 5, 2000, "How far the reflection reaches before it fades out"),
            live(number("opacity", "Opacity", 0.45, 0, 1, ParamUnit::None, "The reflection's opacity")),
            px("blur", "Blur Growth", 14, 0, 200, "The blur at the reflection's far end"),
            number("falloff", "Falloff", 1.4, 0.3, 5, ParamUnit::None, "How quickly it fades", 0.05),
            number("scale", "Vertical Scale", 1, 0.2, 2, ParamUnit::Factor, "Squashes or stretches the reflection"),
        };
        d.reach = [](const ParamValues& p) { return (float)(std::max(0.0, p.number("gap")) + p.number("length") + p.number("blur")); };
        d.gather = [](const ParamValues&) { return -1.0f; };
        d.costClass = CostClass::C3;
        d.fallback = FallbackClass::F0;
        d.live = {LiveKind::ScaleSplit, {"opacity"}, (uint32_t)Program::FloorReflection};
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"floorReflection", "mirror", "reflection", "glossy floor", "映り込み", "反射"};
        d.estimate = estimateFloorReflection;
        d.presetFile = "editor/resources/titleart/fx-presets/floor_reflection.yaml";
        d.apply = applyFloorReflection;
        return d;
    }

}

void titleart::registerDepthEffects(TitleEffectRegistry& registry) {
    registry.add(extrude());
    registry.add(perspectiveShadow());
    registry.add(floorReflection());
}
