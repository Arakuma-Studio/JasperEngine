// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Test-only effect definitions the renderer's GPU tests stack on layers, written against the
// effect context (TitleEffectContext.h) the way the registry's own definitions are, drawing the
// renderer's programs through its TitlePassRunner: a blur, a drop shadow (behind the ink), a glow
// that adds light, an outline that traces the ink, and the adjust effect's operations. Their types start with `test_`, so they never collide with the
// registry's catalogue; registered once per process.

#pragma once

#include "titleart/TitleEffectRegistry.h"
#include "titleart/render/Adjustments.h"
#include "titleart/render/TitleEffectContext.h"
#include "titleart/render/TitleFields.h"
#include "titleart/render/TitleGpu.h"
#include "titleart/render/TitleJobRunner.h"
#include "titleart/render/TitleRenderServices.h"

#include <cmath>
#include <memory>
#include <vector>

namespace jasperengine::titleart::test {

    // a pass of the renderer's own programs (18 on) through the effect runner: what the
    // registry's definitions do with the effect programs (1-17)
    inline bool runRender(EffectContext& ctx, const TitleGpuPass& g, TargetRef& out) {
        TitlePass pass;
        pass.program = (TitleProgram)(uint8_t)g.program;
        pass.label = "test effect";
        pass.output = &out;
        pass.pxScale = ctx.pxScale;
        pass.blend = g.blend;
        for (int i = 2; i < TITLE_PASS_VEC4S; i++) pass.set(i, g.params.v[i][0], g.params.v[i][1], g.params.v[i][2], g.params.v[i][3]);
        for (int i = 0; i < TITLE_PASS_TEXTURES; i++) pass.textures[i].texture = g.tex[i];
        for (int i = 0; i < TITLE_PASS_RAW_TEXTURES; i++) pass.raw[i].texture = g.raw[i];
        return ctx.runner.run(pass);
    }

    inline TextureRender* textureOf(const TargetRef& target) {
        TitleSource source;
        source.target = &target;
        return TitleJobRunner::textureOf(source);
    }

    // a scratch target like `like` (its format) of another size; released by the caller
    inline TargetRef* scratch(EffectContext& ctx, const TargetRef& like, int width, int height) {
        TargetKey key = like.key();
        key.width = width;
        key.height = height;
        return ctx.runner.acquire(key);
    }

    inline bool copyInto(EffectContext& ctx, const TargetRef& src, TargetRef& dst, float dx, float dy, float opacity, BlendMode blend) {
        TitleGpuPass p;
        p.program = TitleRenderProgram::Copy;
        p.tex[0] = textureOf(src);
        const float w = (float)dst.key().width, h = (float)dst.key().height;
        p.params.set(2, 1.0f / w, 1.0f / h, -dx / w, -dy / h);
        p.params.set(3, opacity, 0.0f, 1.0f, 0.0f);
        p.blend = blend;
        return runRender(ctx, p, dst);
    }

    inline bool resample(EffectContext& ctx, const TargetRef& src, TargetRef& dst) {
        TitleGpuPass p;
        setupResample(p, textureOf(src), dst.key().width, dst.key().height);
        return runRender(ctx, p, dst);
    }

    // the renderer's blur (TitleFields.h's plan): down by halvings, the separable kernel, back up
    inline bool blur(EffectContext& ctx, const TargetRef& src, float radiusPx, TargetRef& out) {
        const TargetKey key = src.key();
        const TitleBlurPlan plan = blurPlan(radiusPx, key.width, key.height);
        if (plan.copy) return resample(ctx, src, out);
        std::vector<TargetRef*> held;
        auto make = [&](int w, int h) {
            TargetRef* t = scratch(ctx, src, w, h);
            if (t) held.push_back(t);
            return t;
        };
        bool ok = true;
        const TargetRef* cur = &src;
        int w = key.width, h = key.height;
        for (const auto& size : plan.sizes) {
            TargetRef* level = make(size.first, size.second);
            ok = ok && level && resample(ctx, *cur, *level);
            if (!ok) break;
            cur = level;
            w = size.first;
            h = size.second;
        }
        TargetRef* across = ok ? make(w, h) : nullptr;
        TargetRef* vertical = plan.levels > 0 && across ? make(w, h) : &out;
        ok = ok && across && vertical;
        if (ok) {
            TitleGpuPass px;
            setupBlur(px, textureOf(*cur), w, h, true, plan.sigma);
            ok = runRender(ctx, px, *across);
        }
        if (ok) {
            TitleGpuPass py;
            setupBlur(py, textureOf(*across), w, h, false, plan.sigma);
            ok = runRender(ctx, py, *vertical);
        }
        if (ok && plan.levels > 0) ok = resample(ctx, *vertical, out);
        for (TargetRef* t : held) ctx.runner.release(t);
        return ok;
    }

    // the renderer's layer adjustment (Adjustments.h) as an effect
    inline bool adjust(EffectContext& ctx, const TargetRef& src, const Json& adjustment, TargetRef& out) {
        TitleGpuPass p;
        std::shared_ptr<TitleCacheEntry> lut;
        if (!setupAdjustment(adjustment, static_cast<TitleJobRunner&>(ctx.runner).renderServices(), p, lut)) return resample(ctx, src, out);
        p.tex[0] = textureOf(src);
        p.params.set(6, 1.0f / (float)out.key().width, 1.0f / (float)out.key().height, 0.0f, 0.0f);
        return runRender(ctx, p, out);
    }

    // the silhouette blurred and recoloured (a gradient map of one colour keeps the alpha)
    inline TargetRef* tinted(EffectContext& ctx, const TargetRef& src, float radius, const char* color) {
        TargetRef* blurred = scratch(ctx, src, src.key().width, src.key().height);
        TargetRef* out = scratch(ctx, src, src.key().width, src.key().height);
        const Json adjustment{{"type", "gradient_map"},
                              {"gradient", {{"stops", Json::array({{{"pos", 0}, {"color", color}, {"alpha", 1}, {"mid", 0.5}},
                                                                   {{"pos", 1}, {"color", color}, {"alpha", 1}, {"mid", 0.5}}})},
                                            {"interp", "rgb"}}},
                              {"reverse", false}};
        const bool ok = blurred && out && blur(ctx, src, radius, *blurred) && adjust(ctx, *blurred, adjustment, *out);
        if (blurred) ctx.runner.release(blurred);
        if (ok) return out;
        if (out) ctx.runner.release(out);
        return nullptr;
    }

    inline const TargetRef& contentOf(const EffectContext& ctx, const TargetRef& input) {
        return ctx.content ? *ctx.content : input;
    }

    inline void testBlur(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        blur(ctx, input, (float)(p.number("radius", 8) * ctx.pxScale), output);
    }

    inline void testShadow(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        TargetRef* shadow = tinted(ctx, contentOf(ctx, input), (float)(p.number("size", 6) * ctx.pxScale), "#000000");
        if (!shadow) return;
        // the direction it falls, clockwise on screen: rows down in the node's target
        const double a = p.number("angle", 60) * 3.14159265358979323846 / 180.0;
        const double d = p.number("distance", 10) * ctx.pxScale;
        ctx.runner.clear(output);
        copyInto(ctx, *shadow, output, (float)(std::cos(a) * d), (float)(std::sin(a) * d), (float)p.number("opacity", 0.75), BlendMode::NONE);
        copyInto(ctx, input, output, 0.0f, 0.0f, 1.0f, BlendMode::PREMULTIPLIED);
        ctx.runner.release(shadow);
    }

    inline void testGlow(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        TargetRef* glow = tinted(ctx, contentOf(ctx, input), (float)(p.number("size", 16) * ctx.pxScale), "#ffb000");
        if (!glow) return;
        resample(ctx, input, output);
        copyInto(ctx, *glow, output, 0.0f, 0.0f, (float)p.number("intensity", 1), BlendMode::LIGHT_PREMULTIPLIED);
        ctx.runner.release(glow);
    }

    inline void testOutline(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        TargetRef* line = tinted(ctx, ctx.inputInk ? *ctx.inputInk : input, (float)(p.number("width", 3) * ctx.pxScale), "#202020");
        if (!line) return;
        ctx.runner.clear(output);
        copyInto(ctx, *line, output, 0.0f, 0.0f, 1.0f, BlendMode::NONE);
        copyInto(ctx, input, output, 0.0f, 0.0f, 1.0f, BlendMode::PREMULTIPLIED);
        ctx.runner.release(line);
    }

    inline void testAdjust(EffectContext& ctx, const TargetRef& input, TargetRef& output, const ParamValues& p) {
        adjust(ctx, input, p.raw("adjustment"), output);
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
