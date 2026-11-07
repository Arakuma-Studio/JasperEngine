// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/generators/common.ts (rng, followOffset, drawFollowed, glowInto) and
// src/effects/b/common.ts (rng, hash) by SafEight, used with the author's permission. mulberry32
// is Tommy Ettinger's (public domain).

#include "TitleGen.h"

#include "pool/ShaderPool.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::gen;

const char* gen::genProgramName(GenProgram program) {
    switch (program) {
        case GenProgram::Place: return "place";
        case GenProgram::GlowAdd: return "glow_add";
        case GenProgram::Grid: return "grid";
        case GenProgram::Shockwave: return "shockwave";
        case GenProgram::LensFlare: return "lens_flare";
        case GenProgram::Sunburst: return "sunburst";
        case GenProgram::FractalNoise: return "fractal_noise";
        case GenProgram::Fire: return "fire";
        case GenProgram::Smoke: return "smoke";
        case GenProgram::AuraTendrils: return "aura_tendrils";
        case GenProgram::AuraRising: return "aura_rising";
        case GenProgram::AuraNormals: return "aura_normals";
        case GenProgram::BacklightSource: return "backlight_source";
        case GenProgram::BacklightRadial: return "backlight_radial";
        case GenProgram::BacklightCompose: return "backlight_compose";
        case GenProgram::MagicCircle: return "magic_circle";
        case GenProgram::LightningCompose: return "lightning_compose";
        case GenProgram::LightStreakCompose: return "light_streak_compose";
        case GenProgram::SpeedLinesBeta: return "speed_lines_beta";
        case GenProgram::GlintsCompose: return "glints_compose";
        case GenProgram::QuadsParticles: return "quads_particles";
        case GenProgram::QuadsGlints: return "quads_glints";
        case GenProgram::QuadsInk: return "quads_ink";
        case GenProgram::QuadsSpeedLines: return "quads_speed_lines";
        case GenProgram::QuadsLightStreaks: return "quads_light_streaks";
        case GenProgram::QuadsLightning: return "quads_lightning";
        case GenProgram::QuadsSplatter: return "quads_splatter";
        case GenProgram::QuadsTest: return "quads_test";
        case GenProgram::MeshBrushSwirl: return "mesh_brush_swirl";
    }
    return "";
}

const std::vector<GenProgram>& gen::genPrograms() {
    static const std::vector<GenProgram> all = {
        GenProgram::Place, GenProgram::GlowAdd, GenProgram::Grid, GenProgram::Shockwave, GenProgram::LensFlare, GenProgram::Sunburst,
        GenProgram::FractalNoise, GenProgram::Fire, GenProgram::Smoke, GenProgram::AuraTendrils, GenProgram::AuraRising,
        GenProgram::AuraNormals, GenProgram::BacklightSource, GenProgram::BacklightRadial, GenProgram::BacklightCompose,
        GenProgram::MagicCircle, GenProgram::LightningCompose, GenProgram::LightStreakCompose, GenProgram::SpeedLinesBeta,
        GenProgram::GlintsCompose,
        GenProgram::QuadsParticles, GenProgram::QuadsGlints, GenProgram::QuadsInk, GenProgram::QuadsSpeedLines,
        GenProgram::QuadsLightStreaks, GenProgram::QuadsLightning, GenProgram::QuadsSplatter, GenProgram::QuadsTest,
        GenProgram::MeshBrushSwirl};
    return all;
}

namespace {

    // the programs that read a distance field (TA_RAW slots holding the effect library's field)
    bool readsDistance(GenProgram program) {
        switch (program) {
            case GenProgram::Fire:
            case GenProgram::Smoke:
            case GenProgram::AuraTendrils:
            case GenProgram::AuraRising:
            case GenProgram::AuraNormals:
                return true;
            default:
                return false;
        }
    }

}

uint64_t gen::genProgramVariant(GenProgram program, const TitleArtCaps& caps) {
    uint64_t variant = TITLE_VARIANT_GENERATOR;
    if (readsDistance(program) && caps.format(TitleBuffer::Distance).encoding == TitleEncoding::Fixed16) variant |= TITLE_VARIANT_DISTANCE_FIXED;
    return variant;
}

ShaderKey gen::genProgramKey(GenProgram program, uint64_t variant) {
    return ShaderPool::getShaderKey(ShaderType::TITLE_ART, (uint64_t)program | variant | TITLE_VARIANT_GENERATOR);
}

double gen::spaceScale(const TitleAffine& m) {
    return std::sqrt(m.a * m.a + m.c * m.c);
}

TitleRectI gen::targetBox(const GeneratorContext& ctx, const TitleAffine& m, double x0, double y0, double x1, double y1) {
    double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
    for (const TitleVec2& c : {TitleVec2{x0, y0}, TitleVec2{x1, y0}, TitleVec2{x0, y1}, TitleVec2{x1, y1}}) {
        const TitleVec2 t = m.apply(c);
        minX = std::min(minX, t.x);
        minY = std::min(minY, t.y);
        maxX = std::max(maxX, t.x);
        maxY = std::max(maxY, t.y);
    }
    const int ix0 = std::max(0, (int)std::floor(minX) - 1), iy0 = std::max(0, (int)std::floor(minY) - 1);
    const int ix1 = std::min(ctx.width, (int)std::ceil(maxX) + 1), iy1 = std::min(ctx.height, (int)std::ceil(maxY) + 1);
    if (ix1 <= ix0 || iy1 <= iy0) return {};
    return {ix0, iy0, ix1 - ix0, iy1 - iy0};
}

TitleAffine gen::drawSpace(const GeneratorContext& ctx) {
    return ctx.follow ? ctx.documentToTarget : ctx.layerToTarget;
}

TitlePass gen::genPass(const GeneratorContext& ctx, GenProgram program, const char* label, TargetRef& output, const TitleAffine& spaceToTarget) {
    TitlePass pass;
    pass.program = (TitleProgram)(uint8_t)program;
    pass.variant = genProgramVariant(program, ctx.runner.caps());
    pass.label = label;
    pass.output = &output;
    pass.pxScale = ctx.pxScale;
    if (!ctx.frame.empty() && ctx.width > 0 && ctx.height > 0) {
        pass.frameUv[0] = (float)ctx.frame.x / (float)ctx.width;
        pass.frameUv[1] = (float)ctx.frame.y / (float)ctx.height;
        pass.frameUv[2] = (float)(ctx.frame.x + ctx.frame.width) / (float)ctx.width;
        pass.frameUv[3] = (float)(ctx.frame.y + ctx.frame.height) / (float)ctx.height;
    }
    const TitleAffine inv = spaceToTarget.inverse();
    pass.set(2, (float)inv.a, (float)inv.b, (float)inv.c, (float)inv.d);
    pass.set(3, (float)inv.tx, (float)inv.ty, (float)spaceScale(spaceToTarget), ctx.pxScale);
    pass.set(4, (float)spaceToTarget.a, (float)spaceToTarget.b, (float)spaceToTarget.c, (float)spaceToTarget.d);
    pass.set(5, (float)spaceToTarget.tx, (float)spaceToTarget.ty, 1.5f, 0.0f);
    return pass;
}

EffectContext gen::effectContext(const GeneratorContext& ctx) {
    EffectContext e(ctx.runner);
    e.width = ctx.width;
    e.height = ctx.height;
    e.pxScale = ctx.pxScale;
    e.frame = ctx.frame;
    e.documentToTarget = ctx.documentToTarget;
    e.layerOrigin = ctx.layerToTarget.apply(TitleVec2{0.0, 0.0});
    e.seed = ctx.seed;
    e.lightAngle = ctx.lightAngle;
    e.lightAltitude = ctx.lightAltitude;
    e.layerId = ctx.layerId;
    return e;
}

void gen::glowInto(const GeneratorContext& ctx, const TargetRef& src, TargetRef& target, float radiusCanvasPx, float amount) {
    if (!(amount > 0.0f) || !(radiusCanvasPx > 0.0f)) return;
    EffectContext e = effectContext(ctx);
    fx::Scratch blurred = fx::blur(e, src, radiusCanvasPx * ctx.pxScale);
    if (!blurred) return;
    TitlePass pass = genPass(ctx, GenProgram::GlowAdd, "glow", target, ctx.documentToTarget);
    pass.textures[0].target = blurred.get();
    pass.set(6, amount);
    pass.blend = BlendMode::LIGHT_PREMULTIPLIED;
    ctx.runner.run(pass);
}

namespace {

    // the generator's transform relative to the followed layer, in target px (Pachislop's
    // followOffset: layerToRender after compToRender's inverse)
    TitleAffine followOffset(const GeneratorContext& ctx) {
        return ctx.layerToTarget.after(ctx.documentToTarget.inverse());
    }

    bool isIdentity(const TitleAffine& m) {
        return std::abs(m.a - 1.0) < 1e-9 && std::abs(m.b) < 1e-9 && std::abs(m.c) < 1e-9 && std::abs(m.d - 1.0) < 1e-9 &&
               std::abs(m.tx) < 1e-9 && std::abs(m.ty) < 1e-9;
    }

}

bool gen::hasFollowOffset(const GeneratorContext& ctx) {
    return !isIdentity(followOffset(ctx));
}

void gen::drawFollowed(GeneratorContext& ctx, TargetRef& target, const std::function<void(TargetRef& out)>& draw) {
    const TitleAffine off = followOffset(ctx);
    if (isIdentity(off)) {
        draw(target);
        return;
    }
    fx::Scratch tmp = fx::scratchOf(ctx.runner, TitleBuffer::Accumulator, ctx.width, ctx.height);
    if (!tmp) return;
    draw(*tmp);
    // target px -> the scratch's px: the inverse of the offset (the Place program samples there)
    TitlePass pass = genPass(ctx, GenProgram::Place, "follow offset", target, off.inverse());
    pass.textures[0].target = tmp.get();
    ctx.runner.run(pass);
}

double gen::seedOf(const GeneratorContext& ctx, const ParamValues& p, const char* key) {
    return p.number(key, 1.0) + ((double)ctx.seed - 1.0);
}

Rng::Rng(double seed, Seeding seeding) : generators(seeding == Seeding::Generators) {
    if (generators) {
        // common.ts: Math.imul(Math.floor(seed * 4096) ^ 0x9e3779b9, 0x85ebca6b) >>> 0
        const int32_t s = (int32_t)(int64_t)std::floor(seed * 4096.0);
        state = (uint32_t)imul(s ^ (int32_t)0x9e3779b9, (int32_t)0x85ebca6b);
    } else {
        // b/common.ts: (Math.floor(seed * 7919) ^ 0x9e3779b9) >>> 0
        const int32_t s = (int32_t)(int64_t)std::floor(seed * 7919.0);
        state = (uint32_t)(s ^ (int32_t)0x9e3779b9);
    }
}

double Rng::next() {
    // both files' mulberry32 step: the same arithmetic in two spellings
    state += 0x6d2b79f5u;
    uint32_t t = state;
    t = (uint32_t)imul((int32_t)(t ^ (t >> 15)), (int32_t)(t | 1u));
    t ^= t + (uint32_t)imul((int32_t)(t ^ (t >> 7)), (int32_t)(t | 61u));
    return (double)(t ^ (t >> 14)) / 4294967296.0;
}

double gen::hashN(std::initializer_list<double> values) {
    uint32_t h = 0x811c9dc5u;
    for (double v : values) {
        // Math.floor(v * 1000003) | 0: the double's integer part wrapped to 32 bits
        const double f = std::floor(v * 1000003.0);
        const int32_t i = (int32_t)(uint32_t)(int64_t)std::fmod(f, 4294967296.0);
        h ^= (uint32_t)i;
        h = (uint32_t)imul((int32_t)h, 0x01000193);
        h ^= h >> 15;
        h = (uint32_t)imul((int32_t)h, 0x2c1b3c6d);
        h ^= h >> 12;
    }
    return (double)h / 4294967296.0;
}

ParamDef gen::point(const char* key, const char* label, double x, double y, const char* doc) {
    ParamDef p;
    p.key = key;
    p.label = label;
    p.kind = ParamKind::Vec2;
    p.unit = ParamUnit::Px;
    p.defaultValue = Json::array({x, y});
    p.description = doc;
    return p;
}

FxCost gen::passCost(int passes, double fre, int taps, int instances, double cpuMs) {
    FxCost c;
    c.passes = passes;
    c.fre = fre;
    c.maxTapsPerPixel = taps;
    c.instances = instances;
    c.cpuMs = cpuMs;
    return c;
}
