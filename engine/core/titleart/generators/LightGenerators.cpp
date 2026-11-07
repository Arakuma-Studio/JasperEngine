// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Light generators (docs/specs/S83-title-effects-generators.md §6.7.8-6.7.10): Glints, Lens
// Flare and Backlight, ported from Pachislop src/generators/glints.ts and src/generators/light/
// {lensFlare,backlight}.ts by SafEight, used with the author's permission.

#include "titleart/analysis/HighlightFinder.h"
#include "titleart/analysis/ReadGrid.h"
#include "titleart/generators/TitleGen.h"
#include "titleart/generators/TitleGenFamilies.h"
#include "titleart/gpu/QuadBatch.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fxparams;
using namespace jasperengine::titleart::gen;

namespace {

    constexpr double DEG = 3.14159265358979323846 / 180.0;
    // a grid cell holding any ink (one 8-bit step of mean alpha): the followed layer's box for a
    // centre (Pachislop's layerBounds / bounds take every pixel with alpha > 0.004)
    constexpr float INK_ALPHA = 0.5f / 255.0f;
    // the analysis cell of Glints' scatter shape, canvas px (Pachislop readGrid(content, 8))
    constexpr double SCATTER_CELL = 8.0;

    // the followed layer's ink box centre in document space from its grid; false when it draws
    // nothing or its grid cannot be read yet (the renderer runs the generator again)
    bool inkCentre(const GeneratorContext& ctx, const TargetRef& src, uint64_t key, TitleVec2& centre) {
        const analysis::GridSpec spec = analysis::frameGridSpec(ctx, analysis::HL_CELL);
        const std::shared_ptr<const analysis::Grid> grid = analysis::readGrid(ctx, src, key, spec);
        analysis::InkBox box;
        if (!grid || !analysis::gridBox(*grid, box, INK_ALPHA)) return false;
        centre = {0.5 * (box.x0 + box.x1), 0.5 * (box.y0 + box.y1)};
        return true;
    }

    // ---- Glints ----

    // the smallest ray half-width at a star's core, canvas px (Pachislop GLINT_MIN_THIN)
    constexpr double GLINT_MIN_THIN = 0.6;

    // a star sprite in document space
    struct Star {
        double x = 0.0, y = 0.0;    // canvas px about the canvas centre, +y up
        double size = 0.0;          // radius, canvas px
        double rot = 0.0;           // radians counter-clockwise (Pachislop's)
        double intensity = 1.0;
        double phase = 0.0;         // the twinkle's, 0..1
    };

    // the hero's anamorphic cross: its centre in document space, sizes in target px
    struct Hero {
        double x = 0.0, y = 0.0;
        double t = 10.0, len = 100.0, cross = 20.0, rot = 0.0;
    };

    // the sprite look shared by every star of a layer (Pachislop StarLook)
    struct StarLook {
        int rays = 4;
        double thin = 0.035;        // ray half-width at the core, star radii
        bool spectral = false;
    };

    StarLook starLook(const ParamValues& p) {
        const std::string rays = p.string("rays");
        const std::string placement = p.string("placement");
        StarLook look;
        look.rays = rays == "snow" ? 6 : rays == "eight" ? 8 : 4;
        // big flare stars (peaks, point) are finer than sparkles
        look.thin = (placement == "peaks" || placement == "point" ? 0.026 : 0.035) * (p.number("thickness") / 100.0);
        look.spectral = p.flag("spectral");
        return look;
    }

    // Pachislop's rotation turns counter-clockwise (y-up); ours is clockwise on screen (D16)
    double glintRotation(const ParamValues& p) {
        return -p.number("rotation") * DEG;
    }

    // The stars as light x gain into `out` (Pachislop renderStars / renderHero): the sprites add
    // into a cleared scratch, then one pass writes that light (and the hero's cross) as
    // premultiplied light with alpha = the brightest channel
    void composeStars(GeneratorContext& ctx, TargetRef& out, const std::vector<Star>& stars, const ParamValues& p, const Hero* hero) {
        if (stars.empty() && !hero) {
            ctx.runner.clear(out);
            return;
        }
        fx::Scratch tmp = fx::scratchOf(ctx.runner, TitleBuffer::Accumulator, ctx.width, ctx.height);
        if (!tmp || !ctx.runner.clear(*tmp)) return;
        const TitleAffine space = ctx.documentToTarget;
        const TitleColor color = fx::linearOf(p.color("color"));
        if (!stars.empty()) {
            const StarLook look = starLook(p);
            QuadBatch batch;
            for (const Star& s : stars) {
                QuadInstance q;
                q.cx = (float)s.x;
                q.cy = (float)s.y;
                q.w = q.h = (float)(2.0 * s.size);
                q.rotation = (float)s.rot;
                // the star is linear in its colour, so the intensity scales it
                q.color[0] = (float)(color.r * s.intensity);
                q.color[1] = (float)(color.g * s.intensity);
                q.color[2] = (float)(color.b * s.intensity);
                q.color[3] = 1.0f;
                q.extra[0] = (float)(GLINT_MIN_THIN / std::max(s.size, 1e-6));
                q.extra[1] = (float)s.phase;
                batch.add(q);
            }
            const double twinkle = std::clamp(p.number("twinkle"), 0.0, 1.0);
            const double phase = p.number("twinkle_phase");
            const double turn = p.number("twinkle_rotate") * DEG;
            batch.draw(ctx, GenProgram::QuadsGlints, "glints stars", *tmp, space, QuadBlend::Add, 1.5f, [&](TitlePass& pass) {
                pass.set(6, (float)look.rays, (float)look.thin, look.spectral ? 1.0f : 0.0f, 0.0f);
                pass.set(7, (float)twinkle, (float)(phase - std::floor(phase)), (float)turn, 0.0f);
            });
        }
        TitlePass pass = genPass(ctx, GenProgram::GlintsCompose, hero ? "glints hero" : "glints light", out, space);
        pass.textures[0].target = tmp.get();
        pass.set(6, (float)(p.number("intensity") / 100.0), hero ? 1.0f : 0.0f, hero ? (float)hero->x : 0.0f, hero ? (float)hero->y : 0.0f);
        if (hero) pass.set(7, (float)hero->t, (float)hero->len, (float)hero->cross, (float)hero->rot);
        pass.set(8, color.r, color.g, color.b, 0.0f);
        ctx.runner.run(pass);
    }

    // the "highlights" placement's options from the params
    analysis::HighlightOptions highlightOptions(const GeneratorContext& ctx, const ParamValues& p) {
        analysis::HighlightOptions o;
        o.large = p.integer("large");
        o.small = p.integer("small");
        o.size = p.number("size");
        o.sizeVariation = p.number("size_variation") / 100.0;
        o.threshold = p.number("threshold") / 100.0;
        o.spacing = p.number("spacing");
        o.faceBias = p.number("face_bias") / 100.0;
        o.seed = seedOf(ctx, p);
        return o;
    }

    // the peaks of the followed layer as styled (its grid read once per content key, the peaks
    // kept by it); null while the grid cannot be read yet
    std::shared_ptr<const analysis::Highlights> styledHighlights(const GeneratorContext& ctx, const analysis::FinderOptions& options) {
        const analysis::GridSpec spec = analysis::frameGridSpec(ctx, analysis::HL_CELL);
        const std::shared_ptr<const analysis::Grid> grid = analysis::readGrid(ctx, *ctx.follow, ctx.followKey, spec);
        if (!grid) return nullptr;
        return analysis::findHighlightsCached(*grid, analysis::gridKeyOf(ctx.followKey, spec), options);
    }

    // one star at the position (placement Fixed Point)
    Star pointStar(const ParamValues& p) {
        const TitleVec2 at = p.vec2("position");
        Star s;
        s.x = at.x;
        s.y = at.y;
        s.size = p.number("size");
        s.rot = glintRotation(p);
        return s;
    }

    // big flare stars on the hottest peaks (placement Hottest Peaks)
    std::vector<Star> peakStars(const GeneratorContext& ctx, const ParamValues& p) {
        std::vector<Star> stars;
        analysis::FinderOptions options;
        options.threshold = p.number("threshold") / 100.0;
        const std::shared_ptr<const analysis::Highlights> found = styledHighlights(ctx, options);
        if (!found) return stars;
        const std::vector<analysis::Highlight>& hl = found->peaks;
        const double size = p.number("size");
        const double vr = p.number("size_variation") / 100.0;
        const double rot = glintRotation(p);
        const double seed = seedOf(ctx, p);
        const double top = hl.empty() ? 1.0 : hl[0].score;
        const auto radius = [&](const analysis::Highlight& h) { return size * analysis::highlightSize(h.score, top, vr); };
        for (const analysis::Pick& pk : analysis::pickHighlights(hl, p.integer("stars"), radius, 0.45)) {
            Star s;
            s.x = pk.h.x;
            s.y = pk.h.y;
            s.size = pk.r * (0.6 + 0.4 * pk.fade);
            s.rot = rot;
            s.intensity = (0.4 + 0.6 * (pk.r / size)) * pk.fade;
            s.phase = analysis::glintWobble(pk.h.x, pk.h.y, seed, 3);
            stars.push_back(s);
        }
        return stars;
    }

    // highlight glints or the hero glint on the peaks of the followed layer as styled
    void renderPeaks(GeneratorContext& ctx, TargetRef& out, const ParamValues& p) {
        const analysis::HighlightOptions o = highlightOptions(ctx, p);
        const std::shared_ptr<const analysis::Highlights> found = styledHighlights(ctx, analysis::finderOptions(o));
        if (!found || !found->hasBox || found->peaks.empty()) {
            ctx.runner.clear(out);
            return;
        }
        const std::vector<analysis::Highlight>& hl = found->peaks;
        const double size = o.size, seed = o.seed;
        const double bv = p.number("brightness_variation") / 100.0;
        const double jit = p.number("jitter") / 100.0;
        const double rot0 = glintRotation(p);
        // a little size (+-20 %) and rotation (+-12 deg) jitter at full Jitter, a smooth function of
        // the peak's position
        const auto star = [&](const analysis::Pick& pk, double intensity) {
            Star s;
            s.x = pk.h.x;
            s.y = pk.h.y;
            s.size = pk.r * (0.6 + 0.4 * pk.fade) * (1.0 + 0.4 * jit * (analysis::glintWobble(pk.h.x, pk.h.y, seed, 0) - 0.5));
            s.rot = rot0 + 0.42 * jit * (analysis::glintWobble(pk.h.x, pk.h.y, seed, 1) - 0.5);
            s.intensity = intensity * pk.fade;
            s.phase = analysis::glintWobble(pk.h.x, pk.h.y, seed, 3);
            return s;
        };
        std::vector<Star> stars;
        if (p.string("placement") == "highlights") {
            const analysis::GlintPicks picks = analysis::pickGlints(*found, o);
            // big glints read brightest; Brightness Variation dims the smaller ones
            for (const analysis::Pick& pk : picks.large) stars.push_back(star(pk, 1.0 - bv * (1.0 - pk.r / size)));
            // small twinkles vary in size with Size Variation (dimmer as they shrink), so they never
            // read as a uniform scatter
            for (const analysis::Pick& pk : picks.small) {
                const double v = 1.0 + 1.2 * o.sizeVariation * (analysis::glintWobble(pk.h.x, pk.h.y, seed, 2) - 0.5);
                Star s = star(pk, (1.0 - bv * 0.6) * (0.75 + 0.25 * v));
                s.size *= v;
                stars.push_back(s);
            }
            composeStars(ctx, out, stars, p, nullptr);
            return;
        }
        // Hero: the streak may pass the ink box by at most 10 % of its width per side, so the core
        // sits on the strongest peak inside the central band where the streak fits (blended with
        // near-tied ones beside it)
        const analysis::InkBox& box = found->box;
        const double W = box.x1 - box.x0;
        const double half = (W * p.number("hero_width")) / 200.0;
        const double band = W * std::max(0.1, 0.6 - half / W);
        const double cx = 0.5 * (box.x0 + box.x1);
        std::vector<int> candidates;
        for (size_t i = 0; i < hl.size(); i++) {
            if (std::abs(hl[i].x - cx) <= band) candidates.push_back((int)i);
        }
        if (candidates.empty()) {
            for (size_t i = 0; i < hl.size(); i++) candidates.push_back((int)i);
        }
        std::vector<analysis::Pick> heroPick = analysis::pickHighlights(hl, candidates, 1, [&](const analysis::Highlight&) { return size; }, 0.6, o.spacing);
        if (heroPick.empty()) {
            ctx.runner.clear(out);
            return;
        }
        heroPick[0].r = size;
        heroPick[0].fade = 1.0;
        const auto starRadius = [&](const analysis::Highlight&) { return size * analysis::HERO_STAR; };
        for (const analysis::Pick& pk : analysis::pickHighlights(hl, p.integer("hero_stars"), starRadius, 0.6, o.spacing, heroPick)) {
            stars.push_back(star(pk, 1.0 - bv * 0.4));
        }
        // target px per canvas px (Pachislop's pxScale), as titleart/gen/glints.glsl measures the cross
        const double k = spaceScale(ctx.documentToTarget);
        Hero hero;
        hero.x = heroPick[0].h.x;
        hero.y = heroPick[0].h.y;
        hero.t = std::max(0.5, size * (p.number("hero_thickness") / 100.0) * k);
        hero.len = half * k;
        hero.cross = half * (p.number("hero_cross") / 100.0) * k;
        hero.rot = -p.number("hero_angle") * DEG;
        composeStars(ctx, out, stars, p, &hero);
    }

    // stars scattered over and around the silhouette of the followed layer's own pixels
    std::vector<Star> scatterStars(const GeneratorContext& ctx, const ParamValues& p) {
        std::vector<Star> stars;
        if (!ctx.followContent) return stars;
        const analysis::GridSpec spec = analysis::frameGridSpec(ctx, SCATTER_CELL);
        const std::shared_ptr<const analysis::Grid> g = analysis::readGrid(ctx, *ctx.followContent, ctx.followContentKey, spec);
        analysis::InkBox box;
        if (!g || !analysis::gridBox(*g, box)) return stars;
        const double seed = seedOf(ctx, p);
        const double size = p.number("size");
        const double sv = p.number("size_variation") / 100.0;
        const double bv = p.number("brightness_variation") / 100.0;
        const double rot0 = glintRotation(p);
        const int count = p.integer("count");
        const std::vector<float> dist = analysis::gridDistance(g->w, g->h, g->alpha);
        const double cellPx = g->cellComp;
        const double spread = p.number("spread");
        const bool inside = p.flag("inside");
        const double bx0 = box.x0 - spread, by0 = box.y0 - spread;
        const double bw = box.x1 - box.x0 + 2.0 * spread, bh = box.y1 - box.y0 + 2.0 * spread;
        // glints.ts imports effects/b/common.ts rng (the 7919 seeding)
        Rng r(seed * 131.0 + 7.0, Rng::Seeding::EffectsB);
        for (int tries = 0; (int)stars.size() < count && tries < count * 80; tries++) {
            const double cx = bx0 + r() * bw;
            const double cy = by0 + r() * bh;
            const int gx = (int)std::floor((cx - g->x0) / cellPx);
            const int gy = (int)std::floor((cy - g->y0) / cellPx);
            const double d = gx >= 0 && gy >= 0 && gx < g->w && gy < g->h ? dist[(size_t)gy * g->w + gx] * cellPx : std::numeric_limits<double>::infinity();
            if (d > spread || (!inside && d == 0.0)) continue;
            // keyed by the try, not the placed index: a borderline cell flipping between render
            // scales then changes only that one star instead of re-rolling all that follow
            const double t = (double)tries;
            Star s;
            s.x = cx;
            s.y = cy;
            s.size = size * (1.0 - sv + sv * hashN({seed, t, 3.0}));
            s.rot = rot0 + (hashN({seed, t, 1.0}) - 0.5) * 0.4;
            s.intensity = 1.0 - bv * hashN({seed, t, 2.0});
            s.phase = hashN({seed, t, 4.0});
            stars.push_back(s);
        }
        return stars;
    }

    void renderGlints(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        if (!ctx.follow) {
            ctx.runner.clear(target);
            return;
        }
        const std::string placement = p.string("placement");
        drawFollowed(ctx, target, [&](TargetRef& out) {
            if (placement == "point") composeStars(ctx, out, {pointStar(p)}, p, nullptr);
            else if (placement == "scatter") composeStars(ctx, out, scatterStars(ctx, p), p, nullptr);
            else if (placement == "peaks") composeStars(ctx, out, peakStars(ctx, p), p, nullptr);
            else renderPeaks(ctx, out, p);
        });
    }

    float glintsReach(const ParamValues& p) {
        // jitter grows a glint by up to 20 %; the hero's streak spans a share of the ink box past
        // it, only its core and stars count here (Pachislop followReach)
        const std::string placement = p.string("placement");
        if (placement == "scatter") return (float)(p.number("spread") + p.number("size"));
        if (placement == "peaks" || placement == "point") return (float)p.number("size");
        return (float)(p.number("size") * (1.0 + 0.2 * p.number("jitter") / 100.0));
    }

    FxCost glintsCost(const ParamValues& p, const FxSize& size, FxTier) {
        const std::string placement = p.string("placement");
        int stars = 1;
        if (placement == "highlights") stars = p.integer("large") + p.integer("small");
        else if (placement == "peaks") stars = p.integer("stars");
        else if (placement == "hero") stars = p.integer("hero_stars");
        else if (placement == "scatter") stars = p.integer("count");
        // the sprites' quads (2 x size a side) and the light pass over the target
        const double side = 2.0 * p.number("size") * size.pxScale;
        const double target = std::max(1.0, (double)size.width * size.height);
        const bool analysed = placement != "point" && placement != "scatter";
        FxCost c = passCost(2, 1.0 + stars * side * side / target, 1, stars, analysed ? 3.0 : 0.2);
        c.analysis = c.readback = placement != "point";
        return c;
    }

    GeneratorDef glints() {
        GeneratorDef d;
        d.type = "glints";
        d.label = "Glints";
        d.category = "Light";
        d.description = "キラキラ: star sparkles on the layer it follows: on its highlight peaks (a few large glints plus small ones, favouring the "
                        "face over the outer rims), big flare stars on its hottest peaks, one long anamorphic hero glint across it, scattered "
                        "over and around its shape, or one star at a fixed point; 4-ray, 6-ray snow-cross or 8-ray shapes, optionally with "
                        "rainbow rays. Set the layer to Add";
        const std::vector<const char*> onHighlights = {"highlights"}, onHero = {"hero"}, onHighlightsHero = {"highlights", "hero"};
        d.params = {
            relayout(choice("placement", "Placement", "highlights",
                            {{"highlights", "On Highlights"}, {"peaks", "Hottest Peaks"}, {"hero", "Hero Glint"}, {"scatter", "Scatter Around Shape"},
                             {"point", "Fixed Point"}},
                            "Where the stars go: the followed layer's highlights, its hottest peaks, one hero glint, a scatter or one point")),
            when(relayout(integer("large", "Large Glints", 5, 0, 20, "How many large glints dress the brightest highlights")), "placement", onHighlights),
            when(relayout(integer("small", "Small Glints", 6, 0, 40, "How many small twinkles go on the other highlights")), "placement", onHighlights),
            when(relayout(integer("stars", "Star Count", 5, 1, 40, "How many flare stars go on the hottest peaks")), "placement", {"peaks"}),
            when(relayout(integer("count", "Count", 40, 1, 400, "How many stars scatter over and around the shape")), "placement", {"scatter"}),
            when(percent("hero_width", "Hero Streak Width", 80, "The hero streak's length, percent of the layer's width", 20, 150), "placement", onHero),
            when(percent("hero_thickness", "Hero Thickness", 100, "The hero streak's thickness, percent of the star size", 20, 400), "placement", onHero),
            when(percent("hero_cross", "Vertical Spike", 22, "The hero's vertical spike, percent of its streak"), "placement", onHero),
            when(degrees("hero_angle", "Streak Angle", 0, "Turns the hero streak, clockwise", -90, 90), "placement", onHero),
            when(relayout(integer("hero_stars", "Small Stars", 3, 0, 8, "How many small stars sparkle beside the hero glint")), "placement", onHero),
            when(point("position", "Position", 0, 0, "The star's centre, canvas px from the canvas centre (+y up)"), "placement", {"point"}),
            px("size", "Star Size", 50, 2, 800, "The radius of a full-size star"),
            when(percent("size_variation", "Size Variation", 60, "How much weaker highlights shrink their stars, percent"), "placement",
                 {"highlights", "peaks", "scatter"}),
            when(relayout(percent("threshold", "Highlight Threshold", 30, "Highlights dimmer than this share of the brightest get no star, percent")),
                 "placement", {"highlights", "peaks", "hero"}),
            when(relayout(px("spacing", "Min. Spacing", 80, 0, 600, "The least distance between stars")), "placement", onHighlightsHero),
            when(relayout(percent("face_bias", "Face Priority", 70, "Favours highlights deep in the face over those on the outer rims, percent")),
                 "placement", onHighlightsHero),
            when(percent("jitter", "Size / Angle Jitter", 50, "A little size and angle variation from star to star, percent"), "placement",
                 onHighlightsHero),
            when(relayout(px("spread", "Scatter Area", 60, 0, 600, "How far past the shape's edge stars scatter")), "placement", {"scatter"}),
            when(flag("inside", "Scatter Over Shape", true, "Stars also land on the shape itself, not only around it"), "placement", {"scatter"}),
            choice("rays", "Shape", "four", {{"four", "4-Ray Cross"}, {"snow", "6-Ray Snow Cross"}, {"eight", "8-Ray Star (4 Long + 4 Short)"}},
                   "The star's rays"),
            flag("spectral", "Rainbow Rays", false, "Fans each ray out into a rainbow fringe"),
            percent("thickness", "Ray Thickness", 100, "The rays' width at the core, percent", 20, 400),
            light(color("color", "Color", "#fff4c0", "The stars' colour: white at the core, this toward the ray tips")),
            multiplies(live(percent("intensity", "Intensity", 130, "The stars' brightness, percent", 0, 400))),
            live(degrees("rotation", "Rotation", 0, "Turns every star, clockwise", -180, 180)),
            when(percent("brightness_variation", "Brightness Variation", 50, "How much smaller stars are dimmed, percent"), "placement",
                 {"highlights", "hero", "scatter"}),
            when(relayout(seedParam("Reshuffles the jitter, the scatter and the twinkle phases")), "placement",
                 {"highlights", "peaks", "hero", "scatter"}),
            live(number("twinkle", "Twinkle", 0, 0, 1, ParamUnit::None, "How deeply each star dims and flashes over its twinkle cycle")),
            live(number("twinkle_phase", "Twinkle Phase", 0, 0, 1, ParamUnit::None, "Where every star is in its twinkle cycle (wraps; animate it 0 to 1)")),
            live(degrees("twinkle_rotate", "Twinkle Rotation", 0, "How far each star rocks back and forth over its twinkle cycle", 0, 45)),
        };
        d.gizmos = {{GizmoKind::Point, "position", nullptr}, {GizmoKind::Direction, "rotation", nullptr}};
        d.follow = FollowMode::Required;
        d.followReach = glintsReach;
        d.addsLight = [](const ParamValues&) { return true; };
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F2;
        d.live.kind = LiveKind::Kernel;
        d.live.params = {"intensity", "rotation", "twinkle", "twinkle_phase", "twinkle_rotate"};
        d.live.program = (uint32_t)GenProgram::QuadsGlints | (uint32_t)TITLE_VARIANT_GENERATOR;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"glints", "glint", "キラキラ", "きらきら", "sparkle", "sparkles", "twinkle", "star glints", "highlight stars", "hero glint", "光沢"};
        d.inputs.content = true;
        d.inputs.analysis = AnalysisSource::Styled;
        d.estimate = glintsCost;
        d.presetFile = "editor/resources/titleart/fx-presets/glints.yaml";
        d.render = renderGlints;
        return d;
    }

    // ---- Lens Flare ----

    // the mobile tier's quads (titleart/gen/lens_flare.glsl QuadsLensFlare), target px: the
    // core's half side in flare sizes (its bloom fades out by then), the streak's half length
    // in streak lengths (its hot line is below 1/500 there) and the ghosts' reach about their
    // axis points (their centres move +-0.1 of the axis, their radius is at most 1.15 sizes)
    constexpr double FLARE_CORE_REACH = 6.0, FLARE_STREAK_REACH = 3.0, FLARE_GHOST_SHIFT = 0.2, FLARE_GHOST_RADIUS = 1.15;

    // the flare as additive quads of the full-canvas pass's block (§6.7.9's mobile reduction):
    // the core with its bloom and halo ring, the streak, and the ghosts when `ghosts`
    void drawLensFlareQuads(GeneratorContext& ctx, TargetRef& target, const TitlePass& block, const TitleAffine& space, const TitleVec2& posT, bool ghosts) {
        ctx.runner.clear(target);
        const double k = spaceScale(space);
        if (!(k > 0.0)) return;
        const double size = block.params[6][2], streak = block.params[6][3], rot = block.params[7][1];
        const TitleAffine inv = space.inverse();
        QuadBatch batch;
        // a quad about the point `ox, oy` render px (+y up) from the flare, its long side along
        // the render-px direction `ux, uy`
        auto add = [&](double ox, double oy, double halfLength, double halfWidth, double ux, double uy, int part) {
            const TitleVec2 c = inv.apply(TitleVec2{posT.x + ox, posT.y - oy});
            QuadInstance q;
            q.cx = (float)c.x;
            q.cy = (float)c.y;
            q.w = (float)(2.0 * halfLength / k);
            q.h = (float)(2.0 * halfWidth / k);
            q.rotation = (float)std::atan2(inv.c * ux - inv.d * uy, inv.a * ux - inv.b * uy);
            std::fill(q.color, q.color + 4, 1.0f);
            q.extra[0] = (float)part;
            batch.add(q);
        };
        add(0.0, 0.0, FLARE_CORE_REACH * size, FLARE_CORE_REACH * size, 1.0, 0.0, 0);
        add(0.0, 0.0, FLARE_STREAK_REACH * std::max(1.0, streak) + size, size + 1.0, std::cos(rot), std::sin(rot), 1);
        if (ghosts) {
            const double ax = block.params[9][0], ay = block.params[9][1];
            const double len = std::hypot(ax, ay);
            const double ux = len > 0.0 ? ax / len : 1.0, uy = len > 0.0 ? ay / len : 0.0;
            for (int i = 0; i < 6; i++) {
                const double along = 2.0 * (0.35 + 0.33 * i);
                add(ax * along, ay * along, FLARE_GHOST_SHIFT * len + FLARE_GHOST_RADIUS * size + 1.0, FLARE_GHOST_RADIUS * size + 1.0, ux, uy, 2 + i);
            }
        }
        batch.draw(ctx, GenProgram::QuadsLensFlare, "lens_flare", target, space, QuadBlend::Add, 0.0f, [&](TitlePass& pass) {
            for (int i = 6; i <= 9; i++) pass.set(i, block.params[i][0], block.params[i][1], block.params[i][2], block.params[i][3]);
        });
    }

    void renderLensFlare(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        // Pachislop places the flare in the generator's own layer space, following or not
        const TitleAffine space = ctx.layerToTarget;
        TitleVec2 pos = p.vec2("position");
        if (ctx.follow) {
            // following: an offset from the centre of that layer's ink, so the flare moves with it;
            // a layer that draws nothing has no flare
            TitleVec2 centre;
            if (!inkCentre(ctx, *ctx.follow, ctx.followKey, centre)) {
                ctx.runner.clear(target);
                return;
            }
            pos.x += centre.x;
            pos.y += centre.y;
        }
        const double k = spaceScale(space);
        const TitleVec2 posT = space.apply(pos);
        const TitleVec2 centreT = ctx.documentToTarget.apply(TitleVec2{0.0, 0.0});
        const TitleColor color = fx::linearOf(p.color("color"));
        // Pachislop's angle (counter-clockwise, D16) plus the layer's turn in render px (+y up)
        const double rot = -p.number("rotation") * DEG + std::atan2(-space.c, space.a);
        TitlePass pass = genPass(ctx, GenProgram::LensFlare, "lens_flare", target, space);
        pass.set(6, (float)pos.x, (float)pos.y, (float)std::max(1.0, p.number("size") * k), (float)(p.number("streak") * k));
        pass.set(7, (float)(p.number("intensity") / 100.0), (float)rot, (float)(p.number("rays") / 100.0),
                 (float)(std::fmod(seedOf(ctx, p), 97.0) * 1.37));
        pass.set(8, color.r, color.g, color.b, p.flag("ghosts") ? 1.0f : 0.0f);
        // the ghosts' axis runs through the canvas centre (render px, +y up)
        pass.set(9, (float)(centreT.x - posT.x), (float)(posT.y - centreT.y), 0.0f, 0.0f);
        ctx.usesCanvasPosition = true;
        if (ctx.tier == FxTier::Mobile) {
            drawLensFlareQuads(ctx, target, pass, space, posT, p.flag("ghosts"));
            return;
        }
        ctx.runner.run(pass);
    }

    float lensFlareReach(const ParamValues& p) {
        // the core, its bloom and the streak about the flare's offset; the bloom's tail and the
        // ghosts reach the whole canvas, which the null `bounds` (the canvas) already covers
        const TitleVec2 at = p.vec2("position");
        return (float)(std::hypot(at.x, at.y) + p.number("streak") + 6.0 * p.number("size"));
    }

    FxCost lensFlareCost(const ParamValues& p, const FxSize& size, FxTier tier) {
        const double target = std::max(1.0, (double)size.width * size.height);
        if (tier == FxTier::Mobile) {
            // the mobile tier's quads in one additive draw (drawLensFlareQuads): the core, the
            // streak (no longer than the target's diagonal) and the 6 ghosts about their axis
            // points, each at most the target; the axis runs from the flare to the canvas centre
            const double s = std::max(1.0, p.number("size") * size.pxScale);
            const double streak = std::max(1.0, p.number("streak") * size.pxScale);
            const double diag = std::hypot((double)size.width, (double)size.height);
            const TitleVec2 at = p.vec2("position");
            const double axis = std::min(diag, std::hypot(at.x, at.y) * size.pxScale);
            const double core = std::min(target, std::pow(2.0 * FLARE_CORE_REACH * s, 2.0));
            const double band = std::min(target, std::min(diag, 2.0 * (FLARE_STREAK_REACH * streak + s)) * 2.0 * (s + 1.0));
            const double ghost = std::min(target, 2.0 * (FLARE_GHOST_SHIFT * axis + FLARE_GHOST_RADIUS * s + 1.0) * 2.0 * (FLARE_GHOST_RADIUS * s + 1.0));
            const bool ghosts = p.flag("ghosts");
            return passCost(1, (core + band + (ghosts ? 6.0 * ghost : 0.0)) / target, 1, ghosts ? 8 : 2);
        }
        // one full-canvas pass; the starburst's two value-noise lookups (4 hashes each) count as taps
        return passCost(1, 2.0, 8);
    }

    GeneratorDef lensFlare() {
        GeneratorDef d;
        d.type = "lens_flare";
        d.label = "Lens Flare";
        d.category = "Light";
        d.description = "An anamorphic lens flare added as light: a hot core, a horizontal streak, a starburst, a halo ring and ghosts on the axis "
                        "through the canvas centre. Following a layer, its position is an offset from that layer's centre and it moves with it. "
                        "Set the layer to Add";
        d.params = {
            live(point("position", "Position", -380, 120, "The flare's centre, canvas px from the layer's position (+y up); following a layer, from its centre")),
            px("size", "Size", 70, 4, 1000, "The core's size"),
            px("streak", "Streak Length", 700, 0, 4000, "How far the horizontal streak reaches"),
            degrees("rotation", "Streak Angle", 0, "Turns the streak, clockwise", -180, 180),
            percent("rays", "Starburst", 60, "The starburst spikes' strength, percent", 0, 200),
            flag("ghosts", "Ghosts", true, "Draws the ghost discs along the axis through the canvas centre"),
            light(color("color", "Color", "#7cc4ff", "The flare's tint")),
            multiplies(live(percent("intensity", "Intensity", 100, "The flare's brightness, percent", 0, 400))),
            seedParam("Reshuffles the starburst and the ghosts"),
        };
        d.gizmos = {{GizmoKind::Point, "position", nullptr}, {GizmoKind::Direction, "rotation", "streak"}};
        d.follow = FollowMode::Optional;
        d.followReach = lensFlareReach;
        d.addsLight = [](const ParamValues&) { return true; };
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F2;
        d.live.kind = LiveKind::Kernel;
        d.live.params = {"position", "intensity"};
        d.live.program = (uint32_t)GenProgram::LensFlare | (uint32_t)TITLE_VARIANT_GENERATOR;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"lensFlare", "lens flare", "レンズフレア", "flare", "anamorphic flare", "anamorphic streak", "starburst", "halo ring"};
        d.inputs.analysis = AnalysisSource::Styled;
        d.estimate = lensFlareCost;
        d.presetFile = "editor/resources/titleart/fx-presets/lens_flare.yaml";
        d.render = renderLensFlare;
        return d;
    }

    // ---- Backlight ----

    constexpr int BACKLIGHT_TAPS = 40;      // each radial blur's taps (titleart/gen/backlight.glsl)
    constexpr int BACKLIGHT_MOBILE_DIVISOR = 4;     // the mobile tier's ray targets: quarter resolution

    void renderBacklight(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        // the rays come from the followed layer's own pixels: a glow or shadow on it neither emits
        // rays nor moves the centre
        if (!ctx.follow || !ctx.followContent) {
            ctx.runner.clear(target);
            return;
        }
        drawFollowed(ctx, target, [&](TargetRef& out) {
            const TitleAffine space = ctx.documentToTarget;
            TitleVec2 centre = p.vec2("center");
            if (p.string("center_mode") != "manual") {
                // an empty layer emits no rays wherever the centre is
                if (!inkCentre(ctx, *ctx.followContent, ctx.followContentKey, centre)) {
                    ctx.runner.clear(out);
                    return;
                }
            }
            // the rays at half resolution, at quarter on the mobile tier (§6.7.10)
            const int div = ctx.tier == FxTier::Mobile ? BACKLIGHT_MOBILE_DIVISOR : 2;
            const int hw = std::max(1, (ctx.width + div - 1) / div), hh = std::max(1, (ctx.height + div - 1) / div);
            fx::Scratch a = fx::scratchOf(ctx.runner, TitleBuffer::Accumulator, hw, hh);
            fx::Scratch b = fx::scratchOf(ctx.runner, TitleBuffer::Accumulator, hw, hh);
            if (!a || !b) return;
            // document space -> the reduced targets' px, which cover the full target
            const double sx = (double)hw / ctx.width, sy = (double)hh / ctx.height;
            TitleAffine halfSpace = space;
            halfSpace.a *= sx;
            halfSpace.b *= sx;
            halfSpace.tx *= sx;
            halfSpace.c *= sy;
            halfSpace.d *= sy;
            halfSpace.ty *= sy;
            const TitleVec2 c = space.apply(centre);
            const double len = std::max(1.0, p.number("length") * ctx.pxScale);
            // the source (alpha x the ray mask) blurred over the full length, then a fine pass over
            // length / 40 to fill the gaps between the taps
            TitlePass source = genPass(ctx, GenProgram::BacklightSource, "backlight rays", *a, halfSpace);
            source.textures[0].target = ctx.followContent;
            source.set(6, (float)c.x, (float)c.y, (float)centre.x, (float)centre.y);
            source.set(7, (float)ctx.width, (float)ctx.height, (float)len, 0.0f);
            // Pachislop's rotation turns counter-clockwise (y-up); ours clockwise (D16)
            source.set(8, (float)p.integer("rays"), (float)(p.number("noise") / 100.0), (float)(-p.number("rotation") * DEG),
                       (float)(std::fmod(seedOf(ctx, p), 97.0) * 3.17));
            if (!ctx.runner.run(source)) return;
            TitlePass fill = genPass(ctx, GenProgram::BacklightRadial, "backlight fill", *b, halfSpace);
            fill.textures[0].target = a.get();
            fill.set(6, (float)c.x, (float)c.y, (float)centre.x, (float)centre.y);
            fill.set(7, (float)ctx.width, (float)ctx.height, (float)(len / BACKLIGHT_TAPS), 0.0f);
            if (!ctx.runner.run(fill)) return;
            const TitleColor inner = fx::linearOf(p.color("color")), outer = fx::linearOf(p.color("outer_color"));
            TitlePass compose = genPass(ctx, GenProgram::BacklightCompose, "backlight", out, space);
            compose.textures[0].target = b.get();
            compose.set(6, (float)(p.number("intensity") / 100.0));
            compose.set(7, inner.r, inner.g, inner.b, 0.0f);
            compose.set(8, outer.r, outer.g, outer.b, 0.0f);
            ctx.runner.run(compose);
        });
    }

    float backlightReach(const ParamValues& p) {
        // the coarse ray pass and the fine gap-filling one
        return (float)(p.number("length") * (1.0 + 1.0 / BACKLIGHT_TAPS));
    }

    FxCost backlightCost(const ParamValues& p, const FxSize&, FxTier tier) {
        // two 40-tap radial blurs at half resolution (quarter on the mobile tier; the first also
        // builds the source), the colour pass at full
        const double share = tier == FxTier::Mobile ? 1.0 / (BACKLIGHT_MOBILE_DIVISOR * BACKLIGHT_MOBILE_DIVISOR) : 0.25;
        FxCost c = passCost(3, 2.0 * share * (BACKLIGHT_TAPS / 4.0) + 1.0, BACKLIGHT_TAPS);
        c.analysis = c.readback = p.string("center_mode") != "manual";
        return c;
    }

    GeneratorDef backlight() {
        GeneratorDef d;
        d.type = "backlight";
        d.label = "Backlight";
        d.category = "Light";
        d.description = "後光: god rays streaming out from behind the layer it follows, from its own silhouette (before its effects), from its "
                        "centre or a chosen point; place it below that layer";
        d.params = {
            choice("center_mode", "Light Center", "shape", {{"shape", "Shape Center"}, {"manual", "Manual"}},
                   "Where the rays radiate from: the followed layer's centre or a chosen point"),
            when(point("center", "Center", 0, 0, "The rays' centre, canvas px from the canvas centre (+y up)"), "center_mode", {"manual"}),
            px("length", "Ray Length", 260, 10, 2000, "How far the rays stream out from the silhouette"),
            multiplies(live(percent("intensity", "Intensity", 240, "The rays' brightness, percent", 0, 600))),
            light(color("color", "Inner Color", "#fff6d0", "The colour of the brightest rays")),
            light(color("outer_color", "Outer Color", "#ffa318", "The colour the rays fade to")),
            integer("rays", "Ray Count", 36, 2, 120, "About how many rays go around the circle"),
            percent("noise", "Ray Noise", 90, "How strongly the rays break up into separate beams, percent"),
            live(degrees("rotation", "Rotation", 0, "Turns the ray pattern, clockwise", -180, 180)),
            seedParam("Reshuffles the ray pattern"),
        };
        d.gizmos = {{GizmoKind::Point, "center", nullptr}, {GizmoKind::Direction, "rotation", nullptr}};
        d.follow = FollowMode::Required;
        d.followReach = backlightReach;
        d.addsLight = [](const ParamValues&) { return true; };
        d.costClass = CostClass::C2;
        d.fallback = FallbackClass::F2;
        d.live.kind = LiveKind::Kernel;
        d.live.params = {"intensity", "rotation"};
        d.live.program = (uint32_t)GenProgram::BacklightSource | (uint32_t)TITLE_VARIANT_GENERATOR;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"backlight", "god rays", "後光", "逆光", "light rays", "volumetric light", "光線", "jackpot rays"};
        d.inputs.content = true;
        d.inputs.analysis = AnalysisSource::Content;
        d.estimate = backlightCost;
        d.presetFile = "editor/resources/titleart/fx-presets/backlight.yaml";
        d.render = renderBacklight;
        return d;
    }

}

void titleart::registerLightGenerators(TitleEffectRegistry& registry) {
    registry.add(glints());
    registry.add(lensFlare());
    registry.add(backlight());
}
