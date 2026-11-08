// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Fill and Pattern Fill (docs/specs/S80-title-art-core.md §6.1 Generator, §6.5). Fill replaces
// Pachislop's background layer (src/engine/layer2d/Title2DRenderer.ts BackgroundRenderer and
// shaders.ts FILL_GLSL); Pattern Fill is ported from Pachislop src/generators/pattern.ts
// (patternFill) by SafEight, used with the author's permission.

#include "titleart/effects/TitleFx.h"
#include "titleart/effects/TitleFxFamilies.h"
#include "titleart/effects/TfxCommon.h"
#include "titleart/effects/TitleFxParams.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fxparams;

namespace {

    void setAffine(TitlePass& pass, int index, const TitleAffine& m) {
        pass.set(index, (float)m.a, (float)m.b, (float)m.c, (float)m.d);
    }

    void renderFill(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        const int kind = optionIndex(p, "kind", {"solid", "linear", "radial", "angle", "reflected", "diamond", "shape_burst"});
        const TitleLut* ramp = nullptr;
        if (kind != 0) {
            ramp = ctx.runner.lut(fx::gradientLut(p.gradient("gradient")));
            if (!ramp) return;
            // a gradient spans the canvas, wherever the layer lies
            ctx.usesCanvasPosition = true;
        }
        // target px -> canvas px from its bottom-left, +y up (the space FILL_GLSL works in)
        TitleAffine toCanvas = ctx.documentToTarget.inverse();
        toCanvas.tx += ctx.canvasWidth * 0.5;
        toCanvas.ty += ctx.canvasHeight * 0.5;
        TitlePass pass = fx::makePass(ctx, TitleProgram::Fill, "fill", target);
        pass.textures[0].lut = ramp;
        const TitleColor c = p.color("color");
        // the gradient's axis: clockwise on screen (90 = top to bottom) as a y-up vector
        const double a = p.number("angle") * 3.14159265358979323846 / 180.0;
        setAffine(pass, 2, toCanvas);
        pass.set(3, (float)toCanvas.tx, (float)toCanvas.ty, (float)kind, (float)p.number("scale"));
        pass.set(4, c.r, c.g, c.b, (float)p.number("offset"));
        pass.set(5, (float)std::cos(a), (float)-std::sin(a), (float)ctx.canvasWidth, (float)ctx.canvasHeight);
        ctx.runner.run(pass);
    }

    GeneratorDef fill() {
        GeneratorDef d;
        d.type = "fill";
        d.label = "Fill";
        d.category = "Fill & Pattern";
        d.description = "A solid colour or a gradient covering the canvas: a background for mockups and full-frame looks";
        d.params = {
            choice("kind", "Kind", "solid",
                   {{"solid", "Solid"}, {"linear", "Linear"}, {"radial", "Radial"}, {"angle", "Angle"}, {"reflected", "Reflected"},
                    {"diamond", "Diamond"}, {"shape_burst", "Shape Burst"}},
                   "A solid colour or the gradient's shape over the canvas"),
            when(color("color", "Color", "#202020", "The fill's colour"), "kind", {"solid"}),
            when(gradient("gradient", "Gradient", gradientValue({{0.0, "#000000"}, {1.0, "#ffffff"}}), "The fill's colours"), "kind",
                 {"linear", "radial", "angle", "reflected", "diamond", "shape_burst"}),
            when(degrees("angle", "Angle", 90, "The gradient's direction, clockwise (90 = top to bottom)"), "kind",
                 {"linear", "angle", "reflected"}),
            when(number("scale", "Scale", 1, 0.1, 10, ParamUnit::Factor, "How much of the canvas the gradient spans"), "kind",
                 {"linear", "radial", "angle", "reflected", "diamond", "shape_burst"}),
            when(number("offset", "Offset", 0, -1, 1, ParamUnit::None, "Moves the gradient along itself"), "kind",
                 {"linear", "radial", "angle", "reflected", "diamond", "shape_burst"}),
        };
        d.gizmos = {{GizmoKind::Direction, "angle", nullptr}};
        d.costClass = CostClass::C0;
        d.fallback = FallbackClass::F0;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"background", "solid", "gradient fill", "塗りつぶし", "背景"};
        d.render = renderFill;
        return d;
    }

    void renderPatternFill(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        TitlePass pass = fx::makePass(ctx, TitleProgram::PatternFill, "pattern_fill", target);
        // S83 §6.7.17's scroll: the pattern sampled `offset` back along the layer (y up)
        TitleAffine toLayer = ctx.layerToTarget.inverse();
        const TitleVec2 offset = p.vec2("offset");
        toLayer.tx -= offset.x;
        toLayer.ty -= offset.y;
        const int kind = std::max(1, fx::patternKindId(p.string("kind")));
        // the document seed and the element seed together (R17); the hash stays exact in fp32
        const int seed = (int)((((int64_t)ctx.seed - 1 + p.integer("seed")) % 65536 + 65536) % 65536);
        const TitleColor c = fx::linearOf(p.color("color")), c2 = fx::linearOf(p.color("color2"));
        const TitleColor bg = fx::linearOf(p.color("background"));
        const double tilt = std::min(80.0, std::max(0.0, p.number("tilt"))) * 3.14159265358979323846 / 180.0;
        const double rotation = p.number("rotation") * 3.14159265358979323846 / 180.0;
        setAffine(pass, 2, toLayer);
        pass.set(3, (float)toLayer.tx, (float)toLayer.ty, (float)kind, (float)seed);
        pass.set(4, c.r, c.g, c.b, (float)(p.number("bg_opacity") / 100.0));
        pass.set(5, c2.r, c2.g, c2.b, (float)std::max(1.0, p.number("scale")));
        // a camera about one canvas height away: at 45 degrees the horizon is a canvas height above the origin
        pass.set(6, bg.r, bg.g, bg.b, (float)std::max(1, ctx.canvasHeight));
        pass.set(7, (float)std::sin(tilt), (float)std::cos(tilt), (float)std::cos(rotation), (float)std::sin(rotation));
        ctx.runner.run(pass);
    }

    GeneratorDef patternFill() {
        GeneratorDef d;
        d.type = "pattern_fill";
        d.label = "Pattern Fill";
        d.category = "Fill & Pattern";
        d.description = "A full-frame 柄: zebra, giraffe, leopard, tiger, checker, hazard and 和柄 over a background colour, optionally on a "
                        "floor plane receding to a horizon; fade it with a gradient mask";
        d.params = {
            choice("kind", "Pattern", "zebra", patternOptions(), "The procedural pattern"),
            color("color", "Color", "#121212", "The marks' colour"),
            color("color2", "Color 2", "#ffffff", "The secondary colour of the kinds that have one"),
            color("background", "Background", "#f2efe8", "The colour between the marks"),
            number("bg_opacity", "Background Opacity", 100, 0, 100, ParamUnit::None, "The background's opacity, percent", 1.0),
            px("scale", "Motif Size", 140, 8, 2000, "The size of one motif"),
            degrees("rotation", "Rotation", 0, "The pattern's rotation, clockwise", -180, 180),
            degrees("tilt", "Perspective Tilt", 0, "Lays the pattern on a floor tilted back by this much", 0, 80),
            seed("seed", "Seed", "Reshuffles the random kinds (the document's seed adds to it)"),
            tfx::vec2("offset", "Offset", 0, 0, "Scrolls the pattern (px); animate it for a moving pattern"),
        };
        // S83 §6.7.17: a scroll and a turn play live (one full-canvas pass)
        markLive(d, {"offset", "rotation"}, LiveKind::Kernel, (uint32_t)TitleProgram::PatternFill);
        d.gizmos = {{GizmoKind::Direction, "rotation", nullptr}};
        d.costClass = CostClass::C0;
        d.fallback = FallbackClass::F0;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"patternFill", "pattern", "animal print", "和柄", "柄"};
        d.render = renderPatternFill;
        return d;
    }

}

void titleart::registerFillGenerators(TitleEffectRegistry& registry) {
    registry.add(fill());
    registry.add(patternFill());
}
