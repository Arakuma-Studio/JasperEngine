// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Shatter (docs/specs/S83-title-effects-generators.md §6.6.17), after Pachislop
// src/effects/b/shatter.ts by SafEight, used with the author's permission; the pieces are drawn by
// one fragment pass (titleart/fx/shatter.glsl) instead of a CPU layout and a mesh.

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

    // how far the pieces may move: the push with its sideways scatter, the fall, and a turned
    // piece's corner
    float shatterReach(const ParamValues& p) {
        if (p.flag("cracks_only")) return 0.0f;
        const double progress = std::max(0.0, std::min(1.0, p.number("progress")));
        return (float)(1.45 * p.number("strength") * progress + p.number("fall") * progress * progress +
                       0.5 * p.number("radius") * std::min(1.0, p.number("rotation") * DEG) * progress);
    }

    void applyShatter(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        fx::Scratch box = fx::layerBounds(ctx, ctx.content ? *ctx.content : input);
        if (!box) return;
        // the impact and the layout box are canvas positions
        ctx.usesCanvasPosition = true;
        const TitleVec2 centre = tfx::canvasCentre(ctx);
        const TitleVec2 impact = p.vec2("impact");
        const TitleColor edge = fx::linearOf(p.color("edge_color"));
        TitlePass pass = tfx::makePass(ctx, tfx::Program::Shatter, "shatter", output);
        pass.textures[0].target = &input;
        pass.raw[1].target = box.get();
        pass.set(2, (float)optionIndex(p, "pattern", {"glass", "voronoi", "triangles"}), (float)std::min(400, std::max(10, p.integer("pieces"))),
                 (float)impact.x, (float)impact.y);
        pass.set(3, (float)std::max(1e-3, p.number("radius")), (float)p.number("strength"), (float)(p.number("rotation") * DEG), (float)p.number("fall"));
        pass.set(4, (float)std::max(0.0, std::min(1.0, p.number("progress"))), (float)p.number("fade"), (float)std::max(0.0, p.number("gap")),
                 (float)p.number("lost") / 100.0f);
        pass.set(5, edge.r, edge.g, edge.b, (float)p.number("edge_highlight") / 100.0f);
        pass.set(6, tfx::seedOf(ctx, p) * 7.13f + 1.0f, p.flag("cracks_only") ? 1.0f : 0.0f, (float)centre.x, (float)centre.y);
        pass.set(7, 1.5f);
        ctx.runner.run(pass);
    }

    FxCost estimateShatter(const ParamValues& p, const FxSize& size, FxTier) {
        tfx::CostBuilder b;
        b.box();
        // the candidate pieces a pixel tests: the cells within the pieces' reach, each a
        // displacement, a rest mapping and a membership test (a Voronoi cell's 24 neighbours),
        // counted as taps: the pass is bound by that arithmetic, not by its few texture reads
        const int pieces = std::min(400, std::max(10, p.integer("pieces")));
        const double area = std::max(1.0, (double)size.width * size.height) / (size.pxScale * size.pxScale);
        const double cell = std::sqrt(area / pieces);
        const double range = std::min(21.0, 2.0 * std::ceil(shatterReach(p) / std::max(cell, 1.0) + 1.0) + 1.0);
        const int candidates = (int)std::min((double)pieces, range * range);
        const int perCandidate = p.string("pattern") == "voronoi" ? 8 : 3;
        return b.pass(1.0, candidates * perCandidate).cost;
    }

    EffectDef shatter() {
        using namespace tfx;
        EffectDef d;
        d.type = "shatter";
        d.label = "Shatter";
        d.category = "Distort";
        d.description = "Breaks the layer into glass shards, Voronoi cells or triangles and blasts the pieces near the impact outward, turning, "
                        "falling and fading, each cut edge catching a highlight; Progress animates it from assembled (0) to blown (1)";
        d.params = {
            relayout(choice("pattern", "Pattern", "glass", {{"glass", "Glass"}, {"voronoi", "Voronoi"}, {"triangles", "Triangles"}}, "How the layer breaks")),
            relayout(integer("pieces", "Pieces", 60, 10, 400, "About how many pieces")),
            relayout(vec2("impact", "Impact", 0, 0, "Where it breaks, canvas px from the canvas centre")),
            relayout(px("radius", "Radius", 300, 0, 4000, "How far from the impact the pieces fly")),
            live(number("progress", "Progress", 1, 0, 1, ParamUnit::None, "0 assembled, 1 blown apart: animate it")),
            live(px("strength", "Strength", 80, 0, 1000, "How far the pieces are thrown")),
            live(degrees("rotation", "Rotation", 25, "How much the pieces turn at most", 0, 180)),
            live(px("fall", "Fall", 0, 0, 4000, "How far the pieces drop as they fly")),
            live(number("fade", "Fade", 0, 0, 1, ParamUnit::None, "Pieces fade as they move")),
            relayout(px("gap", "Gap", 1, 0, 40, "The cracks' width", 0.5)),
            color("edge_color", "Edge Color", "#ffffff", "The cut edges' highlight colour", true),
            number("edge_highlight", "Edge Highlight", 100, 0, 300, ParamUnit::None, "How bright the cut edges are, in percent", 1),
            relayout(number("lost", "Lost", 10, 0, 100, ParamUnit::None, "The share of pieces near the impact blown away, in percent", 1)),
            flag("cracks_only", "Cracks Only", false, "Keeps every piece in place: only the cracks show"),
            relayout(seed("seed", "Seed", "The break's random layout")),
        };
        d.gizmos = {{GizmoKind::Point, "impact", nullptr}, {GizmoKind::Radius, "radius", nullptr}};
        d.reach = [](const ParamValues& p) { return shatterReach(p); };
        d.gather = [](const ParamValues&) { return -1.0f; };
        // C3, not §6.6.17's C1: the pieces are found per pixel instead of drawn as a mesh (Deviations)
        d.costClass = CostClass::C3;
        d.fallback = FallbackClass::F0;
        d.live = {LiveKind::Kernel, {"progress", "strength", "rotation", "fall", "fade"}, (uint32_t)Program::Shatter};
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"break", "broken glass", "shards", "explode", "smash", "ガラス割れ", "破砕"};
        d.inputs.content = true;
        d.estimate = estimateShatter;
        d.presetFile = "editor/resources/titleart/fx-presets/shatter.yaml";
        d.apply = applyShatter;
        return d;
    }

}

void titleart::registerShatterEffect(TitleEffectRegistry& registry) {
    registry.add(shatter());
}
