// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/engine/three/glint.ts by SafEight, used with the author's permission.

#include "Glint3D.h"

#include "pool/ShaderPool.h"
#include "render/CameraRender.h"
#include "render/ObjectRender.h"
#include "render/PipelineState.h"
#include "render/ShaderRender.h"
#include "render/TextureRender.h"
#include "titleart/render/TitleGpu.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    // glint.ts's constants (each explained there)
    constexpr double STREAK_PEAK = 1.5;
    constexpr double STREAK_TAIL = 0.02;
    constexpr double STREAK_CORE = 2.5;
    constexpr double STREAK_CORE_LEN = 0.22;
    constexpr double POINT_BLOOM = 8;
    constexpr double POINT_BLOOM_SIGMA = 1.2;
    constexpr double PINCH_K = 8;
    constexpr double ISOLATION_K = 3;
    constexpr double COVER_SIGMA = 6;
    constexpr double COVER_K = 0.5;
    constexpr double BUDGET_SIGMA = 16;
    constexpr double DENSITY_K = 40;
    constexpr double MIP_SIGMA = 5;
    constexpr double DETAIL_SIGMA = 3;
    constexpr int MAX_LEVEL = 6;
    constexpr double PI = 3.14159265358979323846;

    enum Program : uint32_t { BRIGHT = 1, DOWN = 2, BLUR = 3, STREAK = 4, ADD = 5, HIGHPASS = 6, TAME = 7, PINCH = 8, DENSITY = 9 };

    struct alignas(16) Block {
        float p0[4], p1[4], p2[4], p3[4];
    };

    struct alignas(16) SpriteVs {
        float grid[4], map[4], frame[4], gain[4];
    };

    struct alignas(16) SpriteFs {
        float shape[4], bloom[4];
    };

    void set4(float* out, double a, double b = 0, double c = 0, double d = 0) {
        out[0] = (float)a;
        out[1] = (float)b;
        out[2] = (float)c;
        out[3] = (float)d;
    }

    struct Pass {
        std::shared_ptr<ShaderRender> shader;
        ObjectRender object;
        bool loaded = false, failed = false;
        int block = -1, vsBlock = -1;
        std::pair<int, int> src{-1, -1}, aux{-1, -1}, aux2{-1, -1}, aux3{-1, -1}, peaks{-1, -1};
    };

    // one axis of the output: the whole render's level-B lattice (glint.ts outLattice)
    struct Lattice {
        int n = 1, j0 = 0, count = 1;
        double lo = 0, hi = 0;
    };

    Lattice outLattice(double F, double fx, double w, double grid) {
        Lattice l;
        l.n = std::max(1, (int)std::ceil(F / grid));
        l.j0 = (int)std::floor(fx * l.n / F);
        l.count = std::max(1, (int)std::ceil((fx + w) * l.n / F) - l.j0);
        l.lo = l.j0 * F / l.n - fx;
        l.hi = (l.j0 + l.count) * F / l.n - fx;
        return l;
    }

}

struct Glint3D::Impl {
    std::map<uint32_t, std::unique_ptr<Pass>> passes;   // TITLE_GLINT programs; 0: the sprites
    TextureRender placeholder;
    bool placeholderMade = false;
    CameraRender camera;

    ~Impl() {
        for (auto& [k, p] : passes) {
            if (p->loaded) p->object.destroy();
        }
        if (placeholderMade) placeholder.destroyTexture();
    }

    bool ensurePlaceholder() {
        if (placeholderMade) return true;
        static unsigned char texel[4] = {0, 0, 0, 0};
        void* data[1] = {texel};
        size_t size[1] = {4};
        placeholderMade = placeholder.createTexture("title glint placeholder", 1, 1, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data, size,
                                                    TextureFilter::LINEAR, TextureFilter::LINEAR, TextureWrap::CLAMP_TO_EDGE,
                                                    TextureWrap::CLAMP_TO_EDGE);
        return placeholderMade;
    }

    static ShaderType typeOf(uint32_t key) { return key == 0 ? ShaderType::TITLE_GLINT_SPRITE : ShaderType::TITLE_GLINT; }

    bool shaderReady(uint32_t key, std::string* error) {
        const ShaderType type = typeOf(key);
        if (ShaderPool::isShaderBuildFailed(type, key)) {
            if (error) *error = "the glint shader " + ShaderPool::getShaderStr(type, key) + " could not be built";
            return false;
        }
        std::shared_ptr<ShaderRender> s = ShaderPool::get(type, key);
        return s && s->isCreated();
    }

    Pass* pass(uint32_t key, std::string& error) {
        std::unique_ptr<Pass>& slot = passes[key];
        if (!slot) slot = std::make_unique<Pass>();
        Pass& p = *slot;
        if (p.loaded) return &p;
        const ShaderType type = typeOf(key);
        if (p.failed || ShaderPool::isShaderBuildFailed(type, key)) {
            error = "the glint program " + ShaderPool::getShaderStr(type, key) + " could not be loaded";
            return nullptr;
        }
        p.shader = ShaderPool::get(type, key);
        if (!p.shader || !p.shader->isCreated() || !ensurePlaceholder()) {
            error = "the glint programs are not ready";
            return nullptr;
        }
        ShaderData& data = p.shader->shaderData;
        p.object.beginLoad(PrimitiveType::TRIANGLES);
        p.object.setShader(p.shader.get());
        if (key == 0) {
            p.vsBlock = data.getUniformBlockIndexByName("u_vs_glintSprite");
            p.block = data.getUniformBlockIndexByName("u_fs_glintSprite");
            p.peaks = data.getTextureIndexByName("u_gsPeaks");
            if (p.peaks.first >= 0) p.object.addTexture(p.peaks, ShaderStageType::VERTEX, &placeholder);
        } else {
            p.block = data.getUniformBlockIndexByName("u_fs_titleGlint");
            p.src = data.getTextureIndexByName("u_tgSrc");
            p.aux = data.getTextureIndexByName("u_tgAux");
            p.aux2 = data.getTextureIndexByName("u_tgAux2");
            p.aux3 = data.getTextureIndexByName("u_tgAux3");
            for (auto s : {p.src, p.aux, p.aux2, p.aux3}) {
                if (s.first >= 0) p.object.addTexture(s, ShaderStageType::FRAGMENT, &placeholder);
            }
        }
        p.object.setImmediatePipelines(true);
        if (p.block < 0 || (key == 0 && p.vsBlock < 0) || !p.object.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE)) {
            p.object.destroy();
            p.failed = true;
            error = "the glint program " + ShaderPool::getShaderStr(type, key) + " could not be loaded";
            return nullptr;
        }
        p.loaded = true;
        return &p;
    }
};

Glint3D::Glint3D() : impl(std::make_unique<Impl>()) {}

Glint3D::~Glint3D() = default;

bool Glint3D::ready(GlintMode mode, std::string* error) {
    if (mode == GlintMode::Off) return true;
    std::vector<uint32_t> keys = {BRIGHT, BLUR, HIGHPASS};
    if (mode == GlintMode::Sprites) keys.push_back(0);
    else keys.insert(keys.end(), {DOWN, STREAK, ADD, TAME, PINCH, DENSITY});
    for (uint32_t k : keys) {
        if (!impl->shaderReady(k, error)) return false;
    }
    return true;
}

bool Glint3D::render(const Glint3DInput& in, TitleTargetPool& pool, Glint3DOutput& out, std::string& error) {
    Impl& im = *impl;
    const Glint3DParams& g = in.params;
    const bool mobile = in.mode == GlintMode::Mobile;
    const bool sprites = in.mode == GlintMode::Sprites;
    // the grid: level B texels are about one canvas px (render px below a 1x preview); the
    // mobile reduction runs a level coarser
    const int base = std::max(0, (int)std::lround(std::log2(std::max(1.0, in.pxScale)))) + (mobile ? 1 : 0);
    const double grid = std::ldexp(1.0, base);
    auto levelFor = [&](double sigma, double minTexels) {
        return base + std::min(MAX_LEVEL, std::max(0, (int)std::floor(std::log2(std::max(1e-9, sigma / minTexels)))));
    };
    const double radius = std::max(0.0, g.radius * in.unit) / grid;
    const double detail = std::max(1.0, GLINT_DETAIL * in.unit / grid);
    int rays = (int)std::lround(std::clamp(g.streaks, 0, 8));
    if (mobile) rays = std::min(rays, 2);
    const int sl = levelFor(detail, DETAIL_SIGMA);
    const double len = std::max(0.0, g.length * in.unit) / std::ldexp(1.0, sl);
    const bool streaking = rays > 0 && len > 1;
    struct Bloom {
        double sigma, gain;
        int level;
    };
    std::vector<Bloom> blooms;
    if (radius > 0 && !sprites) {
        blooms.push_back({radius * 0.3, 0.4, levelFor(radius * 0.3, MIP_SIGMA / 2)});
        blooms.push_back({radius, 0.3, levelFor(radius, MIP_SIGMA / 2)});
    }
    const double coverSigma = COVER_SIGMA * detail, budgetSigma = BUDGET_SIGMA * detail;
    const int coverLevel = levelFor(coverSigma, MIP_SIGMA / 2), budgetLevel = levelFor(budgetSigma, MIP_SIGMA / 2);
    int deepest = std::max(base, sprites ? sl : coverLevel);
    for (const Bloom& b : blooms) deepest = std::max(deepest, b.level);
    if (streaking && !sprites) deepest = std::max(deepest, std::max(sl + 1, mobile ? sl + 1 : budgetLevel));
    // the output: the whole render's level-B lattice covering the region
    const Lattice lx = outLattice(in.frameWidth, in.region.x, in.region.width, grid);
    const Lattice ly = outLattice(in.frameHeight, in.region.y, in.region.height, grid);
    // the domain: whole deepest-level texels each way from the canvas centre past the output
    const double cell = std::ldexp(1.0, deepest);
    const double cx = in.frameWidth / 2 - in.region.x, cy = in.frameHeight / 2 - in.region.y;
    const double left = std::ceil((cx - lx.lo) / cell), above = std::ceil((cy - ly.lo) / cell);
    const int W = (int)((left + std::ceil((lx.hi - cx) / cell)) * cell);
    const int H = (int)((above + std::ceil((ly.hi - cy) / cell)) * cell);
    const double ox = cx - left * cell, oy = cy - above * cell;

    std::map<int, TargetRef> held;     // level * 32 + slot
    std::string poolError;
    auto target = [&](int level, int slot) -> TitleTarget* {
        TargetRef& ref = held[level * 32 + slot];
        if (!ref) {
            TargetKey key;
            key.width = std::max(1, (int)std::ceil(W / std::ldexp(1.0, level)));
            key.height = std::max(1, (int)std::ceil(H / std::ldexp(1.0, level)));
            key.format = ColorFormat::RGBA16F;
            key.filter = TextureFilter::LINEAR;
            ref = pool.acquire(key, &poolError);
        }
        return ref.get();
    };
    auto textureOf = [](TitleTarget* t) { return &static_cast<SokolTitleTarget*>(t)->texture(); };
    auto sizeOf = [](TitleTarget* t) { return static_cast<SokolTitleTarget*>(t)->key(); };
    auto run = [&](uint32_t program, TitleTarget* dst, TextureRender* src, TextureRender* aux, TextureRender* aux2, TextureRender* aux3,
                   Block block, bool additive) {
        Pass* p = im.pass(program, error);
        if (!p || !dst) {
            if (error.empty()) error = poolError.empty() ? "a glint target could not be made" : poolError;
            return false;
        }
        auto* t = static_cast<SokolTitleTarget*>(dst);
        set4(block.p3, 1.0 / t->key().width, 1.0 / t->key().height);
        im.camera.setColorActions(additive ? LoadAction::LOAD : LoadAction::DONTCARE, StoreAction::STORE);
        im.camera.setDepthStencilActions(LoadAction::DONTCARE, StoreAction::DONTCARE);
        im.camera.setLabel("title 3d glint");
        im.camera.startRenderPass(&t->framebuffer());
        PipelineState state;
        state.target.color = t->key().format;
        state.target.depth = DepthStencilFormat::NONE;
        state.target.sampleCount = 1;
        state.blend = additive ? BlendMode::ADDITIVE_PREMULTIPLIED : BlendMode::NONE;
        bool drawn = false;
        if (p->object.beginDraw(state)) {
            if (p->src.first >= 0) p->object.addTexture(p->src, ShaderStageType::FRAGMENT, src ? src : &im.placeholder);
            if (p->aux.first >= 0) p->object.addTexture(p->aux, ShaderStageType::FRAGMENT, aux ? aux : &im.placeholder);
            if (p->aux2.first >= 0) p->object.addTexture(p->aux2, ShaderStageType::FRAGMENT, aux2 ? aux2 : &im.placeholder);
            if (p->aux3.first >= 0) p->object.addTexture(p->aux3, ShaderStageType::FRAGMENT, aux3 ? aux3 : &im.placeholder);
            p->object.applyUniformBlock(p->block, sizeof(Block), &block);
            p->object.draw(0, 3, 1);
            drawn = true;
        }
        im.camera.endRenderPass();
        out.passes++;
        if (!drawn) error = "a glint pass could not draw";
        return drawn;
    };
    auto blur1D = [&](TitleTarget* src, int level, int slot, double sigma, int axis) -> TitleTarget* {
        TitleTarget* dst = target(level, slot);
        const TargetKey k = sizeOf(src);
        Block b{};
        sigma = std::max(0.5, sigma);
        set4(b.p1, axis == 0 ? 1.0 / k.width : 0, axis == 1 ? 1.0 / k.height : 0, sigma, std::ceil(3 * sigma));
        return run(BLUR, dst, textureOf(src), nullptr, nullptr, nullptr, b, false) ? dst : nullptr;
    };
    auto gaussian = [&](TitleTarget* src, int level, int slot, double sigma) -> TitleTarget* {
        TitleTarget* tmp = blur1D(src, level, 9, sigma, 0);
        return tmp ? blur1D(tmp, level, slot, sigma, 1) : nullptr;
    };
    auto down = [&](TitleTarget* src, int level, int slot) -> TitleTarget* {
        TitleTarget* dst = target(level, slot);
        return run(DOWN, dst, textureOf(src), nullptr, nullptr, nullptr, Block{}, false) ? dst : nullptr;
    };

    // the output, cleared
    {
        TargetKey key;
        key.width = lx.count;
        key.height = ly.count;
        key.format = ColorFormat::RGBA16F;
        key.filter = TextureFilter::NEAREST;
        out.light = pool.acquire(key, &poolError);
        if (!out.light) {
            error = poolError.empty() ? "the glint output could not be made" : poolError;
            return false;
        }
        auto* t = out.light.as<SokolTitleTarget>();
        im.camera.setColorActions(LoadAction::CLEAR, StoreAction::STORE, Vector4(0, 0, 0, 0));
        im.camera.setDepthStencilActions(LoadAction::DONTCARE, StoreAction::DONTCARE);
        im.camera.setLabel("title 3d glint clear");
        im.camera.startRenderPass(&t->framebuffer());
        im.camera.endRenderPass();
    }
    // target uv -> output uv
    out.uv[0] = (float)(in.region.width * lx.n / (in.frameWidth * lx.count));
    out.uv[1] = (float)(in.region.height * ly.n / (in.frameHeight * ly.count));
    out.uv[2] = (float)((in.region.x * lx.n / in.frameWidth - lx.j0) / lx.count);
    out.uv[3] = (float)((in.region.y * ly.n / in.frameHeight - ly.j0) / ly.count);
    const double k = std::max(0.0, g.intensity);
    const auto tint = std::array<float, 3>{srgbToLinear(g.tint.r), srgbToLinear(g.tint.g), srgbToLinear(g.tint.b)};
    // the angle: clockwise on screen, rows down
    const double angle = g.angle * PI / 180.0;

    // the bright pass onto the grid (sprites: straight onto the stars' mip)
    const int brightLevel = sprites ? sl : base;
    const int ss = std::max(1, in.k);
    Block brightBlock{};
    set4(brightBlock.p0, 0, in.exposure, in.shoulder ? 1 : 0, in.compressed ? 1 : 0);
    set4(brightBlock.p1, std::max(0.0, g.threshold), std::ldexp(1.0, brightLevel) * ss);
    set4(brightBlock.p2, std::lround(ox * ss), std::lround(oy * ss));
    TitleTarget* bright = target(brightLevel, 1);
    if (!run(BRIGHT, bright, in.source, nullptr, nullptr, nullptr, brightBlock, false)) return false;

    if (sprites) {
        // the point detector on the stars' mip, then a star per local maximum
        const double sigma = detail / std::ldexp(1.0, sl - base);
        TitleTarget* blurX = blur1D(bright, sl, 6, sigma, 0);
        TitleTarget* blurY = blurX ? blur1D(bright, sl, 8, sigma, 1) : nullptr;
        if (!blurY) return false;
        TitleTarget* peaks = target(sl, 7);
        Block hp{};
        set4(hp.p1, COVER_K);
        if (!run(HIGHPASS, peaks, textureOf(bright), textureOf(blurX), textureOf(blurY), nullptr, hp, false)) return false;
        Pass* sp = im.pass(0, error);
        if (!sp) return false;
        const TargetKey pk = sizeOf(peaks);
        // a point texel's centre on the output lattice: domain px -> lattice texels
        const double texelPx = std::ldexp(1.0, sl);
        const double outPxX = in.frameWidth / lx.n, outPxY = in.frameHeight / ly.n;
        SpriteVs vs{};
        set4(vs.grid, pk.width, pk.height);
        set4(vs.map, texelPx / outPxX, texelPx / outPxY, (ox + 0.5 * texelPx - lx.lo) / outPxX, (oy + 0.5 * texelPx - ly.lo) / outPxY);
        const double lengthOut = std::max(1.0, g.length * in.unit / outPxX);
        set4(vs.frame, lx.count, ly.count, lengthOut, in.originTopLeft ? 1 : 0);
        set4(vs.gain, tint[0] * k, tint[1] * k, tint[2] * k, 1e-3);
        SpriteFs fs{};
        set4(fs.shape, rays, angle, lengthOut, std::max(0.75, detail * grid / outPxX * 0.25));
        set4(fs.bloom, std::max(0.75, POINT_BLOOM_SIGMA * detail * grid / outPxX), POINT_BLOOM * 0.25, STREAK_PEAK / std::max(1.0, sigma));
        auto* t = out.light.as<SokolTitleTarget>();
        im.camera.setColorActions(LoadAction::LOAD, StoreAction::STORE);
        im.camera.setDepthStencilActions(LoadAction::DONTCARE, StoreAction::DONTCARE);
        im.camera.setLabel("title 3d glint sprites");
        im.camera.startRenderPass(&t->framebuffer());
        PipelineState state;
        state.target.color = ColorFormat::RGBA16F;
        state.target.depth = DepthStencilFormat::NONE;
        state.target.sampleCount = 1;
        state.blend = BlendMode::ADDITIVE_PREMULTIPLIED;
        if (sp->object.beginDraw(state)) {
            if (sp->peaks.first >= 0) sp->object.addTexture(sp->peaks, ShaderStageType::VERTEX, textureOf(peaks));
            sp->object.applyUniformBlock(sp->vsBlock, sizeof(SpriteVs), &vs);
            sp->object.applyUniformBlock(sp->block, sizeof(SpriteFs), &fs);
            sp->object.draw(0, 6, (unsigned)(pk.width * pk.height));
        }
        im.camera.endRenderPass();
        out.passes++;
        return true;
    }

    std::vector<TitleTarget*> mips((size_t)deepest + 2, nullptr);
    mips[(size_t)base] = bright;
    for (int l = base + 1; l <= deepest; l++) {
        mips[(size_t)l] = down(mips[(size_t)l - 1], l, 1);
        if (!mips[(size_t)l]) return false;
    }
    // the hot coverage: the bright pass averaged over a few letter strokes (TAME)
    TitleTarget* coverage = gaussian(mips[(size_t)coverLevel], coverLevel, 17, coverSigma / std::ldexp(1.0, coverLevel - base));
    if (!coverage) return false;
    std::map<int, TitleTarget*> tamed;
    auto tame = [&](int level) -> TitleTarget* {
        auto it = tamed.find(level);
        if (it != tamed.end()) return it->second;
        TitleTarget* rt = target(level, 18);
        Block b{};
        set4(b.p1, COVER_K);
        if (!run(TAME, rt, textureOf(mips[(size_t)level]), nullptr, nullptr, textureOf(coverage), b, false)) return nullptr;
        tamed[level] = rt;
        return rt;
    };
    TitleTarget* light = out.light.get();
    Block add{};
    set4(add.p2, (double)lx.count * in.frameWidth / lx.n / W, (double)ly.count * in.frameHeight / ly.n / H, (lx.lo - ox) / W, (ly.lo - oy) / H);
    auto addInto = [&](TitleTarget* src, double gain) {
        Block b = add;
        set4(b.p1, tint[0] * k * gain, tint[1] * k * gain, tint[2] * k * gain);
        return run(ADD, light, textureOf(src), nullptr, nullptr, nullptr, b, true);
    };
    int bloomSlot = 2;
    for (const Bloom& b : blooms) {
        TitleTarget* t = tame(b.level);
        TitleTarget* blurred = t ? gaussian(t, b.level, bloomSlot++, b.sigma / std::ldexp(1.0, b.level - base)) : nullptr;
        if (!blurred || !addInto(blurred, b.gain)) return false;
    }

    if (streaking) {
        const double sigma = detail / std::ldexp(1.0, sl - base);
        TitleTarget* blurX = blur1D(mips[(size_t)sl], sl, 6, sigma, 0);
        TitleTarget* blurY = blurX ? blur1D(mips[(size_t)sl], sl, 8, sigma, 1) : nullptr;
        if (!blurY) return false;
        TitleTarget* peaks = target(sl, 7);
        Block hp{};
        set4(hp.p1, COVER_K);
        if (!run(HIGHPASS, peaks, textureOf(mips[(size_t)sl]), textureOf(blurX), textureOf(blurY), textureOf(coverage), hp, false)) return false;
        // light gathered over a few detail sizes, on sl + 1
        auto gather = [&](TitleTarget* src, int mid, int slot) -> TitleTarget* {
            TitleTarget* half = down(src, sl + 1, mid);
            return half ? gaussian(half, sl + 1, slot, sigma * POINT_BLOOM_SIGMA / 2) : nullptr;
        };
        TitleTarget* ref = gather(peaks, 10, 11);
        if (!ref) return false;
        TitleTarget* pinched = target(sl, 11);
        Block pb{};
        set4(pb.p1, PINCH_K, ISOLATION_K);
        if (!run(PINCH, pinched, textureOf(peaks), textureOf(ref), nullptr, textureOf(coverage), pb, false)) return false;
        TitleTarget* pointLight = gather(pinched, 12, 21);
        if (!pointLight) return false;
        TitleTarget* stars = pinched;
        TitleTarget* core = pointLight;
        if (!mobile) {
            // the star budget: a field of glints keeps its brightest few
            TitleTarget* wideSrc = pointLight;
            for (int l = sl + 2; l <= budgetLevel; l++) {
                wideSrc = down(wideSrc, l, 19);
                if (!wideSrc) return false;
            }
            const int wideLevel = std::max(sl + 1, budgetLevel);
            TitleTarget* pointWide = gaussian(wideSrc, wideLevel, 20, budgetSigma / std::ldexp(1.0, wideLevel - base));
            if (!pointWide) return false;
            auto gated = [&](TitleTarget* src, int level, int slot) -> TitleTarget* {
                TitleTarget* rt = target(level, slot);
                Block b{};
                set4(b.p1, DENSITY_K);
                return run(DENSITY, rt, textureOf(src), textureOf(pointLight), textureOf(pointWide), nullptr, b, false) ? rt : nullptr;
            };
            stars = gated(pinched, sl, 22);
            core = stars ? gated(pointLight, sl + 1, 22) : nullptr;
            if (!core) return false;
        }
        // the hot core: a round bloom of the points under the rays
        if (!addInto(core, POINT_BLOOM)) return false;
        auto streakRays = [&](TitleTarget* src, int level, double n, double gain) {
            const double fall = std::pow(STREAK_TAIL, 1.0 / n);
            const int passCount = std::max(1, (int)std::ceil(std::log(n + 1) / std::log(4.0)));
            // each pass normalises its 4 taps; undo that so the kernel's first tap weighs 1
            double kernelSum = 1;
            for (int i = 0; i < passCount; i++) {
                const double f = std::pow(fall, std::pow(4.0, i));
                kernelSum *= 1 + f + f * f + f * f * f;
            }
            const TargetKey sk = sizeOf(src);
            for (int r = 0; r < rays; r++) {
                const double a = angle + r * 2 * PI / rays;
                const double dx = std::cos(a) / sk.width, dy = std::sin(a) / sk.height;
                TitleTarget* from = src;
                for (int i = 0; i < passCount; i++) {
                    const double stride = std::pow(4.0, i);
                    TitleTarget* to = target(level, 4 + (i % 2));
                    Block b{};
                    set4(b.p1, dx * stride, dy * stride, std::pow(fall, stride));
                    if (!run(STREAK, to, textureOf(from), nullptr, nullptr, nullptr, b, false)) return false;
                    from = to;
                }
                if (!addInto(from, gain * kernelSum)) return false;
            }
            return true;
        };
        // the long thin rays on sl, then their short, twice-as-wide hot starts on sl + 1
        if (!streakRays(stars, sl, len, STREAK_PEAK / std::max(1.0, sigma))) return false;
        TitleTarget* coarse = down(stars, sl + 1, 7);
        if (!coarse || !streakRays(coarse, sl + 1, std::max(1.0, len * STREAK_CORE_LEN / 2), STREAK_CORE / std::max(1.0, sigma / 2))) return false;
    }
    return true;
}
