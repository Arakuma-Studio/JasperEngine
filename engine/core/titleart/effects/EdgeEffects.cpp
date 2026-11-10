// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ink Brush Stroke and Stitch (docs/specs/S83-title-effects-generators.md §6.6.22, §6.6.26): S83's
// stroke effects on the layer's content.

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

    // ---- Ink Brush Stroke ----

    void applyBrushStroke(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float s = ctx.pxScale;
        const float w = std::max(0.5f, (float)p.number("width") * s);
        const TargetRef& content = ctx.content ? *ctx.content : input;
        fx::Scratch field = fx::distanceField(ctx, content, 0.5f, 2.0f * w + 3.0f);
        if (!field) return;
        // the bristle noise lies on the canvas
        ctx.usesCanvasPosition = true;
        const TitleVec2 centre = tfx::canvasCentre(ctx);
        const TitleColor c = fx::linearOf(p.color("color"));
        TitlePass pass = tfx::makePass(ctx, tfx::Program::BrushStroke, "brush_stroke", output);
        pass.textures[0].target = &input;
        pass.raw[0].target = field.get();
        pass.set(2, (float)optionIndex(p, "position", {"outside", "center", "inside"}), w, (float)p.number("pressure"), (float)p.number("bristles"));
        pass.set(3, (float)p.number("dry"), (float)p.number("roughness"), (float)p.number("opacity"), tfx::seedOf(ctx, p));
        pass.set(4, c.r, c.g, c.b);
        pass.set(5, (float)centre.x, (float)centre.y);
        ctx.runner.run(pass);
    }

    EffectDef brushStroke() {
        EffectDef d;
        d.type = "brush_stroke";
        d.label = "Ink Brush Stroke";
        d.category = "Stroke & Edge";
        d.description = "An ink brush stroke along the silhouette (outside, centred or inside) whose width breathes with pressure, split "
                        "into bristle lanes with dry gaps and ragged edges: sumi-e, marker, calligraphy";
        d.params = {
            choice("position", "Position", "outside", {{"outside", "Outside"}, {"center", "Center"}, {"inside", "Inside"}}, "Where the stroke sits on the edge"),
            px("width", "Width", 10, 0.5, 200, "The stroke's width", 0.5),
            number("pressure", "Pressure", 0.6, 0, 1, ParamUnit::None, "How much the width varies along the contour"),
            number("bristles", "Bristles", 0.5, 0, 1, ParamUnit::None, "How visible the bristle lanes are"),
            number("dry", "Dry", 0.3, 0, 1, ParamUnit::None, "Dry-brush gaps along the stroke"),
            number("roughness", "Roughness", 0.4, 0, 1, ParamUnit::None, "How ragged the stroke's edges are"),
            color("color", "Color", "#111111", "The ink's colour"),
            number("opacity", "Opacity", 1, 0, 1, ParamUnit::None, "The stroke's opacity"),
            seed("seed", "Seed", "The stroke's random pattern"),
        };
        d.gizmos = {{GizmoKind::Radius, "width", nullptr}};
        d.reach = [](const ParamValues& p) {
            const std::string pos = p.string("position");
            return (float)(pos == "inside" ? 0.2 * p.number("width") : pos == "center" ? 0.7 * p.number("width") : 1.2 * p.number("width"));
        };
        d.gather = [](const ParamValues& p) { return 2.0f * (float)p.number("width") + 2.0f; };
        d.outline = true;
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F0;
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"brush", "ink", "sumi", "calligraphy", "marker", "筆", "墨", "毛筆"};
        d.inputs.content = true;
        d.inputs.sdf = true;
        d.estimate = [](const ParamValues&, const FxSize&, FxTier) {
            tfx::CostBuilder b;
            return b.field().pass(1.0, 12).cost;
        };
        d.presetFile = "editor/resources/titleart/fx-presets/brush_stroke.yaml";
        d.apply = applyBrushStroke;
        return d;
    }

    // ---- Stitch ----

    // the layout's coarse cells (render px), and its march: step, the longest piece
    constexpr float STITCH_CELL = 4.0f;
    constexpr int STITCH_MARCH_STEPS = 512;

    float stitchAcross(const ParamValues& p) {
        return (float)(p.string("style") == "running" ? p.number("thread_width") : 0.5 * p.number("stitch_length") + p.number("thread_width"));
    }

    void applyStitch(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float s = ctx.pxScale;
        const float offset = (float)p.number("offset") * s;
        const float across = stitchAcross(p) * s;
        const TargetRef& content = ctx.content ? *ctx.content : input;
        fx::Scratch field = fx::distanceField(ctx, content, 0.5f, std::fabs(offset) + std::max(across, 2.0f * STITCH_CELL) + 4.0f);
        const int lw = std::max(1, (int)std::ceil(ctx.width / STITCH_CELL)), lh = std::max(1, (int)std::ceil(ctx.height / STITCH_CELL));
        fx::Scratch layout = fx::scratchOf(ctx.runner, TitleBuffer::Seeds, lw, lh);
        if (!field || !layout) return;
        const float stepPx = 1.5f;
        TitlePass l = tfx::makePass(ctx, tfx::Program::StitchLayout, "stitch layout", *layout);
        l.raw[0].target = field.get();
        l.set(2, offset, STITCH_CELL, stepPx, stepPx * STITCH_MARCH_STEPS);
        l.set(3, 1.5f * STITCH_CELL);
        if (!ctx.runner.run(l)) return;
        ctx.usesCanvasPosition = true;
        const TitleVec2 centre = tfx::canvasCentre(ctx);
        const TitleVec2 light = tfx::lightToward(p.number("light_angle"));
        const TitleVec2 fillDir = fx::dirClockwise(p.number("fill_angle"));
        const TitleColor c = fx::linearOf(p.color("color"));
        TitlePass pass = tfx::makePass(ctx, tfx::Program::Stitch, "stitch", output);
        pass.textures[0].target = &input;
        pass.raw[0].target = field.get();
        pass.raw[1].target = layout.get();
        pass.set(2, (float)optionIndex(p, "style", {"running", "cross", "zigzag", "satin_border"}), offset, (float)p.number("stitch_length") * s,
                 (float)p.number("gap") * s);
        pass.set(3, c.r, c.g, c.b, (float)p.number("thread_width") * s);
        pass.set(4, (float)p.number("shade"), (float)p.number("holes"), (float)optionIndex(p, "fill", {"none", "satin", "tatami"}),
                 (float)p.number("fill_density") * s);
        pass.set(5, (float)light.x, (float)light.y, (float)fillDir.x, (float)fillDir.y);
        pass.set(6, STITCH_CELL, (float)centre.x, (float)centre.y, tfx::seedOf(ctx, p));
        ctx.runner.run(pass);
    }

    FxCost estimateStitch(const ParamValues& p, const FxSize&, FxTier) {
        tfx::CostBuilder b;
        b.field();
        // the layout marches only the cells along the contour (about 3 in 16 of the coarse grid),
        // two ways, each step re-projected (12 taps)
        b.pass(3.0 / 16.0 / (STITCH_CELL * STITCH_CELL), 2 * 64 * 12);
        b.pass(1.0, p.string("fill") == "none" ? 40 : 44);
        return b.cost;
    }

    EffectDef stitch() {
        using namespace tfx;
        EffectDef d;
        d.type = "stitch";
        d.label = "Stitch";
        d.category = "Stroke & Edge";
        d.description = "Embroidery: thread stitches along the contour (running, cross, zigzag or a satin border), evenly fitted and shaded as "
                        "round threads with needle holes, with an optional satin or tatami fill (badges, patches, sports logos)";
        d.params = {
            choice("style", "Style", "running", {{"running", "Running"}, {"cross", "Cross"}, {"zigzag", "Zigzag"}, {"satin_border", "Satin Border"}},
                   "The stitch"),
            relayout(px("offset", "Offset", -6, -200, 200, "The stitch line's distance from the edge (- inside)", 0.5)),
            relayout(px("stitch_length", "Stitch Length", 12, 2, 100, "How long a stitch is", 0.5)),
            relayout(px("gap", "Gap", 6, 0, 100, "The space between two stitches", 0.5)),
            px("thread_width", "Thread Width", 3, 0.5, 20, "How thick the thread is", 0.25),
            color("color", "Color", "#ffffff", "The thread's colour"),
            number("shade", "Shade", 0.5, 0, 1, ParamUnit::None, "How round the thread looks"),
            number("holes", "Holes", 0.4, 0, 1, ParamUnit::None, "How dark the needle holes are"),
            choice("fill", "Fill", "none", {{"none", "None"}, {"satin", "Satin"}, {"tatami", "Tatami"}}, "Thread lines over the layer inside the stitches"),
            when(degrees("fill_angle", "Fill Angle", 45, "The fill threads' direction, clockwise"), "fill", {"satin", "tatami"}),
            when(px("fill_density", "Fill Density", 3, 1, 20, "The distance between two fill threads", 0.5), "fill", {"satin", "tatami"}),
            degrees("light_angle", "Light Angle", 120, "Where the light on the threads comes from (Photoshop's angle)"),
            relayout(seed("seed", "Seed", "The stitches' jitter")),
        };
        d = withGlobalLight(std::move(d), "light_angle", LightRelation::From);
        d.gizmos = {{GizmoKind::Radius, "offset", nullptr}, {GizmoKind::Light, "light_angle", nullptr}};
        d.reach = [](const ParamValues& p) { return (float)std::max(0.0, p.number("offset")) + stitchAcross(p); };
        d.gather = [](const ParamValues&) { return (float)(STITCH_MARCH_STEPS * 1.5); };
        d.outline = true;
        d.costClass = CostClass::C3;
        d.fallback = FallbackClass::F0;
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"embroidery", "sewing", "thread", "patch", "badge", "seam", "刺繍", "ステッチ"};
        d.inputs.content = true;
        d.inputs.sdf = true;
        d.estimate = estimateStitch;
        d.presetFile = "editor/resources/titleart/fx-presets/stitch.yaml";
        d.apply = applyStitch;
        return d;
    }

}

void titleart::registerEdgeEffects(TitleEffectRegistry& registry) {
    registry.add(brushStroke());
    registry.add(stitch());
}
