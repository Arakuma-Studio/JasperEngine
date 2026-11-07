// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Lines generators (docs/specs/S83-title-effects-generators.md §6.7.3, §6.7.4, §6.7.15):
// Speed Lines, Light Streaks and Sunburst, ported from Pachislop src/generators/lines/
// {speedLines,lightStreaks,sunburst,shared}.ts by SafEight, used with the author's permission.
// They draw in their layer space as Pachislop's do; following a layer moves their centre onto
// it (its ink box from the analysis grid), the centre param then an offset from there.

#include "titleart/analysis/ReadGrid.h"
#include "titleart/analysis/SpeedLineLayout.h"
#include "titleart/generators/TitleGen.h"
#include "titleart/generators/TitleGenFamilies.h"
#include "titleart/gpu/QuadBatch.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fxparams;
using namespace jasperengine::titleart::gen;

namespace {

    constexpr double PI = 3.14159265358979323846;
    constexpr double DEG = PI / 180.0;

    // JavaScript's Math.round
    double jsRound(double x) {
        return std::floor(x + 0.5);
    }

    // shared.ts taperFractions: the shares of the length over which each end grows from a point
    // to full width; both 100 % meet in the middle
    void taperFractions(double tailPct, double headPct, float& tail, float& head) {
        const double a = std::clamp(tailPct / 100.0, 0.0, 1.0);
        const double b = std::clamp(headPct / 100.0, 0.0, 1.0);
        const double s = a + b > 1.0 ? 1.0 / (a + b) : 1.0;
        tail = (float)(a * s);
        head = (float)(b * s);
    }

    // the whole render's corners in the drawing space, in order around it (shared.ts
    // framePolygon): lines laid out over the whole frame land the same in a preview window
    std::vector<TitleVec2> framePolygon(const GeneratorContext& ctx, const TitleAffine& space) {
        const TitleRectI f = ctx.frame.empty() ? TitleRectI{0, 0, ctx.width, ctx.height} : ctx.frame;
        const TitleAffine inv = space.inverse();
        const double x0 = f.x, y0 = f.y, x1 = (double)f.x + f.width, y1 = (double)f.y + f.height;
        return {inv.apply(TitleVec2{x0, y0}), inv.apply(TitleVec2{x1, y0}), inv.apply(TitleVec2{x1, y1}), inv.apply(TitleVec2{x0, y1})};
    }

    // the followed layer's analysis grid (6 px cells over the whole render, read back once per
    // change of the layer); null without a followed layer
    std::shared_ptr<const analysis::Grid> followGrid(const GeneratorContext& ctx) {
        if (!ctx.follow) return nullptr;
        return analysis::readGrid(ctx, *ctx.follow, ctx.followKey, analysis::frameGridSpec(ctx, analysis::SPEED_LINE_CELL));
    }

    // the followed layer's ink centre in the drawing space (Pachislop's layerBounds centre taken
    // from comp space into layer space); false when it has none
    bool followCentre(const GeneratorContext& ctx, const analysis::Grid* grid, const TitleAffine& space, TitleVec2& out) {
        double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
        if (!grid || !analysis::gridInkBox(*grid, x0, y0, x1, y1)) return false;
        out = space.inverse().apply(ctx.documentToTarget.apply(TitleVec2{0.5 * (x0 + x1), 0.5 * (y0 + y1)}));
        return true;
    }

    // a following generator that may draw anywhere on the canvas
    float wholeCanvas(const ParamValues&) {
        return -1.0f;
    }

    // ---- Speed Lines ----

    analysis::SpeedLineInput speedLineInput(const GeneratorContext& ctx, const ParamValues& p) {
        using namespace analysis;
        SpeedLineInput in;
        in.type = (SpeedLineType)optionIndex(p, "type", {"focus", "flow", "flash", "beta"});
        in.shape = (SpeedLineShape)optionIndex(p, "shape", {"ellipse", "rect", "layer"});
        in.padding = p.number("padding");
        in.radius = p.vec2("radius");
        // Pachislop's flow angle turns counter-clockwise (y-up); ours clockwise on screen (D16)
        in.angle = -p.number("angle") * DEG;
        in.side = (SpeedLineSide)optionIndex(p, "side", {"both", "front", "back"});
        in.count = p.number("count");
        in.width = p.number("width");
        in.widthRandom = p.number("width_random");
        in.length = p.number("length");
        in.lengthRandom = p.number("length_random");
        in.jitter = p.number("jitter");
        in.jagCount = p.number("jag_count");
        in.jagHeight = p.number("jag_height");
        in.clump = p.number("clump");
        in.gap = p.number("gap");
        in.curve = p.number("curve");
        in.fill = (SpeedLineFill)optionIndex(p, "color_mode", {"solid", "length", "around"});
        in.color = fx::linearOf(p.color("color"));
        in.gradient = p.gradient("gradient");
        in.opacity = p.number("opacity");
        in.seed = seedOf(ctx, p);
        return in;
    }

    void renderSpeedLines(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        // the lines run to the frame's edges wherever the layer lies
        ctx.usesCanvasPosition = true;
        const TitleAffine space = ctx.layerToTarget;
        const double k = spaceScale(space);
        if (!(k > 0.0)) {
            ctx.runner.clear(target);
            return;
        }
        analysis::SpeedLineInput in = speedLineInput(ctx, p);
        in.frame = framePolygon(ctx, space);
        in.k = k;
        const std::shared_ptr<const analysis::Grid> grid = followGrid(ctx);
        TitleVec2 center = p.vec2("center");
        TitleVec2 followed;
        if (followCentre(ctx, grid.get(), space, followed)) {
            center.x += followed.x;
            center.y += followed.y;
        }
        in.center = center;
        if (grid) {
            in.silhouette = grid.get();
            in.spaceToCanvas = ctx.documentToTarget.inverse().after(space);
        }
        const analysis::SpeedLineLayout lay = analysis::layoutSpeedLines(in);

        const TitleLut* ramp = ctx.runner.lut(fx::gradientLut(in.gradient));
        if (!ramp) return;
        float tail = 0.0f, head = 0.0f;
        taperFractions(p.number("taper_in"), p.number("taper_out"), tail, head);
        const float opacity = (float)std::clamp(in.opacity / 100.0, 0.0, 1.0);
        const bool byLength = in.fill == analysis::SpeedLineFill::Length;
        // the lines in their colours, or white for the ベタフラ mask
        auto quads = [&](TargetRef& dst, bool mask) {
            QuadBatch batch;
            for (QuadInstance q : lay.quads) {
                if (mask) std::fill(q.color, q.color + 4, 1.0f);
                batch.add(q);
            }
            batch.draw(ctx, GenProgram::QuadsSpeedLines, mask ? "speed_lines spikes" : "speed_lines", dst, space, QuadBlend::Normal, 0.0f,
                       [&](TitlePass& pass) {
                           pass.textures[0].lut = ramp;
                           pass.set(6, tail, head, opacity, !mask && byLength ? 1.0f : 0.0f);
                       });
        };

        if (in.type != analysis::SpeedLineType::Beta || lay.table.empty()) {
            ctx.runner.clear(target);
            quads(target, false);
            return;
        }

        // ベタフラ: the spikes united with the focus hole, the fill outside them
        fx::Scratch lines = fx::scratchOf(ctx.runner, TitleBuffer::Accumulator, ctx.width, ctx.height);
        fx::Scratch mask = fx::scratchOf(ctx.runner, TitleBuffer::Accumulator, ctx.width, ctx.height);
        if (!lines || !mask) return;
        ctx.runner.clear(*lines);
        quads(*lines, true);
        TitleLutData data;
        data.format = ColorFormat::RGBA32F;
        data.filter = TextureFilter::NEAREST;
        data.width = analysis::SPEED_LINE_TABLE;
        data.height = 1;
        data.bytes.resize(lay.table.size() * sizeof(float));
        std::memcpy(data.bytes.data(), lay.table.data(), data.bytes.size());
        const TitleLut* table = ctx.runner.lut(data);
        if (!table) return;
        const float cx = (float)lay.center.x, cy = (float)lay.center.y;
        TitlePass hole = genPass(ctx, GenProgram::SpeedLinesBeta, "speed_lines beta mask", *mask, space);
        hole.textures[0].target = lines.get();
        hole.raw[0].lut = table;
        hole.set(6, cx, cy, 0.0f, 0.0f);
        if (!ctx.runner.run(hole)) return;
        const TitleColor color = in.color;
        TitlePass fill = genPass(ctx, GenProgram::SpeedLinesBeta, "speed_lines beta fill", target, space);
        fill.textures[0].target = mask.get();
        fill.textures[1].lut = ramp;
        fill.raw[0].lut = table;
        fill.set(6, cx, cy, 1.0f, (float)(int)in.fill);
        fill.set(7, color.r, color.g, color.b, opacity);
        ctx.runner.run(fill);
    }

    FxCost speedLinesCost(const ParamValues& p, const FxSize& size, FxTier) {
        // the lines' quads cover at most the canvas once (anti-aliased wedges, one gradient tap);
        // ベタフラ adds the mask and the fill, full-canvas passes of up to four taps
        const double area = std::max(1.0, (double)size.width * size.height);
        const double s = size.pxScale;
        const std::string type = p.string("type");
        const double n = std::clamp(p.number("count"), 1.0, (double)analysis::SPEED_LINE_MAX);
        const bool both = type == "flow" && p.string("side") == "both";
        const TitleVec2 radius = p.vec2("radius");
        // focus and flow lines run to the frame edge; flash spikes a share of the focus radius
        const double reach = type == "focus" || type == "flow" ? 0.5 * std::hypot((double)size.width, (double)size.height) / s
                                                               : std::max(20.0, std::max(radius.x, radius.y)) * p.number("length") / 100.0;
        const double lines = both ? 2.0 * n : n;
        const double fre = std::min(1.0, lines * reach * (p.number("width") + 2.0) * s * s / area);
        // curved frame-spanning lines split into a quad per 2 deg of their (at most 60 deg) turn
        const int segments = (int)std::clamp(std::ceil(std::abs(p.number("curve")) / 100.0 * 30.0), 1.0, 32.0);
        const double cpu = p.string("shape") == "layer" ? 1.5 : 0.5;
        if (type == "beta") return passCost(3, fre + 2.0, 4, (int)(lines * segments), cpu);
        return passCost(1, fre, 1, (int)(lines * segments), cpu);
    }

    GeneratorDef speedLines() {
        GeneratorDef d;
        d.type = "speed_lines";
        d.label = "Speed Lines";
        d.category = "Lines";
        d.description = "集中線 / 流線 / フラッシュ / ベタフラ: tapered wedges around a clean focus (an ellipse, a rectangle or the followed "
                        "layer's silhouette; following a layer the lines converge on it). Length is a share of the way to the frame edge "
                        "(focus, flow) or of the focus radius (flash, beta). Set the layer to Add for light-like lines";
        const std::vector<const char*> gradients = {"length", "around"};
        d.params = {
            relayout(choice("type", "Type", "focus",
                            {{"focus", "Focus Lines (集中線)"}, {"flow", "Flow Lines (流線)"}, {"flash", "Flash (フラッシュ)"}, {"beta", "Beta Flash (ベタフラ)"}},
                            "Lines from the focus to the frame edge, parallel lines past it, short spikes, or a fill cut by spikes")),
            relayout(point("center", "Center", 0, 0, "Where the lines converge, canvas px (an offset from the followed layer's centre while following one)")),
            relayout(choice("shape", "Focus Shape", "ellipse", {{"ellipse", "Ellipse"}, {"rect", "Rectangle"}, {"layer", "Layer Silhouette"}},
                            "The clear area the lines start from (Layer Silhouette needs a followed layer; without one the ellipse is used)")),
            when(relayout(px("padding", "Padding", 40, 0, 1000, "Clear space around the followed layer's silhouette")), "shape", {"layer"}),
            when(relayout(point("radius", "Focus Radius", 420, 260, "The focus ellipse's or rectangle's half width and height")), "shape",
                 {"ellipse", "rect"}),
            when(relayout(degrees("angle", "Flow Angle", 0, "The flow lines' direction, clockwise", -180, 180)), "type", {"flow"}),
            when(choice("side", "Flow Side", "both", {{"both", "Both Sides"}, {"front", "Ahead"}, {"back", "Behind"}},
                        "Which side of the focus the flow lines run from"),
                 "type", {"flow"}),
            relayout(integer("count", "Line Count", 160, 4, analysis::SPEED_LINE_MAX, "How many lines")),
            px("width", "Width", 10, 0.2, 200, "Each line's width at its widest", 0.1),
            percent("width_random", "Width Random", 60, "How much thinner each line may be at random, percent"),
            relayout(percent("length", "Length", 100, "The lines' length: a share of the way to the frame edge or of the focus radius, percent", 1, 300)),
            relayout(percent("length_random", "Length Random", 0, "How much shorter each line may be at random, percent")),
            relayout(px("jitter", "Base Jitter", 60, 0, 1000, "How far past the focus each line may start at random")),
            integer("jag_count", "Jagged Teeth", 0, 0, 64, "Teeth of a jagged focus edge around the centre (0 = smooth)"),
            relayout(px("jag_height", "Jag Height", 40, 0, 1000, "How far the teeth's tips reach past their notches")),
            integer("clump", "Clumping", 1, 1, 20, "How many lines run side by side in a clump"),
            relayout(number("gap", "Gap", 0, 0, 10, ParamUnit::None, "Empty line slots between clumps", 0.1)),
            percent("taper_in", "Taper (Focus End)", 100, "How much of the length the focus end takes to grow to full width, percent"),
            percent("taper_out", "Taper (Far End)", 0, "How much of the length the far end takes to narrow to a point, percent"),
            relayout(percent("curve", "Curve", 0, "Bends every line the same way (100 = 60 deg over half the frame diagonal), percent", -100, 100)),
            choice("color_mode", "Fill", "solid", {{"solid", "Solid"}, {"length", "Gradient (Base to Tip)"}, {"around", "Gradient (Around)"}},
                   "One colour, a gradient along each line, or a gradient around the centre"),
            when(light(color("color", "Color", "#ffffff", "The lines' colour")), "color_mode", {"solid"}),
            when(light(gradient("gradient", "Gradient", gradientValue({{0.0, "#ffffff"}, {1.0, "#ffc83a"}}), "The lines' colours")), "color_mode", gradients),
            percent("opacity", "Opacity", 85, "Each line's own opacity (overlapping lines build up, unlike the layer's), percent"),
            live(seedParam("Reshuffles the lines (a stepped key reseeds them live: every 2-3 frames strobes them)")),
        };
        d.gizmos = {{GizmoKind::Point, "center", nullptr}, {GizmoKind::Direction, "angle", nullptr}};
        d.follow = FollowMode::Optional;
        d.followReach = wholeCanvas;
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F0;
        d.live.kind = LiveKind::Kernel;
        d.live.params = {"seed"};
        d.live.program = (uint32_t)GenProgram::QuadsSpeedLines | (uint32_t)TITLE_VARIANT_GENERATOR;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"speedLines", "speed lines", "集中線", "流線", "フラッシュ", "ベタフラ", "manga lines", "focus lines", "action lines", "flow lines"};
        d.estimate = speedLinesCost;
        d.presetFile = "editor/resources/titleart/fx-presets/speed_lines.yaml";
        d.render = renderSpeedLines;
        return d;
    }

    // ---- Light Streaks ----

    constexpr int MAX_STREAKS = 200;

    struct Streak {
        double x = 0.0, y = 0.0;    // centre, layer px
        double rot = 0.0;           // radians counter-clockwise; the quad's +x is the head end
        double len = 0.0;
        double thick = 0.0;         // full width, layer px
        double intensity = 0.0;
    };

    // the streaks (lightStreaks.ts streaks): six draws each, the first anamorphic one the hero
    std::vector<Streak> streaksOf(const ParamValues& p, double seed) {
        const std::string mode = p.string("mode");
        const int n = (int)jsRound(std::clamp(p.number("count"), 1.0, (double)MAX_STREAKS));
        const TitleVec2 origin = p.vec2("origin");
        // Pachislop's angle turns counter-clockwise (y-up); ours clockwise on screen (D16)
        const double a0 = -p.number("angle") * DEG;
        const double spread = std::clamp(p.number("spread"), 0.0, 360.0) * DEG;
        const double L = std::max(1.0, p.number("length"));
        const double lr = std::clamp(p.number("length_random") / 100.0, 0.0, 1.0);
        const double T = std::max(0.1, p.number("thickness"));
        const double tr = std::clamp(p.number("thickness_random") / 100.0, 0.0, 1.0);
        Rng r(seed);
        std::vector<Streak> out;
        out.reserve((size_t)n);
        for (int i = 0; i < n; i++) {
            double h[6];
            for (double& v : h) v = r();
            const bool hero = mode == "anamorphic" && i == 0;
            Streak s;
            s.len = hero ? L : L * (1.0 - lr * h[0]);
            s.thick = hero ? T : T * (1.0 - tr * h[1]);
            s.rot = hero ? a0 : a0 + (h[2] - 0.5) * spread;
            const double dx = std::cos(s.rot), dy = std::sin(s.rot);
            double along = 0.0, side = 0.0;     // the centre along its own direction from the origin, and across it
            if (mode == "anamorphic") {
                // stacked tight around the hero (within +-2 thicknesses), slightly shifted along
                along = hero ? 0.0 : (h[4] - 0.5) * 0.3 * s.len;
                side = hero ? 0.0 : (h[3] - 0.5) * T * 4.0;
                s.intensity = hero ? 1.0 : 0.3 + 0.5 * h[5];
            } else if (mode == "spears") {
                // scattered along the flight line, each starting anywhere up to 90 % of the length out
                along = 0.9 * L * h[3] + 0.5 * s.len;
                side = (h[4] - 0.5) * T * 4.0;
                s.intensity = 0.55 + 0.45 * h[5];
            } else {
                // a bundle: every beam leaves the origin, a little staggered
                along = 0.15 * L * h[3] + 0.5 * s.len;
                s.intensity = 0.45 + 0.55 * h[5];
            }
            s.x = origin.x + dx * along - dy * side;
            s.y = origin.y + dy * along + dx * side;
            out.push_back(s);
        }
        return out;
    }

    // the halos' radii as shares of `glow` and their gains (a tight bright one, a wide soft one)
    constexpr double HALO_NARROW = 0.25, HALO_NARROW_GAIN = 1.5, HALO_WIDE_GAIN = 1.2;

    void renderLightStreaks(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        ctx.runner.clear(target);
        const TitleAffine space = ctx.layerToTarget;
        const double k = spaceScale(space);
        if (!(k > 0.0)) return;
        const std::vector<Streak> list = streaksOf(p, seedOf(ctx, p));
        const double chroma = std::max(0.0, p.number("chroma"));
        const double margin = 1.0 / k;      // one target px of anti-aliasing room past each end
        QuadBatch batch;
        for (const Streak& s : list) {
            QuadInstance q;
            q.cx = (float)s.x;
            q.cy = (float)s.y;
            q.w = (float)(s.len + 2.0 * margin);
            q.h = (float)(2.0 * (std::max(0.5 * s.thick * k, 0.5) / k + chroma + margin));
            q.rotation = (float)s.rot;
            std::fill(q.color, q.color + 4, 1.0f);
            q.extra[0] = (float)(0.5 * s.thick * k);
            q.extra[1] = (float)(s.len * k);
            q.extra[2] = (float)s.intensity;
            q.extra[3] = 1.0f;
            batch.add(q);
        }
        float tail = 0.0f, head = 0.0f;
        taperFractions(p.number("taper_tail"), p.number("taper_head"), tail, head);
        const TitleColor core = fx::linearOf(p.color("core_color")), edge = fx::linearOf(p.color("edge_color"));
        const float coreWidth = (float)std::clamp(p.number("core_width") / 100.0, 0.0, 1.0);
        const float bright = (float)std::max(0.0, p.number("brightness") / 100.0);
        // core weight 2 burns the core out to white under Add; 0 draws the edge colour alone
        auto draw = [&](TargetRef& dst, float coreWeight, const char* label) {
            batch.draw(ctx, GenProgram::QuadsLightStreaks, label, dst, space, QuadBlend::Add, 0.0f, [&](TitlePass& pass) {
                pass.set(6, core.r, core.g, core.b, coreWidth);
                pass.set(7, edge.r, edge.g, edge.b, coreWeight);
                pass.set(8, tail, head, (float)(chroma * k), bright);
            });
        };
        draw(target, 2.0f, "light_streaks");
        // the halo is blurred from the edge colour alone: the white core stays a thin hot line
        // inside a coloured bloom. Pachislop's two glowInto passes, added in one compose
        const double glow = std::max(0.0, p.number("glow"));
        if (!(glow > 0.0)) return;
        fx::Scratch edges = fx::scratchOf(ctx.runner, TitleBuffer::Accumulator, ctx.width, ctx.height);
        if (!edges) return;
        ctx.runner.clear(*edges);
        draw(*edges, 0.0f, "light_streaks halo source");
        EffectContext e = effectContext(ctx);
        fx::Scratch narrow = fx::blur(e, *edges, (float)(glow * HALO_NARROW * ctx.pxScale));
        if (!narrow) return;
        fx::Scratch wide = fx::blur(e, *edges, (float)(glow * ctx.pxScale));
        if (!wide) return;
        TitlePass pass = genPass(ctx, GenProgram::LightStreakCompose, "light_streaks halo", target, ctx.documentToTarget);
        pass.textures[0].target = narrow.get();
        pass.textures[1].target = wide.get();
        pass.set(6, (float)HALO_NARROW_GAIN, (float)HALO_WIDE_GAIN);
        pass.blend = BlendMode::LIGHT_PREMULTIPLIED;
        ctx.runner.run(pass);
    }

    // fx::blur's passes at radiusPx (TitleFx.cpp): halvings to sigma <= 6, a separable Gaussian
    // of up to 2 x 18 + 1 taps there, and the copy back to full size
    void addBlurCost(FxCost& c, double radiusPx) {
        const double sigma = radiusPx / 3.0;
        c.passes += 1;
        c.fre += 1.0;
        if (sigma < 0.3) return;
        int level = 0;
        while (sigma / std::ldexp(1.0, level) > 6.0 && level < 5) level++;
        double share = 1.0;
        for (int l = 1; l <= level; l++) {
            share *= 0.25;
            c.passes += 1;
            c.fre += share;
        }
        const int taps = 2 * std::min((int)std::ceil(sigma / std::ldexp(1.0, level) * 3.0), 24) + 1;
        c.passes += 2;
        c.fre += 2.0 * share * std::max(1.0, taps / 4.0);
        c.maxTapsPerPixel = std::max(c.maxTapsPerPixel, taps);
    }

    FxCost lightStreaksCost(const ParamValues& p, const FxSize& size, FxTier) {
        // two quad draws (the core, the halo source) over the streaks' area, the two halo blurs
        // and the compose
        const double area = std::max(1.0, (double)size.width * size.height);
        const double s = size.pxScale;
        const double n = std::clamp(p.number("count"), 1.0, (double)MAX_STREAKS);
        const double quadArea = n * (p.number("length") + 2.0) * (p.number("thickness") + 2.0 * p.number("chroma") + 2.0) * s * s;
        FxCost c = passCost(2, 2.0 * std::min(1.0, quadArea / area), 1, 2 * (int)n, 0.1);
        const double glow = std::max(0.0, p.number("glow")) * s;
        if (glow > 0.0) {
            addBlurCost(c, glow * HALO_NARROW);
            addBlurCost(c, glow);
            c.passes += 1;
            c.fre += 1.0;
            c.maxTapsPerPixel = std::max(c.maxTapsPerPixel, 2);
        }
        return c;
    }

    GeneratorDef lightStreaks() {
        GeneratorDef d;
        d.type = "light_streaks";
        d.label = "Light Streaks";
        d.category = "Lines";
        d.description = "ストリーク光: white-cored light lines with a coloured edge and halo: anamorphic lens streaks through the origin, a bundle "
                        "of beams fanning out of it, or arrow-tapered spears. Brightness above 100 % goes past white; set the layer to Add";
        d.params = {
            choice("mode", "Mode", "anamorphic", {{"anamorphic", "Anamorphic"}, {"bundle", "Bundle"}, {"spears", "Spears"}},
                   "Lens streaks stacked on the origin, beams fanning out of it, or spears flying along the angle"),
            live(point("origin", "Origin", 0, 0, "Where the streaks start or centre, canvas px from the layer's position (+y up)")),
            live(degrees("angle", "Angle", 0, "The streaks' direction, clockwise", -180, 180)),
            degrees("spread", "Spread", 3, "How far the streaks fan out around the angle", 0, 360),
            relayout(integer("count", "Count", 6, 1, MAX_STREAKS, "How many streaks")),
            live(px("length", "Length", 1600, 10, 6000, "The longest streak's length")),
            percent("length_random", "Length Random", 55, "How much shorter each streak may be at random, percent"),
            px("thickness", "Thickness", 16, 0.5, 200, "The thickest streak's width", 0.5),
            percent("thickness_random", "Thickness Random", 60, "How much thinner each streak may be at random, percent"),
            percent("taper_head", "Taper Head", 100, "How much of the length the head end takes to narrow to a point, percent"),
            percent("taper_tail", "Taper Tail", 100, "How much of the length the tail end takes to narrow to a point, percent"),
            light(color("core_color", "Core Color", "#ffffff", "The hot line along each streak's middle")),
            light(color("edge_color", "Edge Color", "#3d8bff", "The colour of the streaks' edges and halo")),
            percent("core_width", "Core Width", 25, "The core's share of each streak's width, percent"),
            multiplies(live(percent("brightness", "Brightness", 250, "How bright the streaks are (above 100 the core burns out), percent", 0, 400))),
            multiplies(px("glow", "Halo", 140, 0, 400, "How far the coloured halo spreads (0 = none)")),
            px("chroma", "Chromatic Split", 0, 0, 40, "Red and blue profiles drawn this far apart across each streak", 0.5),
            relayout(seedParam("Reshuffles the streaks' lengths, widths and places")),
        };
        d.gizmos = {{GizmoKind::Point, "origin", nullptr}, {GizmoKind::Direction, "angle", "length"}};
        d.follow = FollowMode::None;
        d.addsLight = [](const ParamValues&) { return true; };
        d.costClass = CostClass::C2;
        d.fallback = FallbackClass::F2;
        d.live.kind = LiveKind::Kernel;
        d.live.params = {"origin", "angle", "length", "brightness"};
        d.live.program = (uint32_t)GenProgram::QuadsLightStreaks | (uint32_t)TITLE_VARIANT_GENERATOR;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"lightStreaks", "light streaks", "ストリーク光", "anamorphic streak", "lens streak", "light beams", "streak"};
        d.estimate = lightStreaksCost;
        d.presetFile = "editor/resources/titleart/fx-presets/light_streaks.yaml";
        d.render = renderLightStreaks;
        return d;
    }

    // ---- Sunburst ----

    const std::vector<const char*> BURSTS = {"wedges", "focus", "rings"};

    void renderSunburst(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        // it fills the whole frame (a background) and reads the frame's corners
        ctx.usesCanvasPosition = true;
        const TitleAffine space = ctx.layerToTarget;
        if (!(spaceScale(space) > 0.0)) {
            ctx.runner.clear(target);
            return;
        }
        // on the followed layer: 0 rays everywhere, 1 only on its pixels, 2 on them and around
        const int mode = ctx.follow ? optionIndex(p, "on_layer", {"all", "clip", "over"}) : 0;
        const TargetRef* src = mode ? ctx.followContent : nullptr;
        if (mode == 1 && !src) {
            ctx.runner.clear(target);
            return;
        }
        TitleVec2 c = p.vec2("center");
        if (ctx.follow && p.string("center_mode") == "layer") {
            const std::shared_ptr<const analysis::Grid> grid = followGrid(ctx);
            TitleVec2 followed;
            if (followCentre(ctx, grid.get(), space, followed)) {
                c.x += followed.x;
                c.y += followed.y;
            }
        }
        const double duty = std::min(1.0, std::max(0.01, p.number("width") / 100.0));
        const std::string style = p.string("style");
        const int styleId = style == "wedges" ? 0 : style == "focus" ? 1 : style == "rings" ? 2 : 3;
        const TitleColor a = fx::linearOf(p.color("color_a")), b = fx::linearOf(p.color("color_b"));
        // Pachislop's rotation and twist turn counter-clockwise (y-up); ours clockwise on screen (D16)
        const double rotation = -p.number("rotation") * DEG;
        TitlePass pass = genPass(ctx, GenProgram::Sunburst, "sunburst", target, space);
        pass.textures[1].target = src;
        if (styleId < 3) {
            pass.set(6, (float)c.x, (float)c.y, (float)std::max(1.0, jsRound(p.number("rays"))), (float)(std::fmod(seedOf(ctx, p), 97.0) * 1.73));
            pass.set(7, (float)rotation, (float)std::max(0.0, p.number("inner")), (float)std::clamp(p.number("inner_softness") / 100.0, 0.0, 1.0),
                     (float)(p.number("opacity") / 100.0));
            pass.set(8, (float)(p.number("width_variation") / 100.0), (float)(p.number("length_variation") / 100.0), (float)duty,
                     (float)(src ? mode : 0));
            pass.set(9, a.r, a.g, a.b, 0.0f);
            pass.set(10, b.r, b.g, b.b, p.flag("alternate") ? 1.0f : 0.0f);
        } else {
            const TitleLut* ramp = ctx.runner.lut(fx::gradientLut(p.gradient("gradient")));
            if (!ramp) return;
            pass.textures[0].lut = ramp;
            // the gradient's radius: the farthest frame corner from the centre
            double rc = 1.0;
            for (const TitleVec2& v : framePolygon(ctx, space)) rc = std::max(rc, std::hypot(v.x - c.x, v.y - c.y));
            pass.set(6, (float)c.x, (float)c.y, (float)std::max(2.0, jsRound(p.number("rays"))), (float)(-p.number("twist") * DEG / 1000.0));
            pass.set(7, (float)rotation, (float)std::max(0.0, p.number("inner")), (float)std::clamp(p.number("softness") / 100.0, 0.0, 1.0),
                     (float)std::max(0.0, p.number("glow") / 100.0));
            pass.set(8, (float)rc, (float)duty, p.string("color_mode") == "radial" ? 1.0f : 0.0f, mode == 1 ? 1.0f : 0.0f);
            pass.set(9, a.r, a.g, a.b, 0.0f);
            pass.set(10, b.r, b.g, b.b, 0.0f);
        }
        pass.set(11, (float)styleId, src ? 1.0f : 0.0f);
        ctx.runner.run(pass);
    }

    FxCost sunburstCost(const ParamValues& p, const FxSize&, FxTier) {
        // one full-canvas pass: the gradient twice (rays, centre glow) and the followed layer once
        const bool radial = p.string("style") == "sunburst" && p.string("color_mode") == "radial";
        return passCost(1, 1.0, radial ? 3 : 1);
    }

    // with the layer's pixels only the rays stay inside its ink; otherwise they fill the canvas
    float sunburstReach(const ParamValues& p) {
        return p.string("on_layer") == "clip" ? 0.0f : -1.0f;
    }

    GeneratorDef sunburst() {
        GeneratorDef d;
        d.type = "sunburst";
        d.label = "Sunburst";
        d.category = "Lines";
        d.description = "放射 / 集中線: rays from a centre over the whole frame, an opaque background. Sunburst: two colours (or rays coloured by "
                        "a gradient along the radius) merging into a solid disc at the inner radius, with a centre glow; Twist curls the rays. "
                        "Wedges, Focus Lines and Speed Rings: rays of varying width and length around a cleared centre. Following a layer "
                        "the rays centre on it and can stay on its pixels";
        d.params = {
            choice("style", "Style", "sunburst", {{"sunburst", "Sunburst"}, {"wedges", "Wedges"}, {"focus", "Focus Lines (集中線)"}, {"rings", "Speed Rings"}},
                   "Alternating ray wedges with a glowing hub, or varying wedges, needles or broken rings around a clear centre"),
            choice("center_mode", "Center", "layer", {{"layer", "Layer Center"}, {"point", "Center Point"}},
                   "The followed layer's centre (the layer's position without one), or the Center Point"),
            choice("on_layer", "On the Layer", "all", {{"all", "Rays Everywhere"}, {"clip", "Only on Its Pixels"}, {"over", "On Its Pixels and Around"}},
                   "With a followed layer: rays everywhere, only on its pixels, or on them and around them"),
            when(point("center", "Center Point", 0, 0, "The rays' centre, canvas px (an offset from the followed layer's centre while following one)"),
                 "center_mode", {"point"}),
            integer("rays", "Rays", 24, 2, 300, "How many rays (speed rings: rings per 1,000 px)"),
            live(degrees("rotation", "Rotation", 0, "Turns the rays, clockwise", -180, 180)),
            percent("width", "Ray Width", 50, "Each ray's share of its period, percent", 2, 100),
            when(percent("width_variation", "Width Variation", 40, "How much each ray's width varies, percent"), "style", BURSTS),
            when(percent("length_variation", "Length Variation", 60, "How much each needle's start or arc's length varies, percent"), "style",
                 {"focus", "rings"}),
            when(live(degrees("twist", "Twist", 0, "Curls the rays clockwise, degrees per 1,000 px of radius", -360, 360)), "style", {"sunburst"}),
            px("inner", "Inner Radius", 140, 0, 4000, "The solid disc (sunburst) or the cleared centre (the others)"),
            when(percent("softness", "Softness", 10, "How soft the ray edges and the disc's rim are, percent"), "style", {"sunburst"}),
            when(percent("inner_softness", "Inner Softness", 60, "How soft the cleared centre's edge is, percent"), "style", BURSTS),
            when(choice("color_mode", "Ray Color", "ab", {{"ab", "Color A"}, {"radial", "Gradient Along the Radius"}},
                        "Rays in Color A, or coloured by the gradient from the centre out"),
                 "style", {"sunburst"}),
            when(color("color_a", "Color A", "#ffc93c", "The rays' colour"), "color_mode", {"ab"}),
            when(flag("alternate", "Two Colors", false, "Alternate the rays (or rings) between Color A and Color B"), "style", BURSTS),
            color("color_b", "Color B", "#c2121c", "The colour between the rays"),
            when(gradient("gradient", "Gradient", gradientValue({{0.0, "#fff3b0"}, {0.45, "#e3a82a"}, {1.0, "#7a4a0a"}}),
                          "The rays' colours from the centre to the farthest corner"),
                 "color_mode", {"radial"}),
            when(multiplies(percent("glow", "Center Glow", 70, "A whitened glow of the ray colour at the centre, percent", 0, 300)), "style", {"sunburst"}),
            when(percent("opacity", "Ray Opacity", 70, "The rays' opacity, blended like Photoshop's (in sRGB), percent"), "style", BURSTS),
            when(seedParam("Reshuffles the rays' widths, starts and arcs"), "style", BURSTS),
        };
        d.gizmos = {{GizmoKind::Point, "center", nullptr}, {GizmoKind::Radius, "inner", nullptr}, {GizmoKind::Direction, "rotation", nullptr}};
        d.follow = FollowMode::Optional;
        d.followReach = sunburstReach;
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F0;
        d.live.kind = LiveKind::Kernel;
        d.live.params = {"rotation", "twist"};
        d.live.program = (uint32_t)GenProgram::Sunburst | (uint32_t)TITLE_VARIANT_GENERATOR;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"sunburst", "放射", "rays", "light burst", "radial rays", "jackpot background", "starburst"};
        // On the Layer reads the followed layer before its own effects
        d.inputs.content = true;
        d.estimate = sunburstCost;
        d.presetFile = "editor/resources/titleart/fx-presets/sunburst.yaml";
        d.render = renderSunburst;
        return d;
    }

}

void titleart::registerLineGenerators(TitleEffectRegistry& registry) {
    registry.add(speedLines());
    registry.add(lightStreaks());
    registry.add(sunburst());
}
