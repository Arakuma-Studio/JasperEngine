// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Test-only effect definitions the renderer's GPU tests stack on layers, written against the
// effect context (TitleEffectContext.h) the way the registry's own definitions are: a blur, a
// drop shadow (behind the ink), a glow that adds light, an outline that traces the ink, and the
// adjust effect's operations. Their types start with `test_`, so they never collide with the
// registry's catalogue; registered once per process.

#pragma once

#include "titleart/TitleEffectRegistry.h"
#include "titleart/render/TitleEffectContext.h"

#include <cmath>

namespace jasperengine::titleart::test {

    inline void copyInto(TitlePassContext& ctx, const TargetRef& src, TargetRef& dst, float dx, float dy, float opacity, BlendMode blend) {
        TitleGpuPass p;
        p.program = TitleRenderProgram::Copy;
        p.target = dst.get();
        p.tex[0] = TitlePassContext::texture(src);
        const float w = (float)dst.key().width, h = (float)dst.key().height;
        p.params.set(2, 1.0f / w, 1.0f / h, -dx / w, -dy / h);
        p.params.set(3, opacity, 0.0f, 1.0f, 0.0f);
        p.blend = blend;
        ctx.run(p);
    }

    // the silhouette blurred and recoloured (a gradient map of one colour keeps the alpha)
    inline bool tinted(TitlePassContext& ctx, const TargetRef& src, float radius, const char* color, TargetRef& out) {
        TargetRef blurred;
        if (!ctx.blur(src, radius, blurred)) return false;
        const Json adjustment{{"type", "gradient_map"},
                              {"gradient", {{"stops", Json::array({{{"pos", 0}, {"color", color}, {"alpha", 1}, {"mid", 0.5}},
                                                                   {{"pos", 1}, {"color", color}, {"alpha", 1}, {"mid", 0.5}}})},
                                            {"interp", "rgb"}}},
                              {"reverse", false}};
        return ctx.adjust(blurred, adjustment, out);
    }

    inline void testBlur(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        ctx.blur(input, (float)(p.number("radius", 8) * ctx.pxScale()), output);
    }

    inline void testShadow(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        TargetRef shadow;
        if (!tinted(ctx, *ctx.content, (float)(p.number("size", 6) * ctx.pxScale()), "#000000", shadow)) return;
        // the direction it falls, clockwise on screen: rows down in the node's target
        const double a = p.number("angle", 60) * 3.14159265358979323846 / 180.0;
        const double d = p.number("distance", 10) * ctx.pxScale();
        ctx.clear(output);
        copyInto(ctx, shadow, output, (float)(std::cos(a) * d), (float)(std::sin(a) * d), (float)p.number("opacity", 0.75), BlendMode::NONE);
        copyInto(ctx, input, output, 0.0f, 0.0f, 1.0f, BlendMode::PREMULTIPLIED);
    }

    inline void testGlow(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        TargetRef glow;
        if (!tinted(ctx, *ctx.content, (float)(p.number("size", 16) * ctx.pxScale()), "#ffb000", glow)) return;
        ctx.copy(input, output);
        copyInto(ctx, glow, output, 0.0f, 0.0f, (float)p.number("intensity", 1), BlendMode::LIGHT_PREMULTIPLIED);
    }

    inline void testOutline(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        TargetRef line;
        if (!tinted(ctx, *ctx.inputInk, (float)(p.number("width", 3) * ctx.pxScale()), "#202020", line)) return;
        ctx.clear(output);
        copyInto(ctx, line, output, 0.0f, 0.0f, 1.0f, BlendMode::NONE);
        copyInto(ctx, input, output, 0.0f, 0.0f, 1.0f, BlendMode::PREMULTIPLIED);
    }

    inline void testAdjust(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        ctx.adjust(input, p.raw("adjustment"), output);
    }

    inline ParamDef numberParam(const char* key, double def, double lo, double hi, ParamUnit unit = ParamUnit::Px) {
        ParamDef d;
        d.key = key;
        d.label = key;
        d.kind = ParamKind::Number;
        d.unit = unit;
        d.min = lo;
        d.max = hi;
        d.defaultValue = def;
        return d;
    }

    inline void ensureTestEffects() {
        TitleEffectRegistry& r = TitleEffectRegistry::get();
        if (r.effect("test_blur")) return;
        {
            EffectDef d;
            d.type = "test_blur";
            d.label = "Test Blur";
            d.params = {numberParam("radius", 8, 0, 500)};
            d.reach = [](const ParamValues& p) { return (float)p.number("radius", 8); };
            d.apply = testBlur;
            r.add(d);
        }
        {
            EffectDef d;
            d.type = "test_shadow";
            d.label = "Test Shadow";
            d.params = {numberParam("angle", 60, -360, 360, ParamUnit::Deg), numberParam("distance", 10, 0, 500), numberParam("size", 6, 0, 300),
                        numberParam("opacity", 0.75, 0, 1, ParamUnit::None)};
            d.reach = [](const ParamValues& p) { return (float)(p.number("distance", 10) + p.number("size", 6)); };
            d.apply = testShadow;
            r.add(d);
        }
        {
            EffectDef d;
            d.type = "test_glow";
            d.label = "Test Glow";
            d.params = {numberParam("size", 16, 1, 400), numberParam("intensity", 1, 0, 6, ParamUnit::None)};
            d.reach = [](const ParamValues& p) { return (float)p.number("size", 16); };
            d.addsLight = [](const ParamValues&) { return true; };
            d.apply = testGlow;
            r.add(d);
        }
        {
            EffectDef d;
            d.type = "test_outline";
            d.label = "Test Outline";
            d.params = {numberParam("width", 3, 0.5, 100)};
            d.reach = [](const ParamValues& p) { return (float)p.number("width", 3); };
            d.outline = true;
            d.apply = testOutline;
            r.add(d);
        }
        {
            EffectDef d;
            d.type = "test_adjust";
            d.label = "Test Adjust";
            ParamDef a;
            a.key = "adjustment";
            a.label = "Adjustment";
            a.kind = ParamKind::Adjustment;
            a.defaultValue = Json{{"type", "invert"}};
            d.params = {a};
            d.apply = testAdjust;
            r.add(d);
        }
    }

    inline Json effect(const char* id, const char* type, Json params = Json::object()) {
        return Json{{"id", id}, {"type", type}, {"enabled", true}, {"params", params}};
    }

}
