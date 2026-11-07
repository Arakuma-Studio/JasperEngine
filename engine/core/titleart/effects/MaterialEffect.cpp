// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Material (docs/specs/S83-title-effects-generators.md §6.6.16), after Pachislop
// src/effects/b/material.ts by SafEight, used with the author's permission: the layer's (group's,
// part's) shape shaded as a 2D title Material over its own colours or the material's fill, in two
// passes with the renderer's element formulas (titleart/fx/material.glsl). Material dimensions in
// % are of `size` canvas px.

#include "TfxCommon.h"
#include "TfxFamilies.h"
#include "TfxPrograms.h"
#include "TitleFx.h"
#include "TitleFxParams.h"

#include "titleart/TitleArtIO.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fxparams;

namespace {

    constexpr double DEG = 3.14159265358979323846 / 180.0;

    const Json& member(const Json& o, const char* key) {
        static const Json empty = Json::object();
        auto it = o.is_object() ? o.find(key) : o.end();
        return it != o.end() && it->is_object() ? *it : empty;
    }
    double num(const Json& o, const char* key, double fallback) {
        auto it = o.is_object() ? o.find(key) : o.end();
        return it != o.end() && it->is_number() ? it->get<double>() : fallback;
    }
    bool on(const Json& o) {
        auto it = o.is_object() ? o.find("enabled") : o.end();
        return it != o.end() && it->is_boolean() && it->get<bool>();
    }
    std::string str(const Json& o, const char* key, const char* fallback) {
        auto it = o.is_object() ? o.find(key) : o.end();
        return it != o.end() && it->is_string() ? it->get<std::string>() : fallback;
    }
    TitleColor col(const Json& o, const char* key, const char* fallback) {
        TitleColor c;
        auto it = o.is_object() ? o.find(key) : o.end();
        if (it == o.end() || !parseTitleColor(*it, c)) parseTitleColor(Json(fallback), c);
        return c;
    }
    int indexOf(const std::string& v, std::initializer_list<const char*> names) {
        int i = 0;
        for (const char* n : names) {
            if (v == n) return i;
            i++;
        }
        return 0;
    }

    // a curve as a 256-entry lookup (its value in red)
    TitleLutData curveLut(const TitleCurve& curve) {
        TitleLutData data;
        data.format = ColorFormat::RGBA;
        data.bytes.assign(TITLE_LUT_SIZE * 4, 255);
        const std::vector<float> samples = curve.sample(TITLE_LUT_SIZE);
        for (int i = 0; i < TITLE_LUT_SIZE; i++) data.bytes[(size_t)i * 4] = (uint8_t)std::lround(std::min(1.0f, std::max(0.0f, samples[(size_t)i])) * 255.0f);
        return data;
    }

    // the light toward the viewer's side: angle (counter-clockwise, comes from) and altitude
    // (degrees above the surface), as a frame vector (y down, z toward the viewer)
    void lightVector(double angle, double altitude, float out[3]) {
        const double a = angle * DEG, e = std::min(89.5, std::max(1.0, altitude)) * DEG;
        out[0] = (float)(std::cos(a) * std::cos(e));
        out[1] = (float)(-std::sin(a) * std::cos(e));
        out[2] = (float)std::sin(e);
    }

    void applyMaterial(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        const Json& mat = p.raw("material");
        const float s = ctx.pxScale;
        const double unit = std::max(1.0, p.number("size")) * s / 100.0;
        const Json& fill = member(mat, "fill");
        const Json& bevel = member(mat, "bevel");
        const Json& finish = member(mat, "finish");
        const Json& satin = member(mat, "satin");
        const Json& texture = member(mat, "texture");
        const Json& glow = member(mat, "inner_glow");
        const Json& cap = member(mat, "gloss");
        const Json& sheen = member(mat, "sheen");
        const Json& pattern = member(mat, "pattern");
        const bool layerColours = p.string("color_mode") != "fill";
        const bool bevelOn = on(bevel);
        // Fit spans the stroke's half-thickness: 12 % of the 100 % size stands in for it (Deviations)
        const double bevelPx = std::max(0.5, (str(bevel, "size_mode", "fixed") == "fit" ? num(bevel, "fit", 70) * 0.12 : num(bevel, "size", 3)) * unit);
        const double glowPx = std::max(0.5, num(glow, "size", 2) * unit);
        const std::string fillKind = str(fill, "kind", "solid");
        const int fillId = indexOf(fillKind, {"solid", "linear", "radial", "angle", "reflected", "diamond", "shape_burst"});
        const bool burst = !layerColours && fillKind == "shape_burst";
        float reach = 2.0f;
        if (bevelOn) reach = std::max(reach, (float)(bevelPx + num(bevel, "soften", 0) * unit) + 3.0f);
        if (on(glow)) reach = std::max(reach, (float)glowPx + 3.0f);
        fx::Scratch field = fx::distanceField(ctx, input, 0.5f, burst ? 0.0f : reach);
        fx::Scratch box = fx::layerBounds(ctx, input);
        fx::Scratch surface = fx::scratch(ctx);
        if (!field || !box || !surface) return;
        const TitleLut* fillRamp = ctx.runner.lut(fx::gradientLut(decodeGradient(fill.value("gradient", Json()))));
        const TitleLut* finishRamp = ctx.runner.lut(fx::gradientLut(decodeGradient(finish.value("gradient", Json()))));
        const std::string glossName = str(bevel, "gloss", "ring");
        const int gloss = indexOf(glossName, {"linear", "ring", "double_ring", "cove", "rounded_steps", "sawtooth", "custom"});
        const TitleLut* glossCurve = gloss == 6 ? ctx.runner.lut(curveLut(decodeCurve(bevel.value("gloss_curve", Json())))) : nullptr;
        if (!fillRamp || !finishRamp || (gloss == 6 && !glossCurve)) return;
        const bool globalLight = bevel.value("use_global_light", true);
        float light[3];
        lightVector(globalLight ? ctx.lightAngle : num(bevel, "light_angle", 120), globalLight ? ctx.lightAltitude : num(bevel, "light_altitude", 30), light);
        const TitleColor fc = col(fill, "color", "#ffffff"), hi = col(bevel, "highlight_color", "#ffffff"), sh = col(bevel, "shadow_color", "#000000");
        const TitleVec2 fillDir = fx::dirClockwise(num(fill, "angle", 90));
        const std::string texKind = str(texture, "kind", "none");
        const int texId = indexOf(texKind, {"none", "noise", "sparkle", "brushed", "ripple", "grunge", "clouds", "hammered", "crumple", "liquid",
                                            "hairline", "spun", "flakes"});
        const bool texSurface = str(texture, "target", "color") == "surface";
        const TitleVec2 texDir = fx::dirClockwise(num(texture, "angle", 0));
        const float texPx = (float)std::max(0.5, num(texture, "scale", 4) * unit);
        const double texSeed = num(texture, "seed", 1) + (double)ctx.seed - 1.0;
        // the texture anchored to the canvas, so it sits still when the layer moves over it
        ctx.usesCanvasPosition = true;
        const TitleVec2 centre = tfx::canvasCentre(ctx);

        TitlePass a = tfx::makePass(ctx, tfx::Program::MaterialSurface, "material surface", *surface);
        a.textures[0].target = &input;
        a.textures[1].lut = glossCurve;
        a.textures[2].lut = fillRamp;
        a.textures[3].lut = finishRamp;
        a.raw[0].target = field.get();
        a.raw[1].target = box.get();
        a.set(2, (float)fillId, (float)num(fill, "scale", 1), (float)num(fill, "offset", 0), layerColours ? 1.0f : 0.0f);
        a.set(3, fc.r, fc.g, fc.b, (float)num(mat, "opacity", 1));
        a.set(4, (float)fillDir.x, (float)fillDir.y, bevelOn ? 1.0f : 0.0f, (float)bevelPx);
        a.set(5, (float)num(bevel, "depth", 3), (float)indexOf(str(bevel, "profile", "smooth"), {"smooth", "button", "mesa", "chamfer", "ridge", "cove", "stepped"}),
              str(bevel, "direction", "up") == "down" ? -1.0f : 1.0f, (float)std::max(1.0, num(bevel, "soften", 0) * unit));
        a.set(6, light[0], light[1], light[2], (float)gloss);
        a.set(7, hi.r, hi.g, hi.b, (float)num(bevel, "highlight_opacity", 0.85));
        a.set(8, sh.r, sh.g, sh.b, (float)num(bevel, "shadow_opacity", 0.6));
        a.set(9, on(finish) ? 1.0f : 0.0f, (float)num(finish, "amount", 1), texSurface ? (float)texId : 0.0f, (float)num(texture, "amount", 0.3));
        a.set(10, texPx, (float)texDir.x, (float)texDir.y, 0.35f * (float)std::min(ctx.width, ctx.height));
        a.set(11, (float)std::fmod(texSeed * 17.31, 97.0), (float)std::fmod(texSeed * 7.77, 89.0));
        if (!ctx.runner.run(a)) return;

        // satin's blurred silhouette (its size is Photoshop's blur radius ~ 2 sigma)
        fx::Scratch blurred;
        if (on(satin)) {
            blurred = fx::blur(ctx, input, (float)(num(satin, "size", 7) * unit * 1.5));
            if (!blurred) return;
        }
        const TitleLut* satinCurve = ctx.runner.lut(curveLut(decodeCurve(satin.value("contour", Json()))));
        if (!satinCurve) return;
        const double satinAngle = (satin.value("use_global_light", false) ? ctx.lightAngle : num(satin, "angle", 19)) * DEG;
        const double satinPx = num(satin, "distance", 5) * unit;
        const TitleColor sc = col(satin, "color", "#000000"), gc = col(glow, "color", "#ffffff"), shc = col(sheen, "color", "#ffffff");
        const TitleColor pc = col(pattern, "color", "#000000"), pc2 = col(pattern, "color2", "#ffffff");
        const TitleVec2 capDir = fx::dirClockwise(90.0 + num(cap, "angle", 0));
        const int patKind = fx::patternKindId(str(pattern, "kind", "none"));
        const double patPx = std::max(0.5, num(pattern, "scale", 12) * unit);
        const double th = num(pattern, "angle", 0) * DEG;
        const double cs = std::cos(th) / patPx, sn = std::sin(th) / patPx;
        TitlePass b = tfx::makePass(ctx, tfx::Program::MaterialDetail, "material details", output);
        b.textures[0].target = surface.get();
        b.textures[1].target = blurred ? blurred.get() : &input;
        b.textures[2].target = &input;
        b.textures[3].lut = satinCurve;
        b.raw[0].target = field.get();
        b.raw[1].target = box.get();
        b.set(2, on(satin) ? 1.0f : 0.0f, (float)num(satin, "opacity", 0.5), (float)indexOf(str(satin, "blend", "multiply"), {"normal", "multiply", "screen", "overlay"}),
              satin.value("invert", true) ? 1.0f : 0.0f);
        b.set(3, sc.r, sc.g, sc.b);
        b.set(4, (float)(std::cos(satinAngle) * satinPx), (float)(-std::sin(satinAngle) * satinPx), on(glow) ? 1.0f : 0.0f, (float)glowPx);
        b.set(5, gc.r, gc.g, gc.b, (float)num(glow, "opacity", 0.6));
        b.set(6, (float)num(glow, "noise", 0), on(cap) ? 1.0f : 0.0f, (float)num(cap, "opacity", 0.55), (float)num(cap, "position", 0.45));
        b.set(7, (float)num(cap, "softness", 0.15), (float)num(cap, "curve", 0.25), (float)capDir.x, (float)capDir.y);
        b.set(8, on(sheen) ? 1.0f : 0.0f, (float)num(sheen, "amount", 0.7), (float)num(sheen, "position", -0.25), (float)num(sheen, "width", 0.35));
        b.set(9, shc.r, shc.g, shc.b, texSurface ? 0.0f : (float)texId);
        b.set(10, (float)num(texture, "amount", 0.3), texPx, (float)indexOf(str(texture, "blend", "overlay"), {"overlay", "soft_light", "screen", "multiply"}),
              (float)(patKind == 15 ? 0 : patKind));
        b.set(11, (float)num(pattern, "opacity", 1), (float)num(pattern, "shade", 0.6), (float)(num(pattern, "seed", 1) + (double)ctx.seed - 1.0),
              (float)std::fmod(texSeed * 17.31, 97.0));
        // the motif map: canvas px (y up) from the canvas centre, turned by the pattern's angle
        b.set(12, (float)cs, (float)-sn, (float)-sn, (float)-cs);
        b.set(13, pc.r, pc.g, pc.b, (float)(-cs * centre.x + sn * centre.y));
        b.set(14, pc2.r, pc2.g, pc2.b, (float)(sn * centre.x + cs * centre.y));
        b.set(15, (float)texDir.x, (float)texDir.y);
        ctx.runner.run(b);
    }

    FxCost estimateMaterial(const ParamValues& p, const FxSize& size, FxTier) {
        tfx::CostBuilder b;
        const Json& mat = p.raw("material");
        b.field().box();
        // the surface: the bevel's slope reads the smoothed distance at four points
        b.pass(1.0, on(member(mat, "bevel")) ? 20 : 4);
        if (on(member(mat, "satin"))) b.blur(num(member(mat, "satin"), "size", 7) * std::max(1.0, p.number("size")) * size.pxScale / 100.0 * 1.5);
        return b.pass(1.0, 8).cost;
    }

    // Pachislop's BEVEL(): a lit, rounded bevel over the layer's own colours
    Json bevelMaterial() {
        return Json{{"fill", {{"kind", "solid"}, {"color", "#ffffff"}}},
                    {"bevel", {{"enabled", true}, {"profile", "smooth"}, {"size", 8}, {"depth", 3}, {"soften", 1}, {"gloss", "linear"},
                               {"highlight_opacity", 0.85}, {"shadow_opacity", 0.45}}}};
    }

    EffectDef material() {
        EffectDef d;
        d.type = "material";
        d.label = "Material";
        d.category = "Texture & Material";
        d.description = "Bevel, chrome, metal grain or emboss on the shape: a title Material (fill, bevel and emboss, finish, pattern, satin, "
                        "texture, sheen, inner glow, gloss) over the layer's own colours or the material's fill";
        ParamDef m;
        m.key = "material";
        m.label = "Material";
        m.kind = ParamKind::Material;
        m.defaultValue = bevelMaterial();
        m.description = "The material: every Material member but reflection, glass and refraction (those need the title renderer's maps)";
        m.anim = FxAnim::Rerender;
        d.params = {
            choice("color_mode", "Color", "layer", {{"layer", "Layer Colors"}, {"fill", "Material Fill"}}, "Shade the layer's own colours, or the material's fill"),
            px("size", "Size", 200, 10, 2000, "The length the material's percentages are of"),
            m,
        };
        d.gather = [](const ParamValues& p) { return (float)(std::max(1.0, p.number("size")) * 0.25); };
        d.costClass = CostClass::C3;
        d.fallback = FallbackClass::F1;
        d.glyphSafety = GlyphSafety::Safe;
        d.aliases = {"bevel", "emboss", "deboss", "chrome", "metal", "gold", "bevel and emboss", "ベベル", "エンボス", "金属"};
        d.inputs.sdf = true;
        d.estimate = estimateMaterial;
        d.presetFile = "editor/resources/titleart/fx-presets/material.yaml";
        d.apply = applyMaterial;
        return d;
    }

}

void titleart::registerMaterialEffect(TitleEffectRegistry& registry) {
    registry.add(material());
}
