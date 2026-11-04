// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Pattern Overlay (docs/specs/S80-title-art-core.md §6.5): new, Photoshop's Pattern Overlay with
// the Material pattern kinds of titleart/patterns.glsl (ported from Pachislop
// src/engine/patterns.ts, whose patternMatrix this file's motif map follows, by SafEight, used
// with the author's permission).

#include "TitleFx.h"
#include "TitleFxParams.h"
#include "TitleFxFamilies.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fxparams;

namespace {

    // target px -> motif units: one motif per `scale` canvas px from the anchor, the y-up layer
    // space of the pattern turned clockwise on screen by `angle`; a custom image keeps its aspect
    TitleAffine motifMap(const TitleVec2& anchor, double pxScale, double scale, double angleDeg, double aspect) {
        const double k = 1.0 / std::max(1e-3, scale) / std::max(1e-6, pxScale);
        const double a = angleDeg * 3.14159265358979323846 / 180.0;
        const double c = std::cos(a), s = std::sin(a);
        // pattern space (x, y up) = ((fx - ox) / s, (oy - fy) / s); motif = R(angle) . it
        TitleAffine m;
        m.a = c * k;
        m.b = s * k;
        m.c = s * k * aspect;
        m.d = -c * k * aspect;
        m.tx = -(m.a * anchor.x + m.b * anchor.y);
        m.ty = -(m.c * anchor.x + m.d * anchor.y);
        return m;
    }

    void applyPatternOverlay(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        int kind = fx::patternKindId(p.string("pattern"));
        double aspect = 1.0;
        const TextureRender* image = nullptr;
        if (kind == fx::patternKindId("custom")) {
            int w = 0, h = 0;
            image = ctx.image ? ctx.image(p.asset("image"), w, h) : nullptr;
            if (image && w > 0 && h > 0) aspect = (double)w / (double)h;
            else kind = 0;
        }
        const TitleVec2 anchor = p.string("align") == "canvas" ? ctx.documentToTarget.apply(TitleVec2{0.0, 0.0}) : ctx.layerOrigin;
        const TitleAffine m = motifMap(anchor, ctx.pxScale, p.number("scale"), p.number("angle"), aspect);
        TitlePass pass = fx::makePass(ctx, TitleProgram::PatternOverlay, "pattern_overlay", output);
        pass.textures[0].target = &input;
        pass.textures[1].texture = image;
        const TitleColor c = p.color("color"), c2 = p.color("color2");
        // the document seed and the element seed together (R17); the hash stays exact in fp32
        const int seed = (int)((((int64_t)ctx.seed - 1 + p.integer("seed")) % 65536 + 65536) % 65536);
        pass.set(2, (float)m.a, (float)m.b, (float)m.c, (float)m.d);
        pass.set(3, (float)m.tx, (float)m.ty, (float)kind, (float)seed);
        pass.set(4, c.r, c.g, c.b, (float)p.number("opacity"));
        TitleBlend blend = TitleBlend::Normal;
        titleBlendFromName(p.string("mode"), blend);
        pass.set(5, c2.r, c2.g, c2.b, (float)(int)blend);
        ctx.runner.run(pass);
    }

    EffectDef patternOverlay() {
        EffectDef d;
        d.type = "pattern_overlay";
        d.label = "Pattern Overlay";
        d.category = "Texture & Material";
        d.description = "Lays a procedural pattern (zebra, leopard, checker, 和柄...) or a tiled image over the layer in a blend mode, keeping "
                        "its alpha (Photoshop's Pattern Overlay)";
        std::vector<EnumOption> kinds = patternOptions();
        kinds.push_back({"custom", "Custom Image"});
        d.params = {
            choice("pattern", "Pattern", "stripes", kinds, "The pattern: a procedural kind, or a custom image tiled once per motif"),
            color("color", "Color", "#000000", "The marks' colour"),
            color("color2", "Color 2", "#ffffff", "The secondary colour of the kinds that have one (checker, hazard, leopard, sakura)"),
            px("scale", "Scale", 40, 2, 2000, "The size of one motif"),
            degrees("angle", "Angle", 0, "The pattern's rotation, clockwise"),
            seed("seed", "Seed", "Reshuffles the random kinds (the document's seed adds to it)"),
            when(asset("image", "Image", "The image a custom pattern tiles"), "pattern", {"custom"}),
            number("opacity", "Opacity", 1, 0, 1, ParamUnit::None, "How much of the pattern covers the layer"),
            choice("mode", "Blend Mode", "normal", blendOptions(), "How the pattern combines with the layer's colour"),
            choice("align", "Align", "layer", {{"layer", "Layer"}, {"canvas", "Canvas"}},
                   "Anchor the pattern at the layer's position (it moves with the layer) or at the canvas centre"),
        };
        d.gather = [](const ParamValues&) { return 0.0f; };
        d.costClass = CostClass::C0;
        d.fallback = FallbackClass::F0;
        d.glyphSafety = GlyphSafety::Safe;
        d.aliases = {"pattern", "texture overlay", "柄", "パターンオーバーレイ"};
        d.apply = applyPatternOverlay;
        return d;
    }

}

void titleart::registerTextureEffects(TitleEffectRegistry& registry) {
    registry.add(patternOverlay());
}
