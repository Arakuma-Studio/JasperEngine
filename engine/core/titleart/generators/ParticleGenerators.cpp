// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Ported from Pachislop src/generators/particles.ts and src/generators/inkSplatter.ts by SafEight, used with the author's permission.
//
// The Particles generators (docs/specs/S83-title-effects-generators.md §6.7.1, §6.7.2): Particles
// (a closed-form sampler of particle states, D6, R25) and Ink Splatter (seeded ink drops thrown
// off the followed layer's silhouette). Their CPU layouts are analysis/ParticleLayout; this file
// reads the followed layer, turns the records into QuadBatch draws and declares the definitions.

#include "titleart/TitleArtHash.h"
#include "titleart/analysis/ParticleLayout.h"
#include "titleart/analysis/ReadGrid.h"
#include "titleart/generators/TitleGen.h"
#include "titleart/generators/TitleGenFamilies.h"
#include "titleart/gpu/QuadBatch.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <string_view>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::analysis;
using namespace jasperengine::titleart::fxparams;
using namespace jasperengine::titleart::gen;

namespace {

    constexpr double DEG = 3.14159265358979323846 / 180.0;
    // the smallest glint ray half-width at the core, canvas px (Pachislop GLINT_MIN_THIN)
    constexpr double GLINT_MIN_THIN = 0.6;
    // Ink Splatter's content grid, and the coarse grid the Particles layer emitter and the
    // followed shape emitters measure the ink box on: one readback when both follow a layer
    constexpr double INK_GRID_CELL = 8.0;
    constexpr size_t DRAW_CACHE = 64;

    // ---- Particles ----

    // the params the vertex stage animates: a change re-runs no layout
    const char* const MOTION_KEYS[] = {"time", "loop", "gravity", "spin", "twinkle", "fade_in", "fade_out"};

    // the records as the two draws: the main shape (solids far to near) and the mixed-in glints
    struct ParticleDraws {
        QuadBatch shapes, glints;
    };

    struct CachedDraws {
        uint64_t key = 0;
        std::shared_ptr<const ParticleDraws> draws;
    };

    // the last layout per generator layer: records are rebuilt only when what they depend on
    // changes (§6.7.1 "CPU builds ... on X changes"); a live frame only changes uniforms
    std::map<std::string, CachedDraws>& drawCache() {
        static std::map<std::string, CachedDraws> cache;
        return cache;
    }

    QuadInstance quadOf(const ParticleRecord& r) {
        QuadInstance q;
        q.cx = r.x;
        q.cy = r.y;
        q.w = r.w;
        q.h = r.h;
        q.rotation = r.rot;
        for (int i = 0; i < 4; i++) q.color[i] = r.color[i];
        q.extra[0] = r.blur;
        q.extra[1] = r.aspect;
        q.extra[2] = r.phase;
        // glints: GLINT_MIN_THIN in half quad heights
        q.extra[3] = r.h > 0.0f ? (float)(2.0 * GLINT_MIN_THIN / r.h) : 0.0f;
        q.more[0] = r.mx;
        q.more[1] = r.my;
        q.more[2] = r.motionPhase;
        return q;
    }

    std::shared_ptr<const ParticleDraws> drawsOf(std::vector<ParticleRecord> list, const ParticleLook& look) {
        auto draws = std::make_shared<ParticleDraws>();
        std::vector<ParticleRecord> shapes;
        for (const ParticleRecord& r : list) {
            if (r.glint) draws->glints.add(quadOf(r));
            else shapes.push_back(r);
        }
        // opaque shapes far to near (a stable sort, as JavaScript's)
        if (particleShapeSolid(look.shape)) {
            std::stable_sort(shapes.begin(), shapes.end(), [](const ParticleRecord& a, const ParticleRecord& b) { return a.z < b.z; });
        }
        for (const ParticleRecord& r : shapes) draws->shapes.add(quadOf(r));
        return draws;
    }

    uint64_t layoutKey(const GeneratorContext& ctx, const ParamValues& p, uint64_t sourceKey, const TitleAffine& documentToSpace) {
        Json params = p.json();
        if (params.is_object()) {
            for (const char* k : MOTION_KEYS) params.erase(k);
        }
        const double d[9] = {documentToSpace.a, documentToSpace.b,  documentToSpace.c,  documentToSpace.d,  documentToSpace.tx,
                             documentToSpace.ty, (double)ctx.canvasWidth, (double)ctx.canvasHeight, (double)ctx.seed};
        const uint64_t h = hashCombine(fnv1a64(params.dump()), fnv1a64(std::string_view(reinterpret_cast<const char*>(d), sizeof(d))));
        return hashCombine(h, sourceKey);
    }

    std::shared_ptr<const ParticleDraws> cachedDraws(const GeneratorContext& ctx, uint64_t key) {
        auto hit = drawCache().find(ctx.layerId);
        return hit != drawCache().end() && hit->second.key == key ? hit->second.draws : nullptr;
    }

    void keepDraws(const GeneratorContext& ctx, uint64_t key, const std::shared_ptr<const ParticleDraws>& draws) {
        auto& cache = drawCache();
        if (cache.size() >= DRAW_CACHE && cache.find(ctx.layerId) == cache.end()) cache.clear();
        cache[ctx.layerId] = {key, draws};
    }

    // the followed layer's content on Pachislop's lattice (contentGrid): its ink box from the
    // coarse frame grid, then the Color grid around it; null when it has no ink or a readback
    // could not run yet
    std::shared_ptr<const Grid> contentGrid(const GeneratorContext& ctx) {
        auto coarse = readGrid(ctx, *ctx.followContent, ctx.followContentKey, frameGridSpec(ctx, INK_GRID_CELL));
        double box[4];
        if (!coarse || !gridInkBox(*coarse, 0.0f, box)) return nullptr;
        return readGrid(ctx, *ctx.followContent, ctx.followContentKey, particleGridSpec(box));
    }

    void drawParticles(const GeneratorContext& ctx, TargetRef& target, const ParticleDraws& draws, const ParticleLook& look, const ParamValues& p,
                       const TitleAffine& space) {
        const ParticleMotion m = readParticleMotion(p);
        auto setup = [&](ParticleShape shape, bool light) {
            const bool spins = !particleShapeStreaked(shape);
            return [=, &look, &m](TitlePass& pass) {
                pass.set(6, (float)(int)shape, light ? 1.0f : 0.0f, (float)look.ring, (float)look.sheen);
                // our spin is clockwise on screen; the records turn counter-clockwise (+y up)
                pass.set(7, (float)m.time, m.loop ? 1.0f : 0.0f, (float)m.gravity, (float)(-m.spin * DEG));
                pass.set(8, (float)m.twinkle, (float)m.fadeIn, (float)m.fadeOut, spins ? 1.0f : 0.0f);
            };
        };
        const bool light = !particleShapeSolid(look.shape);
        draws.shapes.draw(ctx, GenProgram::QuadsParticles, "particles", target, space, light ? QuadBlend::Add : QuadBlend::Normal, 1.5f,
                        setup(look.shape, light));
        draws.glints.draw(ctx, GenProgram::QuadsParticles, "particle glints", target, space, QuadBlend::Add, 1.5f,
                          setup(ParticleShape::Glint4, true));
    }

    void renderParticles(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        ctx.runner.clear(target);
        const ParticleLook look = readParticleLook(p, seedOf(ctx, p));
        if (look.count == 0) return;
        const std::string em = p.string("emitter");
        if (em == "layer") {
            // the followed layer's own pixels, before its effects, in canvas px: the generator's
            // transform is an offset on top (identity = exactly on the layer)
            if (!ctx.followContent) return;
            const uint64_t key = layoutKey(ctx, p, ctx.followContentKey, TitleAffine{});
            std::shared_ptr<const ParticleDraws> draws = cachedDraws(ctx, key);
            if (!draws) {
                const std::shared_ptr<const Grid> grid = contentGrid(ctx);
                EmitMap map;
                if (!grid || !buildEmitMap(*grid, p.string("source") == "edge", p.number("edge_width"), map)) return;
                std::vector<ParticleRecord> list = layoutParticles(p, look, ParticleSpace{}, &map);
                // front / back: the half drawn above or below the followed layer (a pair of layers)
                const std::string split = p.string("split");
                if (split == "front" || split == "back") {
                    const bool front = split == "front";
                    list.erase(std::remove_if(list.begin(), list.end(), [&](const ParticleRecord& r) { return front != (r.side < 0.5f); }),
                               list.end());
                }
                draws = drawsOf(std::move(list), look);
                keepDraws(ctx, key, draws);
            }
            drawParticles(ctx, target, *draws, look, p, ctx.layerToTarget);
            return;
        }

        // following a layer, the shapes centre on its ink box (Pachislop layerBounds), the
        // generator's transform an offset on top; the records stay in the shape's own space
        TitleAffine space = ctx.layerToTarget;
        if (ctx.follow && em != "full") {
            auto grid = readGrid(ctx, *ctx.follow, ctx.followKey, frameGridSpec(ctx, INK_GRID_CELL));
            if (!grid) return;
            double box[4];
            if (gridInkBox(*grid, 0.0f, box)) {
                TitleAffine centre;
                centre.tx = 0.5 * (box[0] + box[2]);
                centre.ty = 0.5 * (box[1] + box[3]);
                space = ctx.layerToTarget.after(centre);
            }
        }
        ParticleSpace ps;
        ps.canvasWidth = ctx.canvasWidth;
        ps.canvasHeight = ctx.canvasHeight;
        // the Full Frame emitter spans the canvas: document space -> the layer's space
        ps.documentToSpace = ctx.layerToTarget.inverse().after(ctx.documentToTarget);
        const uint64_t key = layoutKey(ctx, p, 0, em == "full" ? ps.documentToSpace : TitleAffine{});
        std::shared_ptr<const ParticleDraws> draws = cachedDraws(ctx, key);
        if (!draws) {
            draws = drawsOf(layoutParticles(p, look, ps, nullptr), look);
            keepDraws(ctx, key, draws);
        }
        drawParticles(ctx, target, *draws, look, p, space);
    }

    // how far (canvas px, conservative) one particle can draw from its centre: the nearest depth
    // size, glint and strip proportions, defocus and streak (Pachislop particleReach)
    double particleReach(const ParticleLook& look) {
        const double d = look.depth;
        const double aspect = look.shape == ParticleShape::Confetti ? 2.4 : look.shape == ParticleShape::Petal ? 1.25 : 1.0;
        const double shapeK = std::max(aspect, look.glintMix > 0.0 ? 3.5 : 1.0);
        const double r = (look.size / 2.0) * (1.0 + 1.7 * d) * shapeK;
        const double defocus = 1.0 + 2.4 * d * (look.shape == ParticleShape::Bokeh ? 0.3 : 1.0);
        return r * defocus + (particleShapeStreaked(look.shape) ? look.streak : 0.0);
    }

    // how far past the followed layer particles draw (Pachislop followReach, plus the fall of
    // the new gravity); the Full Frame emitter draws anywhere on the canvas
    float particlesReach(const ParamValues& p) {
        const std::string em = p.string("emitter");
        if (em == "full") return -1.0f;
        const ParticleLook look = readParticleLook(p, 1.0);
        const double travel = std::max(0.0, p.number("travel"));
        const double fall = std::abs(p.number("gravity"));
        if (em == "layer") return (float)(travel + particleReach(look) + fall);
        // a shape centred on the followed layer reaches at most its own extent past that layer's ink
        const bool scatter = p.string("distribution") == "scatter";
        const double pad = (scatter && em != "point" ? 0.0 : travel) + particleReach(look);
        const TitleVec2 radius = p.vec2("radius");
        const double ring = em == "ring" ? 1.25 : 1.0;
        const double rx = em == "point" ? 0.0 : std::max(0.0, radius.x) * ring;
        const double ry = em == "point" || em == "line" ? 0.0 : std::max(0.0, radius.y) * ring;
        return (float)(std::max(rx, ry) + pad + fall);
    }

    // the mean quad of the look, in drawing-space px^2 (the static cost model's coverage)
    double meanQuadArea(const ParticleLook& look) {
        const double d = look.depth;
        const double r = look.size / 2.0 * (1.0 - 0.5 * look.sizeRandom) * (1.0 + 0.26 * d);
        const double blur = 0.5 * d * (look.shape == ParticleShape::Bokeh ? 0.3 : 1.0);
        double aspect = 1.0;
        if (look.shape == ParticleShape::Confetti) aspect = 2.4;
        else if (look.shape == ParticleShape::Petal) aspect = 1.25;
        else if (particleShapeStreaked(look.shape)) aspect = 1.0 + 0.7 * look.streak / std::max(0.1, 2.0 * r);
        // mixed-in glints are 3.5 times the radius
        return 4.0 * r * r * (aspect + blur) * (1.0 + blur) * (1.0 + look.glintMix * (3.5 * 3.5 - 1.0));
    }

    FxCost particlesCost(const ParamValues& p, const FxSize& size, FxTier tier) {
        const ParticleLook look = readParticleLook(p, 1.0);
        // the mobile tier draws the first 2,000 (a subset by R25)
        const int count = tier == FxTier::Mobile ? std::min(look.count, PARTICLES_MOBILE_MAX) : look.count;
        const double pixels = std::max(1.0, (double)size.width * size.height);
        const double fre = std::max(0.01, count * meanQuadArea(look) * size.pxScale * size.pxScale / pixels);
        const bool layer = p.string("emitter") == "layer";
        // the layout runs at ~0.4 us a particle; the layer emitter adds its emit map
        FxCost c = passCost(look.glintMix > 0.0 ? 2 : 1, fre, 1, count, count * 0.0004 + (layer ? 0.5 : 0.0));
        c.readback = layer;
        c.analysis = layer;
        return c;
    }

    GeneratorDef particles() {
        static const std::vector<const char*> SIZED = {"ellipse", "rect", "ring", "line"};
        static const std::vector<const char*> SHAPED = {"ellipse", "rect", "ring", "line", "point", "full"};
        GeneratorDef d;
        d.type = "particles";
        d.label = "Particles";
        d.category = "Particles";
        d.description = "Particle field: 金粉 gold dust, 火の粉 embers, light motes, confetti, sakura, star showers, bokeh, rain and fireworks. "
                        "Emits from a point, ellipse, rectangle, ring, line or the whole frame (centred on the followed layer when following "
                        "one), scattered, bursting out or streaming; following a layer it can also burst from that layer's edge or fill, "
                        "coloured from it, in front of it or behind it. Depth makes near particles bigger, brighter and defocused. Time, "
                        "gravity, spin and twinkle animate it live";
        d.params = {
            relayout(choice("emitter", "Emitter", "ellipse",
                            {{"ellipse", "Ellipse"},
                             {"rect", "Rectangle"},
                             {"ring", "Ring"},
                             {"line", "Straight Line"},
                             {"point", "Point"},
                             {"full", "Full Frame"},
                             {"layer", "Followed Layer"}},
                            "Where particles are born (Followed Layer: the followed layer's own pixels)")),
            when(relayout(choice("source", "Emit From", "fill", {{"fill", "Fill"}, {"edge", "Edge"}}, "The followed layer's whole fill or only its edge band")),
                 "emitter", {"layer"}),
            when(relayout(px("edge_width", "Edge Width", 4, 1, 100, "How deep the edge band particles are born in reaches")), "source", {"edge"}),
            when(choice("direction", "Direction", "normal", {{"normal", "Normal Out"}, {"point", "From Point"}, {"angle", "Angle"}},
                        "Which way particles burst from the followed layer"),
                 "emitter", {"layer"}),
            when(point("point", "Point", 0, 0, "The point particles burst away from, canvas px (+y up)"), "direction", {"point"}),
            when(degrees("heading", "Heading", -90, "The direction particles burst in, clockwise (-90 = up)", -180, 180), "direction", {"angle"}),
            when(relayout(point("radius", "Radius", 450, 300, "The emitter's half width and half height, px")), "emitter", SIZED),
            when(relayout(choice("distribution", "Distribution", "burst", {{"scatter", "Scatter"}, {"burst", "Burst Out"}, {"stream", "Stream"}},
                                 "Scattered over the emitter, bursting out of it or streaming in one direction")),
                 "emitter", SHAPED),
            when(relayout(percent("falloff", "Density Falloff", 60, "How strongly particles crowd the emitter's centre and the start of their travel, percent")),
                 "emitter", SHAPED),
            relayout(px("travel", "Distance", 200, 0, 4000, "How far particles travel from where they are born")),
            when(percent("distance_random", "Distance Random", 100, "How much each particle's travel varies, percent"), "emitter", {"layer"}),
            when(degrees("angle", "Angle", 90, "The stream's and streaks' direction, clockwise (90 = down)", -180, 180), "distribution",
                 {"scatter", "stream"}),
            degrees("cone", "Spread Angle", 20, "How far each particle's direction varies", 0, 360),
            when(choice("split", "Particles", "all",
                        {{"all", "All"}, {"front", "Front Half (above the layer)"}, {"back", "Back Half (below the layer)"}},
                        "Only the half drawn in front of or behind the followed layer, for a pair of layers"),
                 "emitter", {"layer"}),

            choice("shape", "Shape", "dot",
                   {{"dot", "Soft Dot"},
                    {"glint4", "4-Point Glint"},
                    {"glint6", "6-Point Glint"},
                    {"ember", "Ember (Streak)"},
                    {"square", "Square"},
                    {"confetti", "Confetti Strip"},
                    {"petal", "Sakura Petal"},
                    {"star", "Star"},
                    {"heart", "Heart"},
                    {"bokeh", "Bokeh Disc"},
                    {"rain", "Rain Streak"}},
                   "Each particle's shape"),
            when(percent("glint_mix", "Glint Mix", 0, "The share of dots or squares drawn as big 4-point glints, percent"), "shape", {"dot", "square"}),
            relayout(integer("count", "Count", 400, 1, PARTICLES_MAX, "How many particles (the mobile tier draws the first 2,000)")),
            px("size", "Size", 6, 0.5, 200, "A particle's diameter", 0.5),
            percent("size_random", "Size Random", 60, "How much smaller particles may be, percent"),
            percent("depth", "Depth", 50, "Fakes depth: near particles bigger, brighter and defocused, percent"),
            when(px("streak", "Streak Length", 40, 0, 400, "The length of ember and rain streaks"), "shape", {"ember", "rain"}),
            when(percent("ring", "Bokeh Ring", 30, "A bright rim on the bokeh discs, percent"), "shape", {"bokeh"}),
            when(degrees("rotation_random", "Rotation Random", 20, "How far each particle turns either way", 0, 180), "shape",
                 {"glint4", "glint6", "square", "confetti", "petal", "star", "heart"}),
            when(percent("tumble", "Tumble", 0, "Particles turning in 3D: foreshortened, the back face darker, percent"), "shape",
                 {"square", "confetti", "petal", "star", "heart"}),
            when(percent("sheen", "Foil Sheen", 50, "The glare sliding along the confetti foil, percent"), "shape", {"confetti"}),
            choice("color_mode", "Color", "radial",
                   {{"solid", "Solid"}, {"radial", "Gradient by Radius"}, {"palette", "Palette Random"}, {"sample", "Sample Layer"}},
                   "How each particle gets its colour (Sample Layer: from the followed layer's face)"),
            when(light(color("color", "Solid Color", "#ffd166", "Every particle's colour")), "color_mode", {"solid"}),
            when(light(gradient("gradient", "Gradient", gradientValue({{0.0, "#fff2c2"}, {0.35, "#ffc845"}, {1.0, "#c77a12"}}),
                                "The colours by distance from the emitter, or the palette's stops")),
                 "color_mode", {"radial", "palette"}),
            percent("opacity", "Opacity", 100, "The particles' opacity, percent"),
            percent("opacity_random", "Opacity Random", 40, "How much fainter particles may be, percent"),
            relayout(seedParam("Reshuffles every particle")),

            live(number("time", "Time", 1, 0, 1, ParamUnit::None,
                        "Where the particles are along their travel: 0 at birth, 1 where they come to rest (the still)")),
            flag("loop", "Loop", false, "Each particle runs on its own offset, so animating Time gives a continuous stream"),
            live(px("gravity", "Gravity", 0, -2000, 2000, "How far particles fall by Time 1 (negative rises)")),
            live(degrees("spin", "Spin", 0, "How far particles turn by Time 1, clockwise, each at its own rate", -1080, 1080)),
            when(live(number("twinkle", "Twinkle", 0, 0, 1, ParamUnit::None, "How strongly glints flash, each on its own beat")), "shape",
                 {"glint4", "glint6", "dot", "square"}),
            number("fade_in", "Fade In", 0, 0, 1, ParamUnit::None, "The share of each particle's time its opacity ramps up over"),
            number("fade_out", "Fade Out", 0, 0, 1, ParamUnit::None, "The share of each particle's time its opacity ramps down over"),
        };
        d.gizmos = {{GizmoKind::Point, "point", nullptr}, {GizmoKind::Direction, "angle", "travel"}, {GizmoKind::Direction, "heading", nullptr}};
        d.follow = FollowMode::Optional;
        d.followReach = particlesReach;
        d.addsLight = [](const ParamValues& p) { return !particleShapeSolid(particleShapeOf(p.string("shape"))); };
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F2;
        d.live.kind = LiveKind::Kernel;
        d.live.params = {"time", "gravity", "spin", "twinkle"};
        d.live.program = (uint32_t)GenProgram::QuadsParticles | (uint32_t)TITLE_VARIANT_GENERATOR;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"particles", "particle field", "パーティクル", "金粉", "gold dust", "glitter", "火の粉", "embers", "光の粒", "light motes",
                     "sparkles", "紙吹雪", "confetti", "桜吹雪", "sakura", "bokeh", "ボケ", "rain", "雨", "花火", "fireworks"};
        // the layer emitter reads the followed layer before its effects
        d.inputs.content = true;
        d.estimate = particlesCost;
        d.presetFile = "editor/resources/titleart/fx-presets/particles.yaml";
        d.render = renderParticles;
        return d;
    }

    // ---- Ink Splatter ----

    void renderInkSplatter(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        if (!ctx.followContent) {
            ctx.runner.clear(target);
            return;
        }
        // the content's 8 px grid: its silhouette and the chamfer distance from it
        const std::shared_ptr<const Grid> grid = readGrid(ctx, *ctx.followContent, ctx.followContentKey, frameGridSpec(ctx, INK_GRID_CELL));
        if (!grid) {
            ctx.runner.clear(target);
            return;
        }
        const std::vector<InkBlot> blots = layoutInkSplatter(*grid, p, seedOf(ctx, p));
        const TitleColor ink = fx::linearOf(p.color("color"));
        const float growth = (float)std::clamp(p.number("growth", 1.0), 0.0, 1.0);
        drawFollowed(ctx, target, [&](TargetRef& out) {
            ctx.runner.clear(out);
            if (blots.empty()) return;
            const TitleAffine space = ctx.documentToTarget;
            // target px per canvas px: Pachislop built its blots in render px (comp px x pxScale)
            const double k = std::max(1e-6, spaceScale(space));
            QuadBatch batch;
            for (const InkBlot& b : blots) {
                const double abx = (b.bx - b.ax) * k, aby = (b.by - b.ay) * k;
                const double ra = b.ra * k, rb = b.rb * k;
                // Pachislop's bounding quad (the cone's axis plus room for the ragged rim and the
                // antialiasing pixel), drawn as its axis-aligned box: the shader cuts it back
                const double len = std::hypot(abx, aby);
                const double ux = len > 1e-3 ? abx / len : 1.0, uy = len > 1e-3 ? aby / len : 0.0;
                const double R = std::max(ra, rb) * 1.3 + 1.5;
                const double hx = std::abs(ux) * (0.5 * len + R) + std::abs(uy) * R;
                const double hy = std::abs(uy) * (0.5 * len + R) + std::abs(ux) * R;
                QuadInstance q;
                q.cx = (float)(0.5 * (b.ax + b.bx));
                q.cy = (float)(0.5 * (b.ay + b.by));
                q.w = (float)(2.0 * hx / k);
                q.h = (float)(2.0 * hy / k);
                q.color[0] = q.color[1] = q.color[2] = q.color[3] = 1.0f;
                q.extra[0] = (float)(0.5 * abx);
                q.extra[1] = (float)(0.5 * aby);
                q.extra[2] = (float)ra;
                q.extra[3] = (float)rb;
                q.more[0] = (float)b.seed;
                batch.add(q);
            }
            // no min_px growth: every blot's box is at least 3 target px, and the shader measures it
            batch.draw(ctx, GenProgram::QuadsInk, "ink splatter", out, space, QuadBlend::Normal, 0.0f, [&](TitlePass& pass) {
                pass.set(6, ink.r, ink.g, ink.b, 0.0f);
                pass.set(7, growth);
            });
        });
    }

    FxCost inkSplatterCost(const ParamValues& p, const FxSize& size, FxTier) {
        const double drops = std::round(std::clamp(p.number("amount"), 0.0, 100.0) / 100.0 * 80.0);
        // satellites average 3.5 x the amount x 0.7 for the nearness each drop
        const int instances = (int)std::lround(drops * (1.0 + p.number("satellites") / 100.0 * 3.5 * 0.7));
        const double k = size.pxScale;
        const double rad = 0.34 * std::max(1.0, p.number("size")) * k;
        const double side = 2.0 * (1.3 * rad + 1.5) + p.number("streaks") / 100.0 * 0.5 * p.number("streak_length") * k;
        const double pixels = std::max(1.0, (double)size.width * size.height);
        const double fre = std::max(0.01, instances * side * 2.0 * (1.3 * rad + 1.5) / pixels);
        // the chamfer distance over the frame grid and the rejection sampling
        FxCost c = passCost(1, fre, 4, instances, 1.0);
        c.readback = true;
        return c;
    }

    GeneratorDef inkSplatter() {
        GeneratorDef d;
        d.type = "ink_splatter";
        d.label = "Ink Splatter";
        d.category = "Particles";
        d.description = "墨飛沫: seeded ink drops thrown off the silhouette of the layer it follows, with satellite droplets and drag streaks; "
                        "below that layer, or above it to spatter over it. Black by default; set the colour to red for 赤 titles. Growth "
                        "animates the drops live";
        d.params = {
            percent("amount", "Amount", 45, "How many main drops (80 at 100 %), percent"),
            px("spread", "Spread", 150, 0, 1500, "How far from the silhouette drops land"),
            px("size", "Drop Size", 22, 1, 300, "The largest drop's radius"),
            percent("satellites", "Satellite Droplets", 60, "Droplets thrown around each drop, percent", 0, 200),
            percent("streaks", "Streaks", 35, "The share of drops dragged into streaks, percent"),
            px("streak_length", "Streak Length", 110, 0, 1500, "How far a drag streak runs"),
            choice("direction", "Streak Direction", "radial", {{"radial", "Away from the Centre"}, {"directional", "One Direction"}},
                   "Streaks fly away from the silhouette's centre or all one way"),
            when(degrees("angle", "Streak Angle", 10, "The streaks' direction, clockwise", -180, 180), "direction", {"directional"}),
            color("color", "Ink Color", "#0b0907", "The ink's colour"),
            live(number("growth", "Growth", 1, 0, 1, ParamUnit::None, "Every drop and streak grows from nothing (0) to its full size (1)")),
            relayout(seedParam("Reshuffles every drop")),
        };
        d.gizmos = {{GizmoKind::Direction, "angle", "streak_length"}};
        d.follow = FollowMode::Required;
        // the furthest ink: a drop at the spread limit, its streak head and the satellites thrown past it
        d.followReach = [](const ParamValues& p) {
            return (float)(std::max(0.0, p.number("spread")) + std::max(0.0, p.number("streak_length")) + 3.5 * p.number("size"));
        };
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F0;
        d.live.kind = LiveKind::Kernel;
        d.live.params = {"growth"};
        d.live.program = (uint32_t)GenProgram::QuadsInk | (uint32_t)TITLE_VARIANT_GENERATOR;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"inkSplatter", "ink splatter", "墨飛沫", "墨", "ink splash", "spatter", "splatter", "paint splatter", "赤"};
        d.inputs.content = true;
        d.estimate = inkSplatterCost;
        d.presetFile = "editor/resources/titleart/fx-presets/ink_splatter.yaml";
        d.render = renderInkSplatter;
        return d;
    }

}

void titleart::registerParticleGenerators(TitleEffectRegistry& registry) {
    registry.add(particles());
    registry.add(inkSplatter());
}
