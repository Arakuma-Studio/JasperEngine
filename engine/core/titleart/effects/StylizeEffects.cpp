// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Halftone and Grain (docs/specs/S83-title-effects-generators.md §6.6.9, §6.6.14), ported from
// Pachislop src/effects/a/stylize.ts (halftone) and src/effects/a/finish.ts (grain) by SafEight,
// used with the author's permission; Scanlines, Pixelate and Holographic Foil (§6.6.20, §6.6.21,
// §6.6.27) are S83's designs.

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

    int ceilDiv(int v, int d) {
        return std::max(1, (v + d - 1) / d);
    }

    // the TitleBlend number of a blend-mode param (0 Normal when unknown)
    int blendIndex(const ParamValues& p, const char* key) {
        const std::string v = p.string(key);
        for (int i = 0; i < TITLE_BLEND_COUNT; i++) {
            if (v == titleBlendName((TitleBlend)i)) return i;
        }
        return 0;
    }

    // ---- Halftone ----

    void applyHalftone(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const int source = optionIndex(p, "source", {"ramp", "luminance", "uniform", "alpha"});
        fx::Scratch box, soft;
        if (source == 0) {
            box = fx::layerBounds(ctx, input);
            if (!box) return;
        }
        if (source == 3) {
            // Dissolve: the alpha softened over Fade (radius ~ 3 sigma) sets the screen's density
            soft = fx::blur(ctx, input, (float)std::max(0.0, p.number("fade")) * ctx.pxScale);
            if (!soft) return;
        }
        // the screen sits on the canvas
        ctx.usesCanvasPosition = true;
        const TitleVec2 centre = tfx::canvasCentre(ctx);
        const TitleVec2 ramp = fx::dirClockwise(p.number("ramp_angle"));
        const TitleColor dot = p.color("color");
        TitlePass pass = tfx::makePass(ctx, tfx::Program::Halftone, "halftone", output);
        pass.textures[0].target = &input;
        pass.textures[1].target = soft.get();
        pass.raw[1].target = box.get();
        pass.set(2, (float)std::max(1.0, p.number("size")), (float)(p.number("angle") * DEG), (float)p.number("dot_scale"), (float)p.number("amount"));
        pass.set(3, dot.r, dot.g, dot.b, (float)source);
        pass.set(4, (float)ramp.x, (float)ramp.y, (float)blendIndex(p, "mode"),
                 (float)optionIndex(p, "dot", {"round", "square", "diamond", "line", "cross"}));
        pass.set(5, (float)centre.x, (float)centre.y);
        ctx.runner.run(pass);
    }

    FxCost estimateHalftone(const ParamValues& p, const FxSize& size, FxTier) {
        tfx::CostBuilder b;
        const std::string source = p.string("source");
        if (source == "ramp") b.box();
        if (source == "alpha") b.blur(std::max(0.0, p.number("fade")) * size.pxScale);
        return b.pass(1.0, 2).cost;
    }

    EffectDef halftone() {
        using namespace tfx;
        EffectDef d;
        d.type = "halftone";
        d.label = "Halftone";
        d.category = "Stylize";
        d.description = "A comic dot screen inside the shape (round, square, diamond, line or cross dots), its dot size from a ramp across the "
                        "layer, its darkness or uniform; Dissolve breaks the layer itself into a dot screen toward its soft parts";
        const std::vector<const char*> recolour = {"ramp", "luminance", "uniform"};
        d.params = {
            px("size", "Cell Size", 9, 2, 120, "The screen's cell size", 0.5),
            degrees("angle", "Screen Angle", 45, "Turns the screen"),
            choice("dot", "Dot", "round", {{"round", "Round"}, {"square", "Square"}, {"diamond", "Diamond"}, {"line", "Line"}, {"cross", "Cross"}},
                   "The dots' shape"),
            when(live(number("dot_scale", "Dot Scale", 0.8, 0, 2, ParamUnit::None, "Scales every dot")), "source", recolour),
            when(color("color", "Dot Color", "#8a1c00", "The dots' colour"), "source", recolour),
            when(number("amount", "Amount", 0.6, 0, 1, ParamUnit::None, "How strongly the dots recolour the layer"), "source", recolour),
            choice("source", "Dot Size From", "ramp",
                   {{"ramp", "Ramp"}, {"luminance", "Darkness"}, {"uniform", "Uniform"}, {"alpha", "Transparency (Dissolve)"}},
                   "What sets each dot's size"),
            when(live(px("fade", "Fade", 28, 0, 400, "How far in from the edge Dissolve reaches")), "source", {"alpha"}),
            when(degrees("ramp_angle", "Ramp Angle", 90, "The ramp's direction across the layer, clockwise"), "source", {"ramp"}),
            when(choice("mode", "Blend", "multiply", blendOptions(), "How the dots blend over the layer"), "source", recolour),
        };
        d.gizmos = {{GizmoKind::Direction, "ramp_angle", nullptr}};
        d.gather = [](const ParamValues& p) { return p.string("source") == "ramp" ? -1.0f : 1.5f * (float)p.number("fade"); };
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F0;
        d.live = {LiveKind::Kernel, {"dot_scale", "fade"}, (uint32_t)Program::Halftone};
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"comic dots", "ben-day dots", "screen tone", "dot screen", "dissolve", "網点", "トーン", "スクリーントーン"};
        d.estimate = estimateHalftone;
        d.presetFile = "editor/resources/titleart/fx-presets/halftone.yaml";
        d.apply = applyHalftone;
        return d;
    }

    // ---- Grain ----

    void applyGrain(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        ctx.usesCanvasPosition = true;
        const TitleVec2 centre = tfx::canvasCentre(ctx);
        TitlePass pass = tfx::makePass(ctx, tfx::Program::Grain, "grain", output);
        pass.textures[0].target = &input;
        pass.set(2, (float)p.number("amount"), (float)std::max(0.05, p.number("size")), (float)std::fmod(tfx::seedOf(ctx, p), 997.0f) * 3.71f,
                 p.flag("color") ? 1.0f : 0.0f);
        pass.set(3, (float)centre.x, (float)centre.y);
        ctx.runner.run(pass);
    }

    EffectDef grain() {
        using namespace tfx;
        EffectDef d;
        d.type = "grain";
        d.label = "Grain";
        d.category = "Color";
        d.description = "Film grain over the layer, monochrome or colour, strongest in the midtones; transparency is left untouched";
        d.params = {
            live(number("amount", "Amount", 0.14, 0, 1, ParamUnit::None, "How strong the grain is")),
            px("size", "Size", 1.2, 0.3, 20, "The grain's size", 0.1),
            flag("color", "Color Grain", false, "Coloured grain instead of grey"),
            liveStepped(seed("seed", "Seed", "The grain's random pattern; reseed every frame for moving film grain")),
        };
        d.gather = [](const ParamValues&) { return 0.0f; };
        d.costClass = CostClass::C0;
        d.fallback = FallbackClass::F0;
        d.live = {LiveKind::Kernel, {"amount", "seed"}, (uint32_t)Program::Grain};
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"film grain", "noise", "grit", "粒子", "ノイズ"};
        d.estimate = [](const ParamValues&, const FxSize&, FxTier) {
            tfx::CostBuilder b;
            return b.pass(1.0, 1).cost;
        };
        d.presetFile = "editor/resources/titleart/fx-presets/grain.yaml";
        d.apply = applyGrain;
        return d;
    }

    // ---- Scanlines ----

    void applyScanlines(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        ctx.usesCanvasPosition = true;
        const TitleVec2 centre = tfx::canvasCentre(ctx);
        // the lines run along the angle; their positions are measured across it
        const TitleVec2 along = fx::dirClockwise(p.number("angle"));
        TitlePass pass = tfx::makePass(ctx, tfx::Program::Scanlines, "scanlines", output);
        pass.textures[0].target = &input;
        pass.set(2, (float)p.number("spacing"), (float)p.number("thickness"), (float)p.number("intensity"),
                 (float)optionIndex(p, "style", {"lines", "crt", "interlace", "lcd"}));
        pass.set(3, (float)-along.y, (float)along.x, (float)p.number("offset"), (float)p.number("mask_strength"));
        pass.set(4, (float)p.number("brightness"), (float)p.number("flicker"), tfx::seedOf(ctx, p));
        pass.set(5, (float)centre.x, (float)centre.y);
        ctx.runner.run(pass);
    }

    EffectDef scanlines() {
        using namespace tfx;
        EffectDef d;
        d.type = "scanlines";
        d.label = "Scanlines";
        d.category = "Stylize";
        d.description = "Retro screen lines over the layer: soft scanlines, a CRT with an aperture grille, interlaced lines or LCD sub-pixels";
        d.params = {
            choice("style", "Style", "lines", {{"lines", "Lines"}, {"crt", "CRT"}, {"interlace", "Interlace"}, {"lcd", "LCD"}}, "The screen's look"),
            px("spacing", "Spacing", 4, 1, 64, "The distance between two lines", 0.5),
            number("thickness", "Thickness", 0.5, 0.05, 0.95, ParamUnit::None, "The share of a period the dark line takes"),
            live(number("intensity", "Intensity", 0.45, 0, 1, ParamUnit::None, "How dark the lines get")),
            degrees("angle", "Angle", 0, "The lines' direction, clockwise (0 = horizontal)", -90, 90),
            when(number("mask_strength", "Mask Strength", 0.5, 0, 1, ParamUnit::None, "How strong the RGB stripes are"), "style", {"crt", "lcd"}),
            number("brightness", "Brightness", 1.15, 0.5, 2, ParamUnit::Factor, "Brightens the layer to make up for the lines"),
            live(px("offset", "Offset", 0, -4000, 4000, "Scrolls the lines", 0.5)),
            number("flicker", "Flicker", 0, 0, 1, ParamUnit::None, "Dims the screen by a random amount; reseed to flicker"),
            liveStepped(seed("seed", "Seed", "The flicker's random amount")),
        };
        d.gather = [](const ParamValues&) { return 0.0f; };
        d.costClass = CostClass::C0;
        d.fallback = FallbackClass::F0;
        d.live = {LiveKind::Kernel, {"intensity", "offset", "seed"}, (uint32_t)Program::Scanlines};
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"crt", "lcd", "interlace", "retro screen", "tv lines", "走査線", "スキャンライン"};
        d.estimate = [](const ParamValues&, const FxSize&, FxTier) {
            tfx::CostBuilder b;
            return b.pass(1.0, 1).cost;
        };
        d.presetFile = "editor/resources/titleart/fx-presets/scanlines.yaml";
        d.apply = applyScanlines;
        return d;
    }

    // ---- Pixelate ----

    void applyPixelate(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float cell = std::max(1.0f, (float)p.number("size") * ctx.pxScale);
        // the grid's origin: a cell corner on the canvas centre or on the layer's origin, moved to
        // the first corner at or before the target's
        const bool canvas = p.string("align") != "layer";
        if (canvas) ctx.usesCanvasPosition = true;
        const TitleVec2 anchor = canvas ? tfx::canvasCentre(ctx) : ctx.layerOrigin;
        const float ox = (float)(anchor.x - std::ceil(anchor.x / cell) * cell);
        const float oy = (float)(anchor.y - std::ceil(anchor.y / cell) * cell);
        const int gw = (int)std::ceil((ctx.width - ox) / cell), gh = (int)std::ceil((ctx.height - oy) / cell);
        // cells wider than 12 px sample a downsampled copy, 12 taps a side
        fx::Scratch levels[9];
        const TargetRef* source = &input;
        int lw = ctx.width, lh = ctx.height;
        for (int l = 1; cell / std::ldexp(1.0f, l - 1) > 12.0f && l <= 8; l++) {
            lw = ceilDiv(lw, 2);
            lh = ceilDiv(lh, 2);
            levels[l] = fx::scratch(ctx, lw, lh);
            if (!levels[l] || !fx::copy(ctx, *source, *levels[l])) return;
            source = levels[l].get();
        }
        const TitleAssetRef paletteRef = p.asset("palette");
        int pw = 0, ph = 0;
        const TextureRender* palette = !paletteRef.empty() && ctx.image ? ctx.image(paletteRef, pw, ph) : nullptr;
        const int paletteCount = palette ? std::min(256, pw * ph) : 0;
        fx::Scratch cells = fx::scratch(ctx, std::max(1, gw), std::max(1, gh));
        if (!cells) return;
        TitlePass down = tfx::makePass(ctx, tfx::Program::PixelDown, "pixelate cells", *cells);
        down.textures[0].target = source;
        down.textures[2].texture = palette;
        down.set(2, cell, ox, oy, (float)std::min(12, std::max(1, (int)std::ceil(cell))));
        down.set(3, (float)ctx.width, (float)ctx.height, (float)p.integer("levels"), (float)optionIndex(p, "dither", {"none", "bayer2", "bayer4", "bayer8"}));
        down.set(4, (float)p.number("dither_amount"), p.string("alpha_mode") == "soft" ? 1.0f : 0.0f, (float)p.number("alpha_threshold"), (float)paletteCount);
        down.set(5, (float)std::max(1, pw));
        if (!ctx.runner.run(down)) return;
        TitlePass up = tfx::makePass(ctx, tfx::Program::PixelUp, "pixelate", output);
        up.textures[1].target = cells.get();
        up.set(2, cell, ox, oy, (float)optionIndex(p, "shape", {"square", "round", "diamond", "led"}));
        up.set(3, (float)p.number("gap"), (float)gw, (float)gh);
        ctx.runner.run(up);
    }

    FxCost estimatePixelate(const ParamValues& p, const FxSize& size, FxTier) {
        tfx::CostBuilder b;
        const double cell = std::max(1.0, p.number("size") * size.pxScale);
        double frac = 1.0;
        for (int l = 1; cell / std::ldexp(1.0, l - 1) > 12.0 && l <= 8; l++) b.pass(frac /= 4.0, 1);
        const int taps = std::min(12, std::max(1, (int)std::ceil(cell)));
        b.pass(1.0 / (cell * cell), taps * taps + (p.asset("palette").empty() ? 0 : 256));
        return b.pass(1.0, 1).cost;
    }

    EffectDef pixelate() {
        using namespace tfx;
        EffectDef d;
        d.type = "pixelate";
        d.label = "Pixelate";
        d.category = "Stylize";
        d.description = "Big pixels: the layer averaged into cells drawn as squares, discs, diamonds or LED dots, optionally posterized or "
                        "snapped to a palette with an ordered dither (retro 8-bit, LED boards, mosaics)";
        d.params = {
            live(px("size", "Cell Size", 8, 1, 256, "The pixels' size")),
            choice("shape", "Shape", "square", {{"square", "Square"}, {"round", "Round"}, {"diamond", "Diamond"}, {"led", "LED"}}, "Each pixel's shape"),
            number("gap", "Gap", 0, 0, 0.9, ParamUnit::None, "Space between the pixels, a share of a cell"),
            asset("palette", "Palette", "A palette image (one colour per pixel, up to 256): each cell snaps to its nearest colour"),
            integer("levels", "Levels", 0, 0, 64, "Posterize each channel to this many levels when there is no palette (0 = off)"),
            choice("dither", "Dither", "none", {{"none", "None"}, {"bayer2", "Bayer 2x2"}, {"bayer4", "Bayer 4x4"}, {"bayer8", "Bayer 8x8"}},
                   "An ordered dither before the colours snap"),
            number("dither_amount", "Dither Amount", 0.5, 0, 1, ParamUnit::None, "How strong the dither is"),
            choice("alpha_mode", "Alpha", "hard", {{"hard", "Hard"}, {"soft", "Soft"}}, "Cells are fully in or out, or keep their coverage"),
            when(number("alpha_threshold", "Alpha Threshold", 0.5, 0, 1, ParamUnit::None, "The coverage a cell needs to be drawn"), "alpha_mode", {"hard"}),
            choice("align", "Align", "canvas", {{"canvas", "Canvas"}, {"layer", "Layer"}}, "The grid sits on the canvas, or moves with the layer"),
        };
        d.reach = [](const ParamValues& p) { return (float)p.number("size"); };
        d.gather = [](const ParamValues& p) { return (float)p.number("size"); };
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F0;
        d.live = {LiveKind::Kernel, {"size"}, (uint32_t)Program::PixelUp};
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"pixel", "8-bit", "8bit", "mosaic", "led board", "retro", "モザイク", "ドット絵"};
        d.estimate = estimatePixelate;
        d.presetFile = "editor/resources/titleart/fx-presets/pixelate.yaml";
        d.apply = applyPixelate;
        return d;
    }

    // ---- Holographic Foil ----

    void applyHolographic(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const float relief = (float)p.number("relief");
        const float reliefPx = 8.0f * ctx.pxScale;
        fx::Scratch field;
        if (relief > 0.0f) {
            field = fx::distanceField(ctx, ctx.content ? *ctx.content : input, 0.5f, reliefPx + 2.0f);
            if (!field) return;
        }
        ctx.usesCanvasPosition = true;
        const TitleVec2 centre = tfx::canvasCentre(ctx);
        const TitleVec2 tilt = p.vec2("tilt");
        const float halfDiag = (float)(0.5 * std::hypot((double)ctx.frame.width, (double)ctx.frame.height) / std::max(1e-3f, ctx.pxScale));
        TitlePass pass = tfx::makePass(ctx, tfx::Program::Holographic, "holographic", output);
        pass.textures[0].target = &input;
        pass.raw[0].target = field.get();
        pass.set(2, (float)p.number("amount"), (float)tilt.x, (float)-tilt.y, (float)p.number("film_thickness"));
        pass.set(3, (float)p.number("film_ior"), relief, (float)optionIndex(p, "pattern", {"none", "diffraction", "glitter", "stars"}),
                 (float)p.number("pattern_scale"));
        pass.set(4, tfx::seedOf(ctx, p), (float)blendIndex(p, "mode"), reliefPx, halfDiag);
        pass.set(5, (float)centre.x, (float)centre.y);
        ctx.runner.run(pass);
    }

    EffectDef holographic() {
        using namespace tfx;
        EffectDef d;
        d.type = "holographic";
        d.label = "Holographic Foil";
        d.category = "Color";
        d.description = "A holographic foil: thin-film rainbow colours that shift with the view tilt and across the layer, with a diffraction, "
                        "glitter or star micro pattern";
        d.params = {
            number("amount", "Amount", 1, 0, 1, ParamUnit::None, "How strongly the foil covers the layer"),
            live(vec2("tilt", "Tilt", 0, 0, "The simulated view tilt (-1..1 each way): animate it for the shimmer")),
            number("film_thickness", "Film Thickness", 450, 100, 1200, ParamUnit::None, "The film's thickness in nanometres: which colours it shows", 1),
            number("film_ior", "Film IOR", 1.5, 1, 2.5, ParamUnit::None, "The film's refractive index"),
            number("relief", "Relief", 0.5, 0, 1, ParamUnit::None, "A bevel and wobble that shift the colours"),
            choice("pattern", "Pattern", "glitter", {{"none", "None"}, {"diffraction", "Diffraction"}, {"glitter", "Glitter"}, {"stars", "Stars"}},
                   "The foil's micro pattern"),
            px("pattern_scale", "Pattern Scale", 6, 1, 100, "The micro pattern's size", 0.5),
            choice("mode", "Blend", "overlay", blendOptions(), "How the foil blends over the layer"),
            seed("seed", "Seed", "The pattern's random layout"),
        };
        d.gather = [](const ParamValues&) { return 8.0f; };
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F0;
        d.live = {LiveKind::Kernel, {"tilt"}, (uint32_t)Program::Holographic};
        d.glyphSafety = GlyphSafety::Approximate;
        d.aliases = {"hologram", "holo", "foil", "iridescent", "rainbow foil", "ホログラム", "箔"};
        d.inputs.content = true;
        d.inputs.sdf = true;
        d.estimate = [](const ParamValues& p, const FxSize&, FxTier) {
            tfx::CostBuilder b;
            if (p.number("relief") > 0.0) b.field();
            return b.pass(1.0, p.string("pattern") == "glitter" ? 12 : 6).cost;
        };
        d.presetFile = "editor/resources/titleart/fx-presets/holographic.yaml";
        d.apply = applyHolographic;
        return d;
    }

}

void titleart::registerStylizeEffects(TitleEffectRegistry& registry) {
    registry.add(halftone());
    registry.add(scanlines());
    registry.add(pixelate());
    registry.add(grain());
    registry.add(holographic());
}
