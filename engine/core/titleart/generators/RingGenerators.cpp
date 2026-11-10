// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Rings generators (docs/specs/S83-title-effects-generators.md §6.7.5-6.7.7): Shockwave,
// Magic Circle and Brush Swirl, ported from Pachislop src/generators/rings/{shockwave,
// magicCircle,brushSwirl,shared}.ts by SafEight, used with the author's permission.

#include "titleart/analysis/BrushStroke.h"
#include "titleart/analysis/GlyphAtlas.h"
#include "titleart/generators/TitleGen.h"
#include "titleart/generators/TitleGenFamilies.h"
#include "titleart/gpu/QuadBatch.h"

#include "util/FontFace.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fxparams;
using namespace jasperengine::titleart::gen;

namespace {

    constexpr double PI = 3.14159265358979323846;
    constexpr double DEG = PI / 180.0;

    // ---- Shockwave ----

    constexpr int SHOCKWAVE_MAX_RINGS = 8;

    // furthest the drawn rings reach from the centre along the unsquashed axis (layer px)
    double shockwaveReach(const ParamValues& p) {
        const double R = std::max(1.0, p.number("radius"));
        const double h = std::max(0.5, p.number("width") / 2.0);
        const bool fibrous = p.string("profile") == "fibrous";
        const double soft = std::max(0.0, p.number("soft_out")) * (fibrous ? 1.7 : 1.0);
        const double smear = p.number("smear") > 0.0 ? 0.35 * R + h : 0.0;
        return R + std::max((fibrous ? 1.5 : 1.0) * h + soft, smear) + std::abs(p.number("split")) + 2.0;
    }

    void renderShockwave(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        ctx.runner.clear(target);
        const TitleAffine space = ctx.layerToTarget;
        const TitleVec2 c = p.vec2("center");
        const double E = shockwaveReach(p);
        const double squash = std::clamp(p.number("squash") / 100.0, 0.05, 1.0);
        // Pachislop's tilt turns counter-clockwise (y-up); ours is clockwise on screen (D16)
        const double tilt = -p.number("tilt") * DEG;
        const double ca = std::cos(tilt), sa = std::sin(tilt);
        const double ex = std::hypot(E * ca, E * squash * sa), ey = std::hypot(E * sa, E * squash * ca);
        const TitleRectI box = targetBox(ctx, space, c.x - ex, c.y - ey, c.x + ex, c.y + ey);
        if (box.empty()) return;
        const double R = std::max(1.0, p.number("radius"));
        const int count = (int)std::lround(std::clamp((double)p.integer("rings"), 1.0, (double)SHOCKWAVE_MAX_RINGS));
        const double spacing = std::max(0.0, p.number("spacing"));
        const double width = std::max(1.0, p.number("width"));
        const double half = width / 2.0;
        const double softIn = std::max(0.0, p.number("soft_in"));
        const double softOut = std::max(0.0, p.number("soft_out"));
        static const std::initializer_list<const char*> profiles = {"hard", "soft", "sine", "double", "fibrous"};
        const TitleColor inner = fx::linearOf(p.color("inner")), outer = fx::linearOf(p.color("outer"));
        // the inner colour from the innermost ring's soft inner edge to the outer colour at the
        // leading ring's middle
        const double innermost = std::max(0.0, R - (count - 1) * spacing - half - softIn);
        TitlePass pass = genPass(ctx, GenProgram::Shockwave, "shockwave", target, space);
        pass.scissor = box;
        pass.set(6, (float)c.x, (float)c.y, (float)ca, (float)sa);
        pass.set(7, (float)squash, (float)R, (float)spacing, (float)half);
        pass.set(8, (float)softIn, (float)softOut, (float)(2.0 + 0.12 * width), (float)count);
        pass.set(9, (float)optionIndex(p, "profile", profiles), (float)std::clamp(p.number("breakup") / 100.0, 0.0, 1.0),
                 (float)std::max(2.0, p.number("breakup_scale")), (float)std::max(0.0, p.number("smear") / 100.0));
        pass.set(10, (float)std::max(0.0, p.number("split")), (float)std::max(0.0, p.number("flash") / 100.0), (float)seedOf(ctx, p), 0.0f);
        pass.set(11, inner.r, inner.g, inner.b, (float)innermost);
        pass.set(12, outer.r, outer.g, outer.b, (float)std::max(innermost + 1.0, R));
        ctx.runner.run(pass);
    }

    FxCost shockwaveCost(const ParamValues& p, const FxSize& size, FxTier) {
        // one pass over the rings' box: its share of the target, the fibre profile's noise counted as taps
        const double E = shockwaveReach(p);
        const double area = std::min(1.0, (2.0 * E * size.pxScale) * (2.0 * E * size.pxScale) / std::max(1.0, (double)size.width * size.height));
        const int taps = p.string("profile") == "fibrous" ? 3 * p.integer("rings") + 6 : 6;
        return passCost(1, std::max(area, 0.01) * std::max(1.0, taps / 4.0), taps);
    }

    GeneratorDef shockwave() {
        GeneratorDef d;
        d.type = "shockwave";
        d.label = "Shockwave";
        d.category = "Rings";
        d.description = "衝撃波: concentric light rings with a hard, soft, sine, double or fibrous band, soft edges, squashed and tilted onto a "
                        "floor, broken up by noise, smeared radially, with a chromatic fringe and a core flash. Set the layer to Add";
        d.params = {
            point("center", "Center", 0, 0, "The rings' centre, canvas px from the layer's position (+y up)"),
            live(px("radius", "Radius", 380, 10, 3000, "The leading ring's radius")),
            integer("rings", "Rings", 2, 1, SHOCKWAVE_MAX_RINGS, "How many rings trail inward from the leading one"),
            live(px("spacing", "Ring Spacing", 60, 0, 1000, "The distance between rings")),
            live(px("width", "Width", 24, 1, 600, "Each ring's band width")),
            choice("profile", "Profile", "soft",
                   {{"hard", "Hard"}, {"soft", "Soft"}, {"sine", "Sine Ripple"}, {"double", "Double Rim"}, {"fibrous", "Fibrous"}},
                   "The brightness across the band"),
            px("soft_in", "Inner Softness", 30, 0, 800, "How far the band's light fades inward"),
            px("soft_out", "Outer Softness", 6, 0, 800, "How far the band's light fades outward"),
            percent("squash", "Squash", 100, "The ellipse's height over its width, percent (a ring lying on a floor)", 5, 100),
            degrees("tilt", "Tilt", 0, "Turns the squashed ellipse, clockwise", -90, 90),
            percent("breakup", "Breakup", 25, "Gaps noise cuts across the rings, percent"),
            px("breakup_scale", "Breakup Scale", 40, 2, 1000, "The size of the breakup's gaps"),
            percent("smear", "Radial Smear", 0, "Streaks of ring colour thrown inward and outward, percent"),
            px("split", "Chromatic Split", 0, 0, 60, "Red drawn further out and blue further in by this much"),
            multiplies(live(percent("flash", "Core Flash", 15, "A white-hot flash at the centre, percent", 0, 200))),
            light(color("inner", "Inner Color", "#ffffff", "The colour at the inner edge and of the core flash")),
            light(color("outer", "Outer Color", "#d8ecff", "The colour of the leading ring")),
            seedParam("Reshuffles the fibres, breakup and smear"),
        };
        d.gizmos = {{GizmoKind::Point, "center", nullptr}, {GizmoKind::Radius, "radius", nullptr}, {GizmoKind::Direction, "tilt", nullptr}};
        d.follow = FollowMode::None;
        d.addsLight = [](const ParamValues&) { return true; };
        d.costClass = CostClass::C1;
        d.fallback = FallbackClass::F1;
        d.live.kind = LiveKind::Kernel;
        d.live.params = {"radius", "spacing", "width", "flash"};
        d.live.program = (uint32_t)GenProgram::Shockwave | (uint32_t)TITLE_VARIANT_GENERATOR;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"shockwave", "衝撃波", "radio waves", "impact ring", "ripple rings", "当たり告知"};
        d.estimate = shockwaveCost;
        d.presetFile = "editor/resources/titleart/fx-presets/shockwave.yaml";
        d.render = renderShockwave;
        return d;
    }

    // ---- Magic Circle ----

    constexpr size_t MAGIC_CIRCLE_MAX_ROWS = 6;

    // the row types in Pachislop's ROW_TYPES order: the program's type numbers
    constexpr const char* MAGIC_ROW_TYPES[] = {"ring", "dashed", "ticks", "text", "polygon", "star2", "star3", "chevrons", "arcs", "hex"};
    constexpr int MAGIC_ROW_TEXT = 3;

    // the library fonts of §6.7.6 where S84 copies its pack fonts into a project (S84 R29:
    // assets/TitleArt/Fonts/<dir>/, upstream's file names)
    constexpr const char* MINCHO_FONT = "TitleArt/Fonts/zen-old-mincho-black/ZenOldMincho-Black.ttf";
    constexpr const char* GOTHIC_FONT = "TitleArt/Fonts/zen-kaku-gothic-new-black/ZenKakuGothicNew-Black.ttf";

    struct GlyphSet {
        const char* key;
        const char* chars;
        const char* libraryFont;
        const char* libraryName;
    };

    // Pachislop's GLYPH_SETS (`eto` is our `zodiac`, `bagua` our `trigrams`). No bundled font has
    // the Runic block, so the rune set uses Greek capitals, which read as runes on a circle
    constexpr GlyphSet GLYPH_SETS[] = {
        {"runes", "ΨΓΔΘΛΞΠΣΦΩ", MINCHO_FONT, "Zen Old Mincho Black"},
        {"kuji", "臨兵闘者皆陣列在前", MINCHO_FONT, "Zen Old Mincho Black"},
        {"zodiac", "子丑寅卯辰巳午未申酉戌亥", MINCHO_FONT, "Zen Old Mincho Black"},
        {"trigrams", "乾兌離震巽坎艮坤", MINCHO_FONT, "Zen Old Mincho Black"},
        {"hud", "SYSTEM ONLINE // TARGET LOCK 0451-7F3A // ", GOTHIC_FONT, "Zen Kaku Gothic New Black"},
    };

    struct MagicRow {
        int type = 0;
        double radius = 100.0;      // percent of the circle's radius
        double size = 6.0;          // px
        int count = 1;
        double angle = 0.0;         // degrees, Pachislop's (counter-clockwise)
    };

    int magicRowType(const std::string& name) {
        for (int i = 0; i < (int)(sizeof(MAGIC_ROW_TYPES) / sizeof(MAGIC_ROW_TYPES[0])); i++) {
            if (name == MAGIC_ROW_TYPES[i]) return i;
        }
        return 0;
    }

    // Pachislop's rowsOf over the `rows` array (its row{n}* params)
    std::vector<MagicRow> magicRows(const ParamValues& p) {
        std::vector<MagicRow> rows;
        const Json& list = p.raw("rows");
        if (!list.is_array()) return rows;
        for (const Json& r : list) {
            if (rows.size() >= MAGIC_CIRCLE_MAX_ROWS) break;
            if (!r.is_object()) continue;
            MagicRow row;
            row.type = magicRowType(r.value("type", std::string("ring")));
            row.radius = std::max(0.0, r.value("radius", 100.0));
            row.size = std::max(0.5, r.value("size", 6.0));
            row.count = std::max(1, (int)std::lround(r.value("count", 1.0)));
            // our row angles turn clockwise on screen (D16)
            row.angle = -r.value("angle", 0.0);
            rows.push_back(row);
        }
        return rows;
    }

    // furthest any row reaches from the centre in the circle's plane (layer px)
    double magicOuterRadius(const ParamValues& p, const std::vector<MagicRow>& rows) {
        const double R = std::max(1.0, p.number("radius"));
        double E = R;
        for (const MagicRow& r : rows) E = std::max(E, R * r.radius / 100.0 + r.size);
        return E + 2.0;
    }

    double magicTilt(const ParamValues& p) {
        return std::clamp(p.number("tilt"), 0.0, 80.0) * DEG;
    }

    struct LayerBox {
        double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    };

    // the layer-space box of the circle of plane radius E laid back by `tilt`, seen from `f`
    // (the viewing distance: a fixed multiple of the circle, so its shape doesn't depend on the canvas)
    LayerBox tiltedBox(double cx, double cy, double E, double tilt, double f) {
        const double sT = std::sin(tilt), cT = std::cos(tilt);
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        for (int k = 0; k < 96; k++) {
            const double a = (k / 96.0) * 2.0 * PI;
            const double u = E * std::cos(a), v = E * std::sin(a);
            const double s = f / (f + v * sT);
            x0 = std::min(x0, u * s);
            x1 = std::max(x1, u * s);
            y0 = std::min(y0, v * cT * s);
            y1 = std::max(y1, v * cT * s);
        }
        // the 96-gon sits inside the curve by at most 1 - cos(pi / 96) of its radius, magnified
        // up to 1.5x on the near side
        const double m = 0.0015 * E + 1.0;
        return {cx + x0 - m, cy + y0 - m, cx + x1 + m, cy + y1 + m};
    }

    LayerBox magicBox(const ParamValues& p, const std::vector<MagicRow>& rows) {
        const double E = magicOuterRadius(p, rows);
        const TitleVec2 c = p.vec2("center");
        return tiltedBox(c.x, c.y, E, magicTilt(p), 3.0 * E);
    }

    struct CachedGlyphAtlas {
        analysis::GlyphAtlas atlas;
        TitleLutData data;          // R8 coverage, linear: the program's TA_TEX1
        Json missing;               // glyph_missing's detail when the face lacks characters
    };

    // the problem of a glyph set the face cannot draw whole (S80's glyph_missing, risk 8)
    Json missingDetail(const GeneratorContext& ctx, const analysis::GlyphAtlas& atlas, const std::vector<uint32_t>& codepoints) {
        Json chars = Json::array();
        for (uint32_t cp : atlas.missing) {
            const size_t index = (size_t)(std::find(codepoints.begin(), codepoints.end(), cp) - codepoints.begin());
            chars.push_back(Json{{"index", index}, {"codepoint", cp}, {"char", analysis::utf8Char(cp)}});
        }
        // the project fonts that have them, most of them first, then by path (S80 R23)
        std::vector<std::pair<size_t, std::string>> covering;
        if (ctx.assets) {
            for (const ResolvedTitleAsset& asset : ctx.assets->projectFonts()) {
                const std::shared_ptr<FontFace> face = ctx.assets->loadFont(asset, 0, nullptr);
                if (!face) continue;
                size_t count = 0;
                for (uint32_t cp : atlas.missing) count += face->hasCodepoint(cp) ? 1 : 0;
                if (count) covering.push_back({count, asset.assetPath});
            }
        }
        std::sort(covering.begin(), covering.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
        Json fonts = Json::array();
        for (const auto& c : covering) fonts.push_back(c.second);
        return Json{{"chars", chars}, {"fonts_with_it", fonts}};
    }

    // The glyph atlas of the layer's set at `cell` px: built on the CPU once per (face, set,
    // cell) and kept for a few sets (a preview, an export, two layers), the least recent going
    // first; the runner uploads it once per content
    std::shared_ptr<const CachedGlyphAtlas> magicGlyphAtlas(GeneratorContext& ctx, const ParamValues& p, int cell) {
        const std::string setKey = p.string("glyphs");
        const GlyphSet* set = &GLYPH_SETS[0];
        for (const GlyphSet& s : GLYPH_SETS) {
            if (setKey == s.key) set = &s;
        }
        // a custom string draws in the magic-circle face unless the layer names its own font
        const bool custom = setKey == "custom";
        const std::string chars = custom ? p.string("custom_glyphs") : std::string(set->chars);
        const std::vector<uint32_t> codepoints = analysis::utf8Codepoints(chars);
        if (codepoints.empty()) return nullptr;
        const TitleAssetRef font = p.asset("font");
        const analysis::GlyphFont face = analysis::resolveGlyphFont(ctx.assets, font, TitleAssetRef{custom ? MINCHO_FONT : set->libraryFont, ""});
        if (!face.error.empty()) {
            TitleProblem problem;
            problem.code = "asset_missing";
            problem.layer = ctx.layerId;
            problem.path = "generator.params.font";
            problem.message = face.error + ": the text rows draw with the built-in face";
            problem.detail = Json{{"path", font.path}, {"guid", font.guid}};
            ctx.problems.push_back(std::move(problem));
        }
        if (!face.face) return nullptr;

        static std::list<std::pair<std::string, std::shared_ptr<const CachedGlyphAtlas>>> cache;
        const std::string key = face.key + "|" + chars + "|" + std::to_string(cell);
        std::shared_ptr<const CachedGlyphAtlas> atlas;
        for (auto it = cache.begin(); it != cache.end(); ++it) {
            if (it->first != key) continue;
            atlas = it->second;
            cache.splice(cache.begin(), cache, it);
            break;
        }
        if (!atlas) {
            auto made = std::make_shared<CachedGlyphAtlas>();
            made->atlas = analysis::buildGlyphAtlas(*face.face, codepoints, cell);
            made->data.format = ColorFormat::RED;
            made->data.filter = TextureFilter::LINEAR;
            made->data.width = made->atlas.cols * made->atlas.cell;
            made->data.height = made->atlas.rows * made->atlas.cell;
            made->data.bytes = made->atlas.coverage;
            if (!made->atlas.missing.empty()) made->missing = missingDetail(ctx, made->atlas, codepoints);
            atlas = made;
            cache.emplace_front(key, atlas);
            while (cache.size() > 6) cache.pop_back();
        }
        if (!atlas->atlas.missing.empty()) {
            const size_t n = atlas->atlas.missing.size();
            TitleProblem problem;
            problem.code = "glyph_missing";
            problem.layer = ctx.layerId;
            problem.path = "generator.params.glyphs";
            problem.message = std::to_string(n) + (n == 1 ? " character of the glyph set has" : " characters of the glyph set have") +
                              " no glyph in " + face.name + " (drawn as its missing-glyph box)";
            if (font.empty() && face.key.rfind("builtin:", 0) == 0) {
                problem.message += "; the set's library font, " + std::string(custom ? "Zen Old Mincho Black" : set->libraryName) +
                                   ", is not in the project";
            }
            problem.detail = atlas->missing;
            ctx.problems.push_back(std::move(problem));
        }
        return atlas;
    }

    void renderMagicCircle(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        ctx.runner.clear(target);
        const TitleAffine space = ctx.layerToTarget;
        const std::vector<MagicRow> rows = magicRows(p);
        const LayerBox b = magicBox(p, rows);
        const TitleRectI box = targetBox(ctx, space, b.x0, b.y0, b.x1, b.y1);
        if (box.empty()) return;
        const TitleVec2 c = p.vec2("center");
        const double R = std::max(1.0, p.number("radius"));
        const double E = magicOuterRadius(p, rows);
        const double tilt = magicTilt(p);
        const double f = 3.0 * E;
        // Pachislop's rotation turns counter-clockwise (y up); ours clockwise on screen (D16)
        const double spin = -p.number("rotation") * DEG;
        TitlePass pass = genPass(ctx, GenProgram::MagicCircle, "magic circle", target, space);
        pass.scissor = box;
        double fillR = 0.0, textSize = 0.0;
        for (size_t i = 0; i < rows.size(); i++) {
            const MagicRow& r = rows[i];
            const double radius = R * r.radius / 100.0;
            // type and count share a float (both small integers, exact)
            pass.set(10 + (int)i, (float)(r.type + 16 * r.count), (float)radius, (float)r.size, (float)(r.angle * DEG));
            fillR = std::max(fillR, radius);
            if (r.type == MAGIC_ROW_TEXT) textSize = std::max(textSize, r.size);
        }
        pass.set(6, (float)c.x, (float)c.y, (float)std::cos(spin), (float)std::sin(spin));
        pass.set(7, (float)std::sin(tilt), (float)std::cos(tilt), (float)f, (float)rows.size());
        pass.set(8, (float)R, (float)std::clamp(p.number("fill") / 100.0, 0.0, 1.0), (float)fillR, (float)seedOf(ctx, p));
        pass.textures[0].lut = ctx.runner.lut(fx::gradientLut(p.gradient("color")));
        if (textSize > 0.0) {
            // glyph cells at about the drawn size in target px (the near side of a tilted circle is
            // magnified up to f / (f - E sin t)), in steps of 32 px so small zooms reuse the atlas
            const double mag = f / (f - E * std::sin(tilt));
            const double px = textSize * spaceScale(space) * mag;
            const int cell = (int)std::min(512.0, std::max(32.0, std::ceil(px / 32.0) * 32.0));
            const std::shared_ptr<const CachedGlyphAtlas> atlas = magicGlyphAtlas(ctx, p, cell);
            const TitleLut* lut = atlas && !atlas->atlas.empty() ? ctx.runner.lut(atlas->data) : nullptr;
            if (lut) {
                pass.textures[1].lut = lut;
                pass.set(9, (float)atlas->atlas.cols, (float)atlas->atlas.rows, (float)atlas->atlas.count, 0.0f);
            }
        }
        ctx.runner.run(pass);
    }

    FxCost magicCircleCost(const ParamValues& p, const FxSize& size, FxTier) {
        // one pass over the circle's box; a polygon or star row's chords (up to 32 closest-point
        // tests) counted as taps, a text row's atlas tap, plus the ramp
        const std::vector<MagicRow> rows = magicRows(p);
        const LayerBox b = magicBox(p, rows);
        const double area = std::min(1.0, (b.x1 - b.x0) * (b.y1 - b.y0) * size.pxScale * size.pxScale / std::max(1.0, (double)size.width * size.height));
        int taps = 2;
        for (const MagicRow& r : rows) taps += r.type >= 4 && r.type <= 6 ? std::clamp(r.count, 2, 32) : 1;
        return passCost(1, std::max(area, 0.01) * std::max(1.0, taps / 4.0), taps);
    }

    Json magicRowValue(const char* type, double radius, double size, int count, double angle) {
        return Json{{"type", type}, {"radius", radius}, {"size", size}, {"count", count}, {"angle", angle}};
    }

    ParamDef magicRowsParam() {
        ParamDef p;
        p.key = "rows";
        p.label = "Rows";
        p.kind = ParamKind::RowArray;
        p.description = "Up to six concentric rows, each a type, a radius (percent of the circle's), a size and a count";
        p.rows = {
            choice("type", "Type", "ring",
                   {{"ring", "Ring"}, {"dashed", "Dashed Ring"}, {"ticks", "Ticks"}, {"text", "Text Ring"}, {"polygon", "Polygon"},
                    {"star2", "Star n/2"}, {"star3", "Star n/3"}, {"chevrons", "Chevrons"}, {"arcs", "Arcs"}, {"hex", "Hex Grid"}},
                   "What the row draws"),
            percent("radius", "Radius", 100, "The row's radius, percent of the circle's", 0, 150),
            px("size", "Size", 6, 0.5, 600, "Line width (ring, dashes, arcs, polygon, star), tick length, glyph or chevron height, hex band depth", 0.5),
            integer("count", "Count", 1, 1, 360, "Dashes, ticks, glyphs, sides or points, chevrons, arc slots or hex cells around"),
            live(degrees("angle", "Angle", 0, "Turns the row's slots clockwise (each row can spin at its own speed)", -180, 180)),
        };
        p.maxRows = MAGIC_CIRCLE_MAX_ROWS;
        // Pachislop's ROW_DEFAULTS (the 魔法陣 (金) stack)
        p.defaultValue = Json::array({magicRowValue("ring", 100, 6, 1, 0), magicRowValue("text", 91.5, 30, 30, 0), magicRowValue("ring", 83, 3, 1, 0),
                                      magicRowValue("star2", 83, 4, 6, 0), magicRowValue("ring", 41.5, 3, 1, 0), magicRowValue("dashed", 30, 5, 24, 0)});
        return p;
    }

    ParamDef textParam(const char* key, const char* label, const char* value, const char* doc) {
        ParamDef p;
        p.key = key;
        p.label = label;
        p.kind = ParamKind::String;
        p.defaultValue = value;
        p.description = doc;
        p.anim = FxAnim::Step;
        p.interp = ParamInterp::Stepped;
        return p;
    }

    GeneratorDef magicCircle() {
        GeneratorDef d;
        d.type = "magic_circle";
        d.label = "Magic Circle";
        d.category = "Rings";
        d.description = "魔法陣 / HUD rings: up to six concentric rows (ring, dashes, ticks, text, polygon, star, chevrons, arcs, hex grid), "
                        "rotated, optionally laid back on the floor (ホログラム台座), coloured centre to edge, with an inner fill. "
                        "Set the layer to Add";
        d.params = {
            point("center", "Center", 0, 0, "The circle's centre, canvas px from the layer's position (+y up)"),
            px("radius", "Radius", 320, 10, 3000, "The circle's radius: the rows' radii are percents of it"),
            live(degrees("rotation", "Rotation", 0, "Spins the whole circle, clockwise", -180, 180)),
            degrees("tilt", "Tilt", 0, "Lays the circle back onto a floor plane, degrees (a hologram base)", 0, 80),
            light(gradient("color", "Color", gradientValue({{0.0, "#fff6d0"}, {1.0, "#f0b030"}}), "The colour from the centre to the edge")),
            percent("fill", "Inner Fill", 6, "A disc to the outermost row, brightening toward its rim like a lit lens, percent"),
            choice("glyphs", "Glyphs", "runes",
                   {{"runes", "Runes ΨΓΔ…"}, {"kuji", "Kuji 臨兵闘者…"}, {"zodiac", "Zodiac 子丑寅卯…"}, {"trigrams", "Trigrams 乾兌離震…"},
                    {"hud", "HUD Readout"}, {"custom", "Custom"}},
                   "The characters text rows repeat around the circle"),
            when(textParam("custom_glyphs", "Custom Glyphs", "", "The characters a custom set repeats (up to 64)"), "glyphs", {"custom"}),
            asset("font", "Font", "The font of the text rows; none = the set's library font (Zen Old Mincho Black; HUD: Zen Kaku Gothic New Black)"),
            magicRowsParam(),
            seedParam("Reshuffles the random arcs"),
        };
        d.gizmos = {{GizmoKind::Point, "center", nullptr}, {GizmoKind::Radius, "radius", nullptr}, {GizmoKind::Direction, "rotation", nullptr}};
        d.follow = FollowMode::None;
        d.costClass = CostClass::C2;
        d.fallback = FallbackClass::F0;
        // the kernel runs the one pass again with new spins: `rotation` here, and each row's
        // `angle` (marked live in the row's params)
        d.live.kind = LiveKind::Kernel;
        d.live.params = {"rotation"};
        d.live.program = (uint32_t)GenProgram::MagicCircle | (uint32_t)TITLE_VARIANT_GENERATOR;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"magicCircle", "magic circle", "魔法陣", "summoning circle", "sigil", "HUD rings", "hologram base", "ホログラム台座", "rune circle"};
        d.estimate = magicCircleCost;
        d.presetFile = "editor/resources/titleart/fx-presets/magic_circle.yaml";
        d.render = renderMagicCircle;
        return d;
    }

    // ---- Brush Swirl ----

    analysis::BrushStrokeParams brushParams(const ParamValues& p, double seed) {
        static const std::initializer_list<const char*> paths = {"arc", "spiral", "slash", "ring"};
        analysis::BrushStrokeParams s;
        s.path = (analysis::BrushPath)optionIndex(p, "path", paths);
        const TitleVec2 c = p.vec2("center");
        s.cx = c.x;
        s.cy = c.y;
        s.radius = p.number("radius");
        s.sweep = p.number("sweep");
        // Pachislop's start angle turns counter-clockwise (y up); ours clockwise on screen (D16)
        s.angle = -p.number("angle");
        s.strokes = p.integer("strokes");
        s.spacing = p.number("spacing");
        s.width = p.number("width");
        s.taperIn = p.number("taper_in");
        s.taperOut = p.number("taper_out");
        s.seed = seed;
        return s;
    }

    struct CachedRibbon {
        std::vector<analysis::BrushStroke> strokes;
        std::vector<MeshVertex> mesh;
    };

    // The strokes and their ribbon, rebuilt only when a param they depend on (or the rim padding,
    // which follows px_scale) changes: Pachislop's per-layer ribbon cache, keyed by content so a
    // preview and an export of one layer each keep theirs
    std::shared_ptr<const CachedRibbon> brushRibbonFor(const analysis::BrushStrokeParams& s, double padPx) {
        char key[512];
        std::snprintf(key, sizeof(key), "%d|%.17g|%.17g|%.17g|%.17g|%.17g|%d|%.17g|%.17g|%.17g|%.17g|%.17g|%.17g", (int)s.path, s.cx, s.cy, s.radius,
                      s.sweep, s.angle, s.strokes, s.spacing, s.width, s.taperIn, s.taperOut, s.seed, padPx);
        static std::list<std::pair<std::string, std::shared_ptr<const CachedRibbon>>> cache;
        for (auto it = cache.begin(); it != cache.end(); ++it) {
            if (it->first != key) continue;
            cache.splice(cache.begin(), cache, it);
            return it->second;
        }
        auto made = std::make_shared<CachedRibbon>();
        made->strokes = analysis::buildBrushStrokes(s);
        const std::vector<analysis::BrushRibbonVertex> ribbon = analysis::brushRibbon(made->strokes, padPx);
        made->mesh.reserve(ribbon.size());
        for (const analysis::BrushRibbonVertex& r : ribbon) {
            MeshVertex v;
            v.x = r.x;
            v.y = r.y;
            v.u = r.u;
            v.v = r.across;
            v.extra[0] = r.len;
            v.extra[1] = r.hw;
            v.extra[2] = r.seed;
            v.extra[3] = r.width;
            made->mesh.push_back(v);
        }
        cache.emplace_front(key, made);
        while (cache.size() > 6) cache.pop_back();
        return made;
    }

    void renderBrushSwirl(GeneratorContext& ctx, TargetRef& target, const ParamValues& p) {
        ctx.runner.clear(target);
        const TitleAffine space = ctx.layerToTarget;
        const analysis::BrushStrokeParams s = brushParams(p, seedOf(ctx, p));
        // the ribbon is 1.5 target px wider than the ink on each side for the anti-aliased rim
        const double padPx = 1.5 / std::max(1e-6, spaceScale(space));
        const std::shared_ptr<const CachedRibbon> ribbon = brushRibbonFor(s, padPx);
        const TitleColor color = fx::linearOf(p.color("color"));
        const float dry = (float)std::clamp(p.number("dry") / 100.0, 0.0, 1.0);
        const double reveal = std::clamp(p.number("reveal", 1.0), 0.0, 1.0);
        // premultiplied over, both faces (a strip's winding follows its direction of travel)
        drawMesh(ctx, GenProgram::MeshBrushSwirl, "brush swirl", target, space, ribbon->mesh, QuadBlend::Normal, [&](TitlePass& pass) {
            pass.set(6, color.r, color.g, color.b, dry);
            pass.set(7, (float)reveal);
        });
        QuadBatch splatter;
        for (const analysis::BrushDroplet& drop : analysis::brushSplatter(ribbon->strokes, p.number("splatter"), s.seed, reveal)) {
            QuadInstance q;
            q.cx = drop.cx;
            q.cy = drop.cy;
            q.w = drop.w;
            q.h = drop.h;
            q.rotation = drop.rotation;
            q.color[0] = color.r;
            q.color[1] = color.g;
            q.color[2] = color.b;
            q.color[3] = 1.0f;
            q.extra[0] = drop.noise;
            splatter.add(q);
        }
        splatter.draw(ctx, GenProgram::QuadsSplatter, "brush swirl splatter", target, space, QuadBlend::Normal);
    }

    FxCost brushSwirlCost(const ParamValues& p, const FxSize& size, FxTier) {
        // the ribbon: every stroke's band shaded on its own (they overlap), five value noises a
        // pixel counted as taps; then one quad per droplet
        const analysis::BrushStrokeParams s = brushParams(p, p.number("seed"));
        const int strokes = std::clamp(s.strokes, 1, 12);
        const double band = (std::max(1.0, s.width) + 3.0 / std::max(1e-3, (double)size.pxScale)) * analysis::brushPathLength(s) * strokes;
        const double area = band * size.pxScale * size.pxScale / std::max(1.0, (double)size.width * size.height);
        const int taps = 6;
        const int drops = (int)std::round(std::clamp(p.number("splatter"), 0.0, 100.0) / 100.0 * analysis::BRUSH_MAX_SPLATS);
        return passCost(drops > 0 ? 2 : 1, std::max(area, 0.01) * std::max(1.0, taps / 4.0), taps, drops, 0.2);
    }

    GeneratorDef brushSwirl() {
        GeneratorDef d;
        d.type = "brush_swirl";
        d.label = "Brush Swirl";
        d.category = "Rings";
        d.description = "筆渦 / 墨: thick dry-brush strokes along an arc, an inward spiral, a 円相 ring or a 一閃 slash, tapered in and out, "
                        "with bristle gaps opening toward the dry tail and splatter droplets; Reveal draws the strokes";
        d.params = {
            point("center", "Center", 0, 0, "The swirl's centre, canvas px from the layer's position (+y up)"),
            relayout(choice("path", "Path", "arc", {{"arc", "Arc"}, {"spiral", "Spiral"}, {"slash", "Slash"}, {"ring", "Ring (円相)"}},
                            "Strokes along an arc, an inward spiral, a slash through the centre or once round")),
            relayout(px("radius", "Radius", 380, 10, 3000, "The path's radius (the slash's half length)")),
            when(relayout(degrees("sweep", "Sweep", 300, "How far the arc or spiral turns, degrees", 10, 1080)), "path", {"arc", "spiral"}),
            relayout(degrees("angle", "Start Angle", -150, "Where the strokes start, clockwise from +x", -180, 180)),
            relayout(integer("strokes", "Strokes", 4, 1, 12, "Bristle clumps side by side; the middle one is the full, widest stroke")),
            relayout(px("spacing", "Stroke Spacing", 26, 0, 400, "The distance between the strokes")),
            relayout(px("width", "Width", 70, 1, 600, "The main stroke's width")),
            relayout(percent("taper_in", "Taper In", 15, "How thin and how long the press-in is, percent")),
            relayout(percent("taper_out", "Taper Out", 85, "How thin and how long the lift-off tail is, percent")),
            percent("dry", "Dry Brush", 45, "Bristle gaps (かすれ) opening toward the tail and the rims, percent"),
            percent("splatter", "Splatter", 30, "Droplets sprayed off the sides and flicked past the ends, percent"),
            color("color", "Color", "#0b0907", "The ink's colour"),
            live(number("reveal", "Reveal", 1, 0, 1, ParamUnit::None, "How much of each stroke is drawn, 0 to 1: animate it to draw the strokes")),
            relayout(seedParam("Reshuffles the strokes' wobble, bristles and splatter")),
        };
        d.gizmos = {{GizmoKind::Point, "center", nullptr}, {GizmoKind::Radius, "radius", nullptr}, {GizmoKind::Direction, "angle", nullptr}};
        d.follow = FollowMode::None;
        d.costClass = CostClass::C2;
        d.fallback = FallbackClass::F0;
        // the kernel draws the cached ribbon again with a new cut (one uniform) and the droplets
        // thrown before it
        d.live.kind = LiveKind::Kernel;
        d.live.params = {"reveal"};
        d.live.program = (uint32_t)GenProgram::MeshBrushSwirl | (uint32_t)TITLE_VARIANT_GENERATOR;
        d.glyphSafety = GlyphSafety::Unsafe;
        d.aliases = {"brushSwirl", "brush swirl", "筆渦", "墨", "円相", "enso", "一閃", "ink brush", "sumi stroke", "brush stroke ring"};
        d.estimate = brushSwirlCost;
        d.presetFile = "editor/resources/titleart/fx-presets/brush_swirl.yaml";
        d.render = renderBrushSwirl;
        return d;
    }

}

void titleart::registerRingGenerators(TitleEffectRegistry& registry) {
    registry.add(shockwave());
    registry.add(magicCircle());
    registry.add(brushSwirl());
}
