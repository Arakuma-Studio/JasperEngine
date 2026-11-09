// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleAnimRender.h"

#include "titleanim/TitleAnimSampling.h"
#include "titleart/TitleArtHash.h"
#include "titleart/TitleArtIO.h"
#include "titleart/anim/TitleAnimIO.h"
#include "titleart/anim/TitleAnimRealtime.h"
#include "titleart/text/TitleTextLayout.h"
#include "titleart/TitleArtRenderHost.h"
#include "util/Fnv.h"

#include <algorithm>
#include <chrono>
#include <list>
#include <cmath>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace ta = jasperengine::titleanim;
namespace tt = jasperengine::titleart;

namespace {

    // ---- pixels: linear premultiplied float RGBA ----

    struct Linear {
        int width = 0, height = 0;
        std::vector<float> px;
        void make(int w, int h){
            width = std::max(0, w);
            height = std::max(0, h);
            px.assign((size_t)width * (size_t)height * 4, 0.0f);
        }
    };

    float toLinear(float c){
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }

    float toSrgb(float c){
        c = std::max(0.0f, c);
        return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    }

    Linear linearOf(const texsynth::Image& image){
        Linear out;
        out.make(image.width, image.height);
        if (image.empty() || image.channels != 4) return out;
        // a table of the 256 codes
        static float table[256];
        static bool built = false;
        if (!built){
            for (int i = 0; i < 256; i++) table[i] = toLinear((float)i / 255.0f);
            built = true;
        }
        const bool sixteen = image.bitDepth == 16;
        for (size_t i = 0; i < image.pixelCount(); i++){
            float c[4];
            if (sixteen){
                const uint16_t* w = reinterpret_cast<const uint16_t*>(image.data.data()) + i * 4;
                for (int k = 0; k < 4; k++) c[k] = (float)w[k] / 65535.0f;
                for (int k = 0; k < 3; k++) c[k] = toLinear(c[k]);
            }else{
                const uint8_t* b = image.data.data() + i * 4;
                for (int k = 0; k < 3; k++) c[k] = table[b[k]];
                c[3] = (float)b[3] / 255.0f;
            }
            for (int k = 0; k < 3; k++) out.px[i * 4 + k] = c[k] * c[3];
            out.px[i * 4 + 3] = c[3];
        }
        return out;
    }

    texsynth::Image imageOf(const Linear& in){
        texsynth::Image out = texsynth::Image::make(in.width, in.height, 4, 8);
        for (size_t i = 0; i < (size_t)in.width * (size_t)in.height; i++){
            const float a = std::min(1.0f, std::max(0.0f, in.px[i * 4 + 3]));
            for (int k = 0; k < 3; k++){
                const float straight = a > 0.0f ? in.px[i * 4 + k] / a : 0.0f;
                out.data[i * 4 + k] = (uint8_t)std::lround(std::min(1.0f, toSrgb(straight)) * 255.0f);
            }
            out.data[i * 4 + 3] = (uint8_t)std::lround(a * 255.0f);
        }
        return out;
    }

    void sample(const Linear& src, double x, double y, float* out){
        // bilinear on pixel centres, transparent outside
        const double fx = x - 0.5, fy = y - 0.5;
        const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
        const float tx = (float)(fx - x0), ty = (float)(fy - y0);
        for (int k = 0; k < 4; k++) out[k] = 0.0f;
        for (int j = 0; j < 2; j++){
            for (int i = 0; i < 2; i++){
                const int sx = x0 + i, sy = y0 + j;
                if (sx < 0 || sy < 0 || sx >= src.width || sy >= src.height) continue;
                const float w = (i ? tx : 1.0f - tx) * (j ? ty : 1.0f - ty);
                const float* p = src.px.data() + ((size_t)sy * src.width + sx) * 4;
                for (int k = 0; k < 4; k++) out[k] += p[k] * w;
            }
        }
    }

    // a 2x3 affine in pixels: q = (a x + b y + c, d x + e y + f)
    using Affine = ta::FrameAffine;

    // `src` (its top-left at (ox, oy) in dst pixels) moved by `move` (dst pixels to dst pixels),
    // times opacity, tinted and flashed (linear, under its alpha), over `dst`
    void overMoved(Linear& dst, const Linear& src, double ox, double oy, const Affine& move, float opacity, const float* tint,
                   const float* flash){
        if (src.width <= 0 || src.height <= 0 || opacity <= 0.0f) return;
        const Affine inverse = move.inverse();
        // the destination box the moved source can reach
        double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
        for (int c = 0; c < 4; c++){
            const double x = ox + ((c & 1) ? src.width : 0), y = oy + ((c & 2) ? src.height : 0);
            double mx, my;
            move.apply(x, y, mx, my);
            minX = std::min(minX, mx); maxX = std::max(maxX, mx);
            minY = std::min(minY, my); maxY = std::max(maxY, my);
        }
        const int x0 = std::max(0, (int)std::floor(minX) - 1), x1 = std::min(dst.width, (int)std::ceil(maxX) + 1);
        const int y0 = std::max(0, (int)std::floor(minY) - 1), y1 = std::min(dst.height, (int)std::ceil(maxY) + 1);
        const bool identityColor = (!tint || (tint[0] == 1.0f && tint[1] == 1.0f && tint[2] == 1.0f)) &&
                                   (!flash || (flash[0] == 0.0f && flash[1] == 0.0f && flash[2] == 0.0f));
        for (int y = y0; y < y1; y++){
            for (int x = x0; x < x1; x++){
                double sx, sy;
                inverse.apply(x + 0.5, y + 0.5, sx, sy);
                float s[4];
                sample(src, sx - ox, sy - oy, s);
                if (s[3] <= 0.0f && s[0] <= 0.0f && s[1] <= 0.0f && s[2] <= 0.0f) continue;
                if (!identityColor){
                    for (int k = 0; k < 3; k++) s[k] = std::max(0.0f, s[k] * (tint ? tint[k] : 1.0f) + (flash ? flash[k] : 0.0f) * s[3]);
                }
                float* d = dst.px.data() + ((size_t)y * dst.width + x) * 4;
                for (int k = 0; k < 4; k++){
                    const float v = s[k] * opacity;
                    d[k] = v + d[k] * (1.0f - s[3] * opacity);
                }
            }
        }
    }

    void overAt(Linear& dst, const Linear& src, int ox, int oy){
        overMoved(dst, src, ox, oy, Affine(), 1.0f, nullptr, nullptr);
    }

    // layer space -> document space for a layer in the tree, as the renderer places it
    bool chainOf(const tt::Json& layers, const std::string& id, std::vector<const tt::Json*>& chain){
        if (!layers.is_array()) return false;
        for (const tt::Json& l : layers){
            chain.push_back(&l);
            if (l.value("id", std::string()) == id) return true;
            if (tt::layerTypeOf(l) == tt::LayerType::Group && chainOf(tt::layerChildren(l), id, chain)) return true;
            chain.pop_back();
        }
        return false;
    }

    Affine layerToDocument(const tt::TitleArtDoc& doc, const std::string& id){
        std::vector<const tt::Json*> chain;
        Affine m;
        if (!chainOf(doc.layers(), id, chain)) return m;
        for (const tt::Json* l : chain){
            const tt::TitleTransform t = tt::decodeTransform(l->value("transform", tt::Json::object()));
            const double r = -t.rotation * 3.14159265358979323846 / 180.0;
            const double sx = (t.flipX ? -1.0 : 1.0) * t.scale * t.stretch.x, sy = (t.flipY ? -1.0 : 1.0) * t.scale * t.stretch.y;
            Affine local;
            local.a = std::cos(r) * sx;
            local.b = -std::sin(r) * sy;
            local.d = std::sin(r) * sx;
            local.e = std::cos(r) * sy;
            local.c = t.position.x;
            local.f = t.position.y;
            m = local.then(m);
        }
        return m;
    }

    // the layers drawn below and above a layer, at every level of its chain (whole siblings)
    void platesOf(const tt::TitleArtDoc& doc, const std::string& id, std::vector<std::string>& below, std::vector<std::string>& above){
        std::vector<const tt::Json*> chain;
        if (!chainOf(doc.layers(), id, chain)) return;
        const tt::Json* siblings = &doc.layers();
        for (const tt::Json* at : chain){
            bool before = true;
            for (const tt::Json& s : *siblings){
                if (&s == at){
                    before = false;
                    continue;
                }
                (before ? below : above).push_back(s.value("id", std::string()));
            }
            if (tt::layerTypeOf(*at) == tt::LayerType::Group) siblings = &tt::layerChildren(*at);
        }
    }

    uint64_t baseKey(const tt::RenderRequest& request);

    uint64_t overridesHash(const std::vector<tt::FieldOverride>& overrides){
        uint64_t h = 0x55;
        for (const tt::FieldOverride& o : overrides) h = fnv1a64(o.layerId + "\x1f" + o.path + "\x1f" + o.value.dump(), h);
        return h;
    }

    double fract(double v){
        return v - std::floor(v);
    }

    float hash01(float seed, float index){
        // title_fx.glsl's titleFxHash (Hoskins' Hash without Sine, MIT)
        float p0 = seed * 0.1031f + 0.123f, p1 = index * 0.11369f + 0.456f;
        float p3[3] = {(float)fract(p0 * 0.1031), (float)fract(p1 * 0.1030), (float)fract(p0 * 0.0973)};
        const float d = p3[0] * (p3[1] + 33.33f) + p3[1] * (p3[2] + 33.33f) + p3[2] * (p3[0] + 33.33f);
        for (float& v : p3) v += d;
        return (float)fract((p3[0] + p3[1]) * p3[2]);
    }

    float twinkle(double t){
        const double f = fract(t);
        const double x = std::min(1.0, std::max(0.0, f / 0.25));
        const double bump = std::sin(3.14159265 * x);
        return f < 0.25 ? (float)(bump * bump) : 0.0f;
    }

}

// ==================================================================== names

const char* editor::titleAnimRenderAsName(TitleAnimRenderAs as){
    switch (as){
        case TitleAnimRenderAs::Exact: return "exact";
        case TitleAnimRenderAs::Baked: return "baked";
        case TitleAnimRenderAs::Realtime: return "realtime";
        case TitleAnimRenderAs::GlyphAtlas: return "glyph_atlas";
    }
    return "exact";
}

bool editor::titleAnimRenderAsFromName(const std::string& name, TitleAnimRenderAs& out){
    if (name == "exact") out = TitleAnimRenderAs::Exact;
    else if (name == "baked") out = TitleAnimRenderAs::Baked;
    else if (name == "realtime") out = TitleAnimRenderAs::Realtime;
    else if (name == "glyph_atlas") out = TitleAnimRenderAs::GlyphAtlas;
    else return false;
    return true;
}

// ==================================================================== Live Effects (S81 R33-R37)

TitleLiveEffectValues editor::liveEffectsOf(const ta::TitlePoseSummary& s){
    TitleLiveEffectValues v;
    auto set = [&](ta::TitleField f){ return s.liveSet[(size_t)f]; };
    auto value = [&](ta::TitleField f){ return (float)s.live[(size_t)f]; };
    v.sweep = set(ta::TitleField::SweepPosition) || set(ta::TitleField::SweepAngle) || set(ta::TitleField::SweepWidth);
    if (set(ta::TitleField::SweepPosition)) v.sweepPosition = value(ta::TitleField::SweepPosition);
    if (set(ta::TitleField::SweepAngle)) v.sweepAngle = value(ta::TitleField::SweepAngle);
    if (set(ta::TitleField::SweepWidth)) v.sweepWidth = value(ta::TitleField::SweepWidth);
    v.glow = set(ta::TitleField::GlowPulse);
    if (v.glow) v.glowPulse = value(ta::TitleField::GlowPulse);
    v.tint = set(ta::TitleField::TintColor) || set(ta::TitleField::TintAmount);
    if (set(ta::TitleField::TintAmount)) v.tintAmount = value(ta::TitleField::TintAmount);
    if (set(ta::TitleField::TintColor)) for (int i = 0; i < 3; i++) v.tintColor[i] = toLinear((float)s.tintColor[i]);
    v.glints = set(ta::TitleField::GlintAmount);
    if (v.glints) v.glintAmount = value(ta::TitleField::GlintAmount);
    return v;
}

void editor::applyLiveEffects(texsynth::Image& frame, const TitleLiveEffectValues& v, const std::vector<TitleArtAnchor>& anchors, const texsynth::Image* glow){
    if (frame.empty() || frame.channels != 4 || frame.bitDepth != 8) return;
    if (!v.sweep && !v.tint && !(v.glow && glow) && !(v.glints && !anchors.empty())) return;
    const int w = frame.width, h = frame.height;
    const double angle = v.sweepAngle * 3.14159265358979323846 / 180.0;
    const double dx = std::cos(angle), dy = std::sin(angle);
    const double extent = std::fabs(w * dx) + std::fabs(h * dy);
    const double halfWidth = std::max(1e-4f, v.sweepWidth) * 0.5;
    const double center = -0.5 - halfWidth + (1.0 + 2.0 * halfWidth) * std::min(1.0f, std::max(0.0f, v.sweepPosition));
    const double softness = std::min(1.0f, std::max(0.0f, v.sweepSoftness));
    const double inner = std::min(halfWidth * (1.0 - softness), halfWidth * 0.999);
    auto smooth = [](double a, double b, double x){
        const double t = std::min(1.0, std::max(0.0, (x - a) / (b - a)));
        return t * t * (3.0 - 2.0 * t);
    };
    for (int y = 0; y < h; y++){
        for (int x = 0; x < w; x++){
            uint8_t* p = frame.pixel(x, y);
            float c[4] = {toLinear(p[0] / 255.0f), toLinear(p[1] / 255.0f), toLinear(p[2] / 255.0f), p[3] / 255.0f};
            if (v.tint){
                for (int k = 0; k < 3; k++){
                    const float base = toSrgb(c[k]), tint = toSrgb(v.tintColor[k]);
                    const float overlay = base < 0.5f ? 2.0f * base * tint : 1.0f - 2.0f * (1.0f - base) * (1.0f - tint);
                    c[k] = c[k] + (toLinear(overlay) - c[k]) * v.tintAmount * v.tintColor[3];
                }
            }
            if (v.sweep){
                const double px = x + 0.5 - w * 0.5, py = y + 0.5 - h * 0.5;
                const double s = (px * dx + py * dy) / std::max(extent, 1e-4);
                const double band = 1.0 - smooth(inner, halfWidth, std::fabs(s - center));
                for (int k = 0; k < 3; k++) c[k] += v.sweepColor[k] * (float)(v.sweepColor[3] * band);
            }
            // the light the glow and the glints add (premultiplied colour and coverage)
            float light[3] = {0.0f, 0.0f, 0.0f};
            float coverage = 0.0f;
            if (v.glow && glow && glow->width == w && glow->height == h){
                const uint8_t* g = glow->pixel(x, y);
                const float ga = g[3] / 255.0f;
                const float pulse = v.glowPulse * v.glowColor[3];
                for (int k = 0; k < 3; k++){
                    const float added = toLinear(g[k] / 255.0f) * ga * v.glowColor[k] * pulse;
                    light[k] += added;
                    coverage = std::max(coverage, std::min(1.0f, added));
                }
            }
            if (v.glints){
                for (size_t i = 0; i < anchors.size() && i < TITLE_ART_MAX_ANCHORS; i++){
                    const TitleArtAnchor& a = anchors[i];
                    const double radius = std::max(0.5, (double)v.glintSize * (a.size > 0.0f ? a.size : 1.0f) * h);
                    const double ddx = (x + 0.5 - a.x * w) / radius, ddy = (y + 0.5 - a.y * h) / radius;
                    const float brightness = v.glintAmount * a.strength * a.fade * twinkle(v.liveTime * v.glintRate + hash01(v.glintSeed, (float)i));
                    const double core = std::max(0.0, 1.0 - std::sqrt(ddx * ddx + ddy * ddy));
                    const double rayX = std::max(0.0, 1.0 - std::fabs(ddx)) * std::max(0.0, 1.0 - std::fabs(ddy) * 10.0);
                    const double rayY = std::max(0.0, 1.0 - std::fabs(ddy)) * std::max(0.0, 1.0 - std::fabs(ddx) * 10.0);
                    const float star = (float)(core * core * core + rayX * rayX + rayY * rayY);
                    for (int k = 0; k < 3; k++){
                        const float added = v.glintColor[k] * v.glintColor[3] * brightness * star;
                        light[k] += added;
                        coverage = std::max(coverage, std::min(1.0f, added));
                    }
                }
            }
            // titleFxCompose: the light over the image as its own coverage
            const float alpha = std::min(1.0f, std::max(0.0f, c[3] + coverage * (1.0f - c[3])));
            for (int k = 0; k < 3; k++){
                const float premultiplied = c[k] * c[3] + light[k];
                const float straight = alpha > 1e-5f ? premultiplied / alpha : 0.0f;
                p[k] = (uint8_t)std::lround(std::min(1.0f, toSrgb(straight)) * 255.0f);
            }
            p[3] = (uint8_t)std::lround(alpha * 255.0f);
        }
    }
}

// ==================================================================== the renderer

struct TitleAnimRenderer::Cache {
    // glyph cells by (pose values, cluster): back and front, their pixel origins
    struct Cells {
        Linear back, front;
        int backX = 0, backY = 0, frontX = 0, frontY = 0;
        bool ok = false;
    };
    std::map<std::pair<uint64_t, uint32_t>, Cells> cells;
    std::map<uint64_t, Linear> glowImages;
    std::unique_ptr<tt::TitleTextLayout> layout;
    // R39: base renders by what they rendered (re-render values, scale, region)
    std::map<uint64_t, TitleRenderedImage> bases;
    std::list<uint64_t> baseOrder;
    // the base render being stepped across frames
    std::unique_ptr<tt::TitleRenderJob> sliced;
    uint64_t slicedKey = 0;

    void storeBase(uint64_t key, const TitleRenderedImage& image){
        if (bases.count(key)) return;
        bases[key] = image;
        baseOrder.push_back(key);
        while (baseOrder.size() > 32){
            bases.erase(baseOrder.front());
            baseOrder.pop_front();
        }
    }
};

struct TitleAnimRenderer::PoseSetup {
    const ta::CompiledTitleAnimation* animation = nullptr;
    tt::PlayerPose pose;
    std::vector<tt::FieldOverride> overrides;
    tt::RenderRequest base;
    std::string cellLayer;
    ta::TitleAnimPlayer player;
};

TitleAnimRenderer::TitleAnimRenderer(Project* project, std::shared_ptr<const tt::TitleArtDoc> doc)
    : project(project), doc(std::move(doc)), cache(std::make_unique<Cache>()){
    const tt::AssetResolver* resolver = TitleArtRenderHost::editor().resolver(project);
    cache->layout = std::make_unique<tt::TitleTextLayout>(resolver);
    lib = tt::compileAnimations(*this->doc, cache->layout.get(), &problems);
    // the companion's frame before a bake exists: the canvas
    still.frame = {0, 0, this->doc->canvasWidth(), this->doc->canvasHeight()};
    still.originX = this->doc->canvasWidth() * 0.5;
    still.originY = this->doc->canvasHeight() * 0.5;
}

TitleAnimRenderer::~TitleAnimRenderer() = default;

void TitleAnimRenderer::setStillFrame(const TitleArtBakeInfo& bake){
    if (bake.frame.width > 0 && bake.frame.height > 0) still = bake;
}

const tt::TitleAnimPlan& TitleAnimRenderer::plan(TitleArtRenderMode mode){
    auto it = plans.find((int)mode);
    if (it == plans.end()) it = plans.emplace((int)mode, tt::buildAnimPlan(*doc, lib->animations, mode)).first;
    return it->second;
}

ta::TitlePoseSummary TitleAnimRenderer::summaryAt(const std::string& animation, double time) const{
    ta::TitlePoseSummary s;
    ta::TitleAnimPlayer player;
    player.setLibrary(lib);
    if (player.sample(animation, time)) player.summary(nullptr, s);
    return s;
}

namespace {
    uint64_t baseKey(const tt::RenderRequest& request){
        uint64_t h = overridesHash(request.overrides);
        h = tt::hashCombine(h, (uint64_t)std::llround((double)request.pxScale * 1000.0));
        return tt::hashCombine(h, (((uint64_t)(uint32_t)request.region.x) << 32) ^ (uint32_t)request.region.y ^ ((uint64_t)request.region.width << 16) ^ request.region.height);
    }
}

// R39: a pose that differs only in title-live, layer-live or glyph values reuses this image
bool TitleAnimRenderer::baseRender(const tt::RenderRequest& request, TitleRenderedImage& image, std::string& error){
    const uint64_t key = baseKey(request);
    auto found = cache->bases.find(key);
    if (found != cache->bases.end()){
        image = found->second;
        return true;
    }
    if (!TitleArtRenderHost::editor().render(project, request, image, error)) return false;
    cache->storeBase(key, image);
    return true;
}

// R39: the pose's base image advanced within `budgetMs` of this frame; true once it is ready, and
// render() then only composes (the glyph-cell path renders its plates in render())
bool TitleAnimRenderer::advancePose(const TitleAnimFrameRequest& request, double budgetMs, std::string& error){
    PoseSetup setup;
    TitleAnimFrame scratch;
    if (!poseSetup(request, setup, scratch, error)) return false;
    if (!setup.cellLayer.empty()) return true;
    tt::RenderRequest r = setup.base;
    r.overrides = setup.overrides;
    const uint64_t key = baseKey(r);
    if (cache->bases.count(key)) return true;
    if (cache->slicedKey != key || !cache->sliced){
        cache->sliced = TitleArtRenderHost::editor().begin(project, r, error);
        cache->slicedKey = key;
        if (!cache->sliced) return false;
    }
    const auto start = std::chrono::steady_clock::now();
    while (!cache->sliced->done() && !cache->sliced->failed()){
        cache->sliced->step(tt::StepBudget{1, 0});
        if (cache->sliced->waiting()) break;
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >= budgetMs) break;
    }
    if (cache->sliced->failed()){
        error = cache->sliced->error();
        cache->sliced.reset();
        return false;
    }
    if (!cache->sliced->done()) return false;
    TitleRenderedImage image;
    const bool ok = TitleArtRenderHost::readBack(*cache->sliced, 8, image, error);
    cache->sliced.reset();
    if (!ok) return false;
    cache->storeBase(key, image);
    return true;
}

bool TitleAnimRenderer::poseSetup(const TitleAnimFrameRequest& request, PoseSetup& setup, TitleAnimFrame& out, std::string& error){
    const ta::CompiledTitleAnimation* animation = lib->find(request.animation);
    setup.animation = animation;
    if (!animation){
        error = "no animation '" + request.animation + "'";
        return false;
    }
    ta::TitleAnimPlayer& player = setup.player;
    player.setLibrary(lib);
    player.sample(request.animation, request.time);
    out.localTime = player.baseAnimation()->localTime();

    // which tracks this `as` plays besides the companion's, and at which time
    const TitleArtRenderMode mode = request.as == TitleAnimRenderAs::Baked ? TitleArtRenderMode::BAKED
                                   : request.as == TitleAnimRenderAs::Realtime ? TitleArtRenderMode::REALTIME
                                   : request.as == TitleAnimRenderAs::GlyphAtlas ? TitleArtRenderMode::GLYPH_ATLAS : TitleArtRenderMode::REALTIME;
    std::map<const ta::CompiledTitleTrack*, bool> played;
    if (request.as != TitleAnimRenderAs::Exact){
        const tt::AnimationPlan* ap = plan(mode).find(animation->name);
        if (ap){
            for (const tt::TrackPlan& t : ap->tracks){
                if (t.track >= animation->tracks.size()) continue;
                const bool plays = t.play != tt::TrackPlay::NotPlayed && t.how != "companion";
                if (plays) played[&animation->tracks[t.track]] = true;
                if (t.play == tt::TrackPlay::NotPlayed && t.code != "muted")
                    out.notPlayed.push_back((t.layer.empty() ? std::string("title") : t.layer) + "/" + t.field + ": " + t.reason);
            }
        }
    }
    const tt::TrackFilter include = request.as == TitleAnimRenderAs::Exact ? tt::TrackFilter()
                                                                          : [&](const ta::CompiledTitleTrack& t){ return played.count(&t) > 0; };
    // Baked: frames hold the values of their frame time (k / fps); Realtime and Glyph Atlas
    // re-render at rerender_fps
    float pxScale = std::max(0.05f, request.pxScale);
    tt::PlayerPose& pose = setup.pose;
    if (request.as == TitleAnimRenderAs::Baked){
        const double fps = std::min((double)ta::TITLE_ANIM_MAX_FLIPBOOK_FPS, std::max(1.0, (double)animation->flipbookSettings.fps));
        const double frameTime = std::floor(out.localTime * fps + 1e-6) / fps;
        ta::TitleAnimPlayer frames;
        frames.setLibrary(lib);
        frames.sample(request.animation, frameTime);
        tt::playerPose(*doc, frames, false, include, pose);
        pxScale *= std::max(0.25f, std::min(1.0f, animation->flipbookSettings.scale));
    }else{
        tt::playerPose(*doc, player, request.as != TitleAnimRenderAs::Exact, include, pose);
    }

    std::vector<tt::FieldOverride>& overrides = setup.overrides;
    overrides = request.overrides;
    for (tt::FieldOverride& o : pose.values.overrides()) overrides.push_back(std::move(o));

    tt::RenderRequest& base = setup.base;
    base.doc = doc;
    base.pxScale = pxScale;
    base.region = request.region;
    base.testFlags = request.testFlags;
    base.output = tt::TitleOutputFormat::Srgb8;

    // the glyph-cell text layer (the first with a pose that moves anything)
    std::string& cellLayer = setup.cellLayer;
    for (const auto& [layer, poses] : pose.glyphs){
        for (const auto& [cluster, g] : poses) if (!g.identity()){ cellLayer = layer; break; }
        if (!cellLayer.empty()) break;
    }
    if (cellLayer.empty() && request.forceCells){
        const std::vector<std::string> layers = tt::cellLayersOf(*doc, *animation);
        if (!layers.empty()) cellLayer = layers.front();
    }
    return true;
}

bool TitleAnimRenderer::render(const TitleAnimFrameRequest& request, TitleAnimFrame& out, std::string& error){
    out = TitleAnimFrame();
    PoseSetup setup;
    if (!poseSetup(request, setup, out, error)) return false;
    const ta::CompiledTitleAnimation* animation = setup.animation;
    (void)animation;
    ta::TitleAnimPlayer& player = setup.player;
    tt::PlayerPose& pose = setup.pose;
    std::vector<tt::FieldOverride>& overrides = setup.overrides;
    tt::RenderRequest& base = setup.base;
    std::string& cellLayer = setup.cellLayer;
    const float pxScale = base.pxScale;
    Linear composed;
    tt::TitleRectI frame;
    if (cellLayer.empty()){
        tt::RenderRequest r = base;
        r.overrides = overrides;
        TitleRenderedImage image;
        if (!baseRender(r, image, error)) return false;
        frame = image.frame;
        out.problems = image.problems;
        composed = linearOf(image.image);
    }else{
        // R15: back plate, back cells, front cells, front plate
        out.cells = true;
        std::vector<std::string> below, above;
        platesOf(*doc, cellLayer, below, above);
        auto plate = [&](const std::vector<std::string>& hidden, bool transparentCanvas, TitleRenderedImage& image) -> bool{
            tt::RenderRequest r = base;
            r.overrides = overrides;
            r.overrides.push_back({cellLayer, "visible", false});
            for (const std::string& id : hidden) r.overrides.push_back({id, "visible", false});
            if (transparentCanvas) r.overrides.push_back({"", "canvas.background.transparent", true});
            return baseRender(r, image, error);
        };
        TitleRenderedImage backPlate, frontPlate;
        if (!plate(above, false, backPlate) || !plate(below, true, frontPlate)) return false;
        frame = backPlate.frame;
        out.problems = backPlate.problems;
        composed = linearOf(backPlate.image);
        const Linear front = linearOf(frontPlate.image);
        const tt::Json* layer = tt::findLayer(doc->root, cellLayer);
        // the layer as the pose renders it (re-render values on it apply to its cells)
        tt::TitleArtDoc posed = *doc;
        tt::applyFieldOverrides(posed, overrides, nullptr);
        const tt::Json* posedLayer = tt::findLayer(posed.root, cellLayer);
        const tt::LaidOutText& text = cache->layout->layout(posedLayer ? *posedLayer : *layer);
        const Affine toDoc = layerToDocument(posed, cellLayer);
        const double size = (posedLayer ? *posedLayer : *layer).value("size", 200.0);
        const uint64_t valuesHash = overridesHash(overrides);
        const std::map<uint32_t, tt::GlyphPose>& poses = pose.glyphs[cellLayer];
        const double canvasW = doc->canvasWidth(), canvasH = doc->canvasHeight();
        // document (y up, origin at the centre) -> this image's pixels
        auto toPixels = [&](double dx, double dy, double& px, double& py){
            px = (canvasW * 0.5 + dx - frame.x) * pxScale;
            py = (canvasH * 0.5 - dy - frame.y) * pxScale;
        };
        struct Placed { Cache::Cells* cells; Affine move; float opacity; float tint[3]; float flash[3]; };
        std::vector<Placed> placed;
        for (size_t i = 0; i < text.clusters.size(); i++){
            const tt::LaidOutCluster& c = text.clusters[i];
            if (c.inkBox.empty()) continue;
            auto key = std::make_pair(valuesHash, (uint32_t)i);
            Cache::Cells& cells = cache->cells[key];
            if (!cells.ok){
                tt::RenderRequest r = base;
                r.overrides = overrides;
                r.glyphCell = tt::GlyphCellRequest{cellLayer, (uint32_t)i, tt::GlyphCellRequest::Back};
                std::unique_ptr<tt::TitleRenderJob> job = TitleArtRenderHost::editor().runToEnd(project, r, error);
                if (!job) return false;
                if (job->failed() || !job->done()){
                    error = job->error().empty() ? std::string("the glyph cell did not render") : job->error();
                    return false;
                }
                TitleRenderedImage b, f;
                const tt::TitleSplits& splits = job->splits();
                if (splits.glyphBack.target && TitleArtRenderHost::readImage(splits.glyphBack, 8, b, error)){
                    cells.back = linearOf(b.image);
                    cells.backX = splits.glyphBack.pixels.x;
                    cells.backY = splits.glyphBack.pixels.y;
                }
                if (splits.glyphFront.target && TitleArtRenderHost::readImage(splits.glyphFront, 8, f, error)){
                    cells.front = linearOf(f.image);
                    cells.frontX = splits.glyphFront.pixels.x;
                    cells.frontY = splits.glyphFront.pixels.y;
                }
                cells.ok = true;
            }
            auto found = poses.find(c.index);
            const tt::GlyphPose g = found != poses.end() ? found->second : tt::GlyphPose();
            // the pivot in layer space (the ink box's centre, baseline, bottom or top), then pixels
            double lx = c.inkBox.x + c.inkBox.width * 0.5, ly = c.inkBox.y + c.inkBox.height * 0.5;
            if (g.pivot == ta::TrackPivot::Bottom) ly = c.inkBox.y;
            else if (g.pivot == ta::TrackPivot::Top) ly = c.inkBox.y + c.inkBox.height;
            else if (g.pivot == ta::TrackPivot::Baseline) ly = c.pen.y;
            double dx, dy, px, py;
            toDoc.apply(lx, ly, dx, dy);
            toPixels(dx, dy, px, py);
            // the offset (% of the layer's size, layer axes) in pixels
            double ox, oy;
            toDoc.apply(lx + g.offset[0] * size / 100.0, ly + g.offset[1] * size / 100.0, ox, oy);
            double qx, qy;
            toPixels(ox, oy, qx, qy);
            const double radians = g.rotation * 3.14159265358979323846 / 180.0;
            const double cs = std::cos(radians), sn = std::sin(radians);
            Affine move;
            move.a = cs * g.scale[0];
            move.b = -sn * g.scale[1];
            move.d = sn * g.scale[0];
            move.e = cs * g.scale[1];
            // p' = Q + R S (p - P): about the pivot P, to the offset pivot Q
            move.c = qx - (move.a * px + move.b * py);
            move.f = qy - (move.d * px + move.e * py);
            Placed p;
            p.cells = &cells;
            p.move = move;
            p.opacity = (float)g.opacity * (float)std::min(1.0, std::max(0.0, (posedLayer ? *posedLayer : *layer).value("opacity", 1.0)));
            for (int k = 0; k < 3; k++){
                p.tint[k] = toLinear((float)g.tint[k]);
                p.flash[k] = toLinear((float)g.flash[k]) * (float)g.flashAmount;
            }
            placed.push_back(p);
        }
        // cell origins are in the canvas's scaled pixels; this image starts at the frame's
        const double fx = frame.x * pxScale, fy = frame.y * pxScale;
        for (const Placed& p : placed)
            overMoved(composed, p.cells->back, p.cells->backX - fx, p.cells->backY - fy, p.move, p.opacity, p.tint, p.flash);
        for (const Placed& p : placed)
            overMoved(composed, p.cells->front, p.cells->frontX - fx, p.cells->frontY - fy, p.move, p.opacity, p.tint, p.flash);
        overAt(composed, front, 0, 0);
        if (layer->value("blend", std::string("normal")) != "normal"){
            tt::TitleProblem problem;
            problem.code = "glyph_cells_blend";
            problem.layer = cellLayer;
            problem.message = "the glyph cells of a text layer that blends other than normal compose as normal";
            out.problems.push_back(problem);
        }
    }

    // the companion's draw (R13): Live Effects, flash and opacity, then the title's transform
    texsynth::Image image = imageOf(composed);
    if (request.companion){
        ta::TitlePoseSummary summary;
        player.summary(nullptr, summary);
        if (request.liveEffects){
            TitleLiveEffectValues live = liveEffectsOf(summary);
            live.liveTime = (float)out.localTime;
            applyLiveEffects(image, live, still.anchors, nullptr);
        }
        if (summary.transformed){
            Linear lin = linearOf(image);
            const float opacity = (float)std::min(1.0, std::max(0.0, summary.opacity));
            float flash[3];
            for (int k = 0; k < 3; k++) flash[k] = toLinear((float)summary.flashColor[k]) * (float)summary.flashAmount;
            // the frame-normalised affine of the still's frame, in this image's pixels
            ta::TitleFrameGeometry g;
            g.frameX = still.frame.x;
            g.frameY = still.frame.y;
            g.frameWidth = std::max(1, still.frame.width);
            g.frameHeight = std::max(1, still.frame.height);
            g.originX = still.originX;
            g.originY = still.originY;
            g.inkX = still.alphaBounds.x;
            g.inkY = still.alphaBounds.y;
            g.inkWidth = still.alphaBounds.width;
            g.inkHeight = still.alphaBounds.height;
            const ta::FrameAffine n = ta::titleFrameAffine(summary, g, ta::titlePivotOf(*animation));
            // pixels (this image) -> frame-normalised -> moved -> pixels
            ta::FrameAffine toFrame;
            toFrame.a = 1.0 / (g.frameWidth * pxScale);
            toFrame.c = (frame.x - g.frameX) / g.frameWidth;
            toFrame.e = 1.0 / (g.frameHeight * pxScale);
            toFrame.f = (frame.y - g.frameY) / g.frameHeight;
            const ta::FrameAffine move = toFrame.then(n).then(toFrame.inverse());
            Linear moved;
            moved.make(lin.width, lin.height);
            overMoved(moved, lin, 0, 0, move, opacity, nullptr, flash);
            image = imageOf(moved);
        }
    }
    out.image = std::move(image);
    out.frame = frame;
    out.alphaBounds = TitleArtRenderHost::alphaBoundsOf(out.image);
    size_t covered = 0;
    for (size_t i = 0; i < out.image.pixelCount(); i++) covered += out.image.data[i * 4 + 3] > 0 ? 1 : 0;
    out.coveragePct = out.image.pixelCount() ? 100.0 * (double)covered / (double)out.image.pixelCount() : 0.0;
    out.poseHash = fnv1a64(out.image.data.data(), out.image.data.size());
    return true;
}
