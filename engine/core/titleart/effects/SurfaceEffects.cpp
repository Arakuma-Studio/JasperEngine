// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Distress, Drips, Frost and Chalk (docs/specs/S83-title-effects-generators.md §6.6.18, §6.6.19,
// §6.6.23, §6.6.25): S83's genre effects. Distress's dry brush runs S80's かすれ code (itself ported
// from Pachislop src/engine/layer2d/Title2DRenderer.ts by SafEight, used with the author's
// permission) on the layer's content.

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

    const TargetRef& contentOf(const EffectContext& ctx, const TargetRef& input) {
        return ctx.content ? *ctx.content : input;
    }

    // ---- Distress ----

    const std::initializer_list<const char*> DISTRESS_PATTERNS = {"grunge", "dry_brush", "rough_edges", "scratches", "speckle", "spray"};

    float distressFieldReach(const ParamValues& p, float s) {
        const float edge = (float)p.number("edge") * s, scale = (float)p.number("scale") * s;
        if (p.string("pattern") == "dry_brush") return std::max(0.5f, edge) * 3.0f + 3.0f;
        return std::max(edge, scale) + 3.0f;
    }

    void applyDistress(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float s = ctx.pxScale;
        const TargetRef& content = contentOf(ctx, input);
        fx::Scratch field = fx::distanceField(ctx, content, 0.5f, distressFieldReach(p, s));
        if (!field) return;
        // the wear sits on the canvas (the noise and the brush's lanes)
        ctx.usesCanvasPosition = true;
        const TitleVec2 centre = tfx::canvasCentre(ctx);
        const TitleVec2 dir = fx::dirClockwise(p.number("angle"));
        const TitleColor wear = fx::linearOf(p.color("wear_color"));
        const float seed = tfx::seedOf(ctx, p);
        // the dry brush's own values, as S80's style.erode reads them (Title2DPass)
        const float edgePx = std::max(0.5f, (float)p.number("edge") * s);
        const float lenPx = std::max(1.0f, (float)p.number("scale") * s);
        const float lanePx = std::max(0.5f, 0.6f * s);
        TitlePass pass = tfx::makePass(ctx, tfx::Program::Distress, "distress", output);
        pass.textures[0].target = &input;
        pass.textures[1].target = &content;
        pass.raw[0].target = field.get();
        pass.set(2, (float)optionIndex(p, "pattern", DISTRESS_PATTERNS), (float)p.number("amount"), (float)p.number("edge") * s, (float)p.number("scale"));
        pass.set(3, (float)dir.x, (float)dir.y, (float)p.number("contrast"), (float)p.number("wear"));
        pass.set(4, wear.r, wear.g, wear.b, (float)p.number("outside"));
        pass.set(5, seed, (float)centre.x, (float)centre.y, lanePx);
        pass.set(6, lenPx, (float)std::fmod(seed * 37.31, 997.0), (float)std::fmod(seed * 17.73, 991.0), edgePx);
        ctx.runner.run(pass);
    }

    FxCost estimateDistress(const ParamValues& p, const FxSize&, FxTier) {
        tfx::CostBuilder b;
        b.field();
        if (p.string("pattern") == "dry_brush") {
            // two exit marches of up to 96 steps and 6 refinements, on the band along the edge
            // (about a third of the layer's pixels)
            return b.pass(0.3, 2 * (96 + 6) + 12).cost;
        }
        return b.pass(1.0, 12).cost;
    }

    EffectDef distress() {
        using namespace tfx;
        EffectDef d;
        d.type = "distress";
        d.label = "Distress";
        d.category = "Texture & Material";
        d.description = "Wears the layer away: grunge, a dry brush (かすれ), rough edges, scratches, speckle or spray paint, inside a band "
                        "along the edge or over the whole layer, the rest darkened by dirt";
        d.params = {
            choice("pattern", "Pattern", "grunge",
                   {{"grunge", "Grunge"}, {"dry_brush", "Dry Brush"}, {"rough_edges", "Rough Edges"}, {"scratches", "Scratches"}, {"speckle", "Speckle"},
                    {"spray", "Spray"}},
                   "How the layer wears"),
            number("amount", "Amount", 0.5, 0, 1, ParamUnit::None, "How much wears away"),
            px("edge", "Edge", 12, 0, 400, "The band in from the edge that wears (0 = the whole layer)"),
            px("scale", "Scale", 24, 1, 1000, "The wear's size (the dry brush: its stroke length)"),
            when(degrees("angle", "Angle", 0, "The brush or scratch direction, clockwise"), "pattern", {"dry_brush", "scratches"}),
            number("contrast", "Contrast", 0.6, 0, 1, ParamUnit::None, "How crisp the worn edges are"),
            number("wear", "Wear", 0, 0, 1, ParamUnit::None, "Darkens what remains with dirt"),
            color("wear_color", "Wear Color", "#3a2a1a", "The dirt's colour"),
            when(number("outside", "Outside", 0, 0, 1, ParamUnit::None, "Speckle or spray beyond the edge, within Edge"), "pattern", {"speckle", "spray"}),
            seed("seed", "Seed", "The wear's random pattern"),
        };
        d.gizmos = {{GizmoKind::Direction, "angle", nullptr}};
        d.reach = [](const ParamValues& p) {
            const std::string pattern = p.string("pattern");
            return (pattern == "speckle" || pattern == "spray") && p.number("outside") > 0.0
                       ? (float)(p.number("edge") > 0.0 ? p.number("edge") : p.number("scale"))
                       : 0.0f;
        };
        d.gather = [](const ParamValues& p) { return (float)std::max(p.number("edge"), p.number("scale")) * 3.0f; };
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F0;
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"grunge", "worn", "かすれ", "weathered", "dry brush", "rough", "scratched", "spray paint", "汚し"};
        d.inputs.content = true;
        d.inputs.sdf = true;
        d.estimate = estimateDistress;
        d.presetFile = "editor/resources/titleart/fx-presets/distress.yaml";
        d.apply = applyDistress;
        return d;
    }

    // ---- Drips ----

    const std::initializer_list<const char*> DRIP_STYLES = {"paint", "slime", "blood", "icicles"};

    void applyDrips(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float s = ctx.pxScale;
        const int count = std::max(0, std::min(256, p.integer("count")));
        if (count == 0) {
            fx::copy(ctx, input, output);
            return;
        }
        const TargetRef& content = contentOf(ctx, input);
        const int style = optionIndex(p, "style", DRIP_STYLES);
        const float width = std::max(1.0f, (float)p.number("width") * s);
        const float merge = (float)p.number("merge") * s;
        fx::Scratch field = fx::distanceField(ctx, content, 0.5f, merge + width + 4.0f);
        fx::Scratch box = fx::layerBounds(ctx, content);
        fx::Scratch origins = fx::scratchOf(ctx.runner, TitleBuffer::Seeds, count, 1);
        fx::Scratch drips = fx::scratchOf(ctx.runner, TitleBuffer::Distance, ctx.width, ctx.height);
        if (!field || !box || !origins || !drips) return;
        const TitleVec2 g = fx::dirClockwise(p.number("angle"));
        const float seed = tfx::seedOf(ctx, p);
        TitlePass o = tfx::makePass(ctx, tfx::Program::DripOrigins, "drips origins", *origins);
        o.textures[1].target = &content;
        o.raw[0].target = field.get();
        o.raw[1].target = box.get();
        o.set(2, (float)g.x, (float)g.y, (float)p.number("edge_bias") / 100.0f, seed);
        o.set(3, std::max(1.0f, 1.5f * s), (float)ctx.width, (float)ctx.height);
        if (!ctx.runner.run(o)) return;
        TitlePass f = tfx::makePass(ctx, tfx::Program::DripField, "drips field", *drips);
        f.raw[0].target = origins.get();
        f.set(2, (float)g.x, (float)g.y, (float)count, seed);
        f.set(3, (float)std::max(0.0, p.number("length")) * s, (float)p.number("length_random") / 100.0f, width, (float)p.number("width_random") / 100.0f);
        // icicles taper to a point
        f.set(4, style == 3 ? 0.0f : (float)p.number("head"));
        if (!ctx.runner.run(f)) return;
        TitlePass c = tfx::makePass(ctx, tfx::Program::DripCompose, "drips compose", output);
        c.textures[0].target = &input;
        c.textures[1].target = &content;
        c.raw[0].target = field.get();
        c.raw[1].target = drips.get();
        const TitleColor col = fx::linearOf(p.color("color"));
        const TitleVec2 light = tfx::lightToward(p.number("light_angle"));
        c.set(2, std::max(0.5f, merge), p.string("color_mode") == "custom" ? 1.0f : 0.0f, (float)p.number("gloss"), (float)style);
        c.set(3, col.r, col.g, col.b, width);
        c.set(4, (float)light.x, (float)light.y);
        ctx.runner.run(c);
    }

    FxCost estimateDrips(const ParamValues& p, const FxSize& size, FxTier) {
        tfx::CostBuilder b;
        const int count = std::max(0, std::min(256, p.integer("count")));
        if (count == 0) return b.pass(1.0, 1).cost;
        b.field().box();
        // the origins: 8 candidate columns, two marches each, one texel per drip
        const double columnSteps = std::min(512.0, std::max(size.height, size.width) / std::max(1.0, 1.5 * size.pxScale));
        b.pass((double)count / std::max(1.0, (double)size.width * size.height), (int)(16 * columnSteps));
        b.pass(1.0, count);
        return b.pass(1.0, 8).cost;
    }

    EffectDef drips() {
        using namespace tfx;
        EffectDef d;
        d.type = "drips";
        d.label = "Drips";
        d.category = "Texture & Material";
        d.description = "Paint, slime, blood or icicles dripping from the edges that face gravity, merged smoothly with the shape and shaded "
                        "with a gloss streak";
        d.params = {
            choice("style", "Style", "paint", {{"paint", "Paint"}, {"slime", "Slime"}, {"blood", "Blood"}, {"icicles", "Icicles"}},
                   "The drips' look (icicles taper to a point)"),
            relayout(integer("count", "Count", 14, 0, 200, "How many drips")),
            live(px("length", "Length", 90, 0, 2000, "How long the drips run")),
            number("length_random", "Length Random", 60, 0, 100, ParamUnit::None, "How much the drips' lengths vary, in percent", 1),
            px("width", "Width", 14, 1, 200, "How wide the drips are"),
            number("width_random", "Width Random", 40, 0, 100, ParamUnit::None, "How much the drips' widths vary, in percent", 1),
            number("head", "Head", 1.4, 1, 3, ParamUnit::Factor, "The rounded head's size against the drip's width"),
            degrees("angle", "Gravity", 90, "The direction the drips run, clockwise (90 = down)"),
            number("edge_bias", "Edge Bias", 70, 0, 100, ParamUnit::None, "Prefer edges facing gravity squarely, in percent", 1),
            px("merge", "Merge", 6, 0, 60, "How smoothly the drips join the shape"),
            choice("color_mode", "Color Mode", "layer", {{"layer", "Layer"}, {"custom", "Custom"}},
                   "The drips take the ink above them, or one colour"),
            when(color("color", "Color", "#c8102e", "The drips' colour"), "color_mode", {"custom"}),
            number("gloss", "Gloss", 0.6, 0, 1, ParamUnit::None, "How glossy the drips are"),
            degrees("light_angle", "Light Angle", 120, "Where the light on the drips comes from (Photoshop's angle)"),
            relayout(seed("seed", "Seed", "Where the drips fall")),
        };
        d = withGlobalLight(std::move(d), "light_angle", LightRelation::From);
        d.gizmos = {{GizmoKind::Direction, "angle", "length"}, {GizmoKind::Light, "light_angle", nullptr}};
        d.reach = [](const ParamValues& p) { return (float)(p.number("length") + p.number("width") * p.number("head") + p.number("merge")); };
        d.gather = [](const ParamValues&) { return -1.0f; };
        d.costClass = CostClass::C3;
        d.fallback = FallbackClass::F1;
        d.live = {LiveKind::Kernel, {"length"}, (uint32_t)Program::DripField};
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"drip", "dripping", "melting", "slime", "blood", "icicles", "horror", "垂れ", "したたり"};
        d.inputs.content = true;
        d.inputs.sdf = true;
        d.estimate = estimateDrips;
        d.presetFile = "editor/resources/titleart/fx-presets/drips.yaml";
        d.apply = applyDrips;
        return d;
    }

    // ---- Frost ----

    void applyFrost(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float s = ctx.pxScale;
        const TargetRef& content = contentOf(ctx, input);
        const float reach = (float)p.number("reach") * s, outside = (float)p.number("outside") * s;
        fx::Scratch field = fx::distanceField(ctx, content, 0.5f, std::max(reach * 1.5f, outside) + 3.0f);
        if (!field) return;
        ctx.usesCanvasPosition = true;
        const TitleVec2 centre = tfx::canvasCentre(ctx);
        const TitleVec2 light = tfx::lightToward(ctx.lightAngle);
        const TitleColor c = fx::linearOf(p.color("color")), tint = fx::linearOf(p.color("tint"));
        TitlePass pass = tfx::makePass(ctx, tfx::Program::Frost, "frost", output);
        pass.textures[0].target = &input;
        pass.textures[1].target = &content;
        pass.raw[0].target = field.get();
        pass.set(2, (float)p.number("amount"), reach, outside, (float)p.number("crystal_size"));
        pass.set(3, c.r, c.g, c.b, (float)p.number("tint_amount"));
        pass.set(4, tint.r, tint.g, tint.b, (float)p.number("sparkle"));
        pass.set(5, tfx::seedOf(ctx, p), (float)light.x, (float)light.y);
        pass.set(6, (float)centre.x, (float)centre.y);
        ctx.runner.run(pass);
    }

    EffectDef frost() {
        using namespace tfx;
        EffectDef d;
        d.type = "frost";
        d.label = "Frost";
        d.category = "Texture & Material";
        d.description = "Ice crystals growing in from the edges as Amount rises, rime crystals reaching outward, facets glinting in the light";
        d.params = {
            live(number("amount", "Amount", 0.7, 0, 1, ParamUnit::None, "How far the frost has grown")),
            px("reach", "Reach", 18, 0, 400, "How far in from the edge the frost grows"),
            px("outside", "Outside", 6, 0, 200, "How far the rime crystals reach outward"),
            px("crystal_size", "Crystal Size", 10, 1, 200, "The crystals' size"),
            color("color", "Color", "#dff6ff", "The frost's colour"),
            color("tint", "Tint", "#7fc8ff", "The tint mixed into the frost"),
            number("tint_amount", "Tint Amount", 0.35, 0, 1, ParamUnit::None, "How much of the tint"),
            number("sparkle", "Sparkle", 0.4, 0, 1, ParamUnit::None, "How much the facets glint in the global light"),
            seed("seed", "Seed", "The crystals' random layout"),
        };
        d.reach = [](const ParamValues& p) { return (float)p.number("outside"); };
        d.gather = [](const ParamValues& p) { return (float)std::max(p.number("reach"), p.number("outside")) * 1.5f; };
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F1;
        d.live = {LiveKind::Kernel, {"amount"}, (uint32_t)Program::Frost};
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"ice", "frozen", "icy", "rime", "winter", "霜", "氷"};
        d.inputs.content = true;
        d.inputs.sdf = true;
        d.estimate = [](const ParamValues&, const FxSize&, FxTier) {
            tfx::CostBuilder b;
            return b.field().pass(1.0, 12).cost;
        };
        d.presetFile = "editor/resources/titleart/fx-presets/frost.yaml";
        d.apply = applyFrost;
        return d;
    }

    // ---- Chalk ----

    void applyChalk(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float s = ctx.pxScale;
        const TargetRef& content = contentOf(ctx, input);
        const float edgeW = (float)p.number("edge_width") * s, dustReach = (float)p.number("dust_reach") * s;
        fx::Scratch field = fx::distanceField(ctx, content, 0.5f, std::max(edgeW, dustReach) + 3.0f);
        if (!field) return;
        ctx.usesCanvasPosition = true;
        const TitleVec2 centre = tfx::canvasCentre(ctx);
        const TitleVec2 dir = fx::dirClockwise(p.number("stroke_angle"));
        const TitleColor c = fx::linearOf(p.color("color"));
        TitlePass pass = tfx::makePass(ctx, tfx::Program::Chalk, "chalk", output);
        pass.textures[0].target = &input;
        pass.textures[1].target = &content;
        pass.raw[0].target = field.get();
        pass.set(2, (float)p.number("grain"), (float)p.number("coverage"), (float)p.number("stroke_length"), (float)p.number("edge_rough"));
        pass.set(3, (float)dir.x, (float)dir.y, edgeW, (float)p.number("dust"));
        pass.set(4, c.r, c.g, c.b, p.string("color_mode") == "custom" ? 1.0f : 0.0f);
        pass.set(5, dustReach, tfx::seedOf(ctx, p), (float)centre.x, (float)centre.y);
        ctx.runner.run(pass);
    }

    EffectDef chalk() {
        using namespace tfx;
        EffectDef d;
        d.type = "chalk";
        d.label = "Chalk";
        d.category = "Texture & Material";
        d.description = "The shape drawn in chalk: strokes along an angle over a paper grain, a broken edge and dust falling around it";
        d.params = {
            px("grain", "Grain", 3, 0.5, 40, "The paper grain's size", 0.5),
            number("coverage", "Coverage", 0.7, 0, 1, ParamUnit::None, "How much of the shape the chalk covers"),
            degrees("stroke_angle", "Stroke Angle", -30, "The strokes' direction, clockwise"),
            px("stroke_length", "Stroke Length", 40, 1, 400, "How long the strokes are"),
            number("edge_rough", "Edge Roughness", 0.5, 0, 1, ParamUnit::None, "How broken the edge is"),
            px("edge_width", "Edge Width", 4, 0, 50, "How far the broken edge reaches", 0.5),
            number("dust", "Dust", 0.3, 0, 1, ParamUnit::None, "Chalk dust around the shape"),
            px("dust_reach", "Dust Reach", 12, 0, 200, "How far the dust falls"),
            choice("color_mode", "Color Mode", "layer", {{"layer", "Layer"}, {"custom", "Custom"}}, "The layer's colours, or one chalk colour"),
            when(color("color", "Color", "#f4f1e8", "The chalk's colour"), "color_mode", {"custom"}),
            seed("seed", "Seed", "The strokes' random pattern"),
        };
        d.gizmos = {{GizmoKind::Direction, "stroke_angle", "stroke_length"}};
        d.reach = [](const ParamValues& p) { return (float)std::max(p.number("dust_reach"), p.number("edge_width")); };
        d.gather = [](const ParamValues& p) { return (float)std::max(p.number("dust_reach"), p.number("edge_width")) + 2.0f; };
        d.costClass = CostClass::C2;
        d.fallback = FallbackClass::F0;
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"chalkboard", "blackboard", "pastel", "crayon", "チョーク", "黒板"};
        d.inputs.content = true;
        d.inputs.sdf = true;
        d.estimate = [](const ParamValues&, const FxSize&, FxTier) {
            tfx::CostBuilder b;
            return b.field().pass(1.0, 14).cost;
        };
        d.presetFile = "editor/resources/titleart/fx-presets/chalk.yaml";
        d.apply = applyChalk;
        return d;
    }

}

void titleart::registerSurfaceEffects(TitleEffectRegistry& registry) {
    registry.add(distress());
    registry.add(drips());
    registry.add(frost());
    registry.add(chalk());
}
