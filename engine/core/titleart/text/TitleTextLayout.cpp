// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The layout rules (tsume, vertical cells, tate-chu-yoko, line and column pitch, per-character
// transforms, the frame) are ported from Pachislop src/text/layout.ts by SafEight, used with the
// author's permission; shaping (HarfBuzz through TextShaper, fallback fonts, variable axes)
// replaces its one glyph per code point.

#include "TitleTextLayout.h"

#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtHash.h"
#include "titleart/TitleArtIO.h"
#include "titleart/text/GlyphOutlines.h"

#include "util/DefaultFont.h"
#include "util/FontFace.h"
#include "util/TextShaper.h"

#include "hb.h"
#include "hb-ot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <list>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    constexpr double PI = 3.14159265358979323846;
    constexpr double DEG = PI / 180;
    // side bearing (em) each side of the ink box when proportional spacing fits an advance without
    // palt / vpal
    constexpr double TSUME_SIDE = 0.05;
    constexpr size_t LAYOUT_CACHE_ENTRIES = 32;

    // ---------- members ----------

    double num(const Json& o, const char* key, double fallback) {
        const auto it = o.find(key);
        return it != o.end() && it->is_number() ? it->get<double>() : fallback;
    }

    bool flag(const Json& o, const char* key, bool fallback) {
        const auto it = o.find(key);
        return it != o.end() && it->is_boolean() ? it->get<bool>() : fallback;
    }

    std::string str(const Json& o, const char* key, const std::string& fallback = {}) {
        const auto it = o.find(key);
        return it != o.end() && it->is_string() ? it->get<std::string>() : fallback;
    }

    // a positive multiplier (non-positive or missing means unchanged, as Pachislop's)
    double pos(double v) { return v > 0 ? v : 1.0; }

    const Json* entry(const Json& layer, const char* map, uint32_t index) {
        const auto m = layer.find(map);
        if (m == layer.end() || !m->is_object()) return nullptr;
        const auto e = m->find(std::to_string(index));
        return e != m->end() && e->is_object() ? &*e : nullptr;
    }

    // the members a layout depends on, canonical (CharAdjust.face only shades: left out)
    std::string layoutKey(const Json& layer) {
        static const char* const MEMBERS[] = {
            "text", "font", "face_index", "fallback_fonts", "variations", "features", "language", "size", "tracking",
            "line_height", "kerning", "align", "vertical", "tate_chu_yoko", "tate_chu_yoko_max", "proportional",
            "h_scale", "v_scale", "shear", "lines"};
        Json key = Json::object();
        for (const char* m : MEMBERS) {
            const auto it = layer.find(m);
            if (it != layer.end()) key[m] = *it;
        }
        Json chars = Json::object();
        if (const auto it = layer.find("chars"); it != layer.end() && it->is_object()) {
            for (auto c = it->begin(); c != it->end(); ++c) {
                Json adjust = c.value();
                if (adjust.is_object()) adjust.erase("face");
                chars[c.key()] = adjust;
            }
        }
        key["chars"] = chars;
        return writeTitleArtJson(key);
    }

    // ---------- characters ----------

    bool isLetterOrNumber(uint32_t cp) {
        switch (hb_unicode_general_category(hb_unicode_funcs_get_default(), cp)) {
            case HB_UNICODE_GENERAL_CATEGORY_LOWERCASE_LETTER:
            case HB_UNICODE_GENERAL_CATEGORY_MODIFIER_LETTER:
            case HB_UNICODE_GENERAL_CATEGORY_OTHER_LETTER:
            case HB_UNICODE_GENERAL_CATEGORY_TITLECASE_LETTER:
            case HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER:
            case HB_UNICODE_GENERAL_CATEGORY_DECIMAL_NUMBER:
            case HB_UNICODE_GENERAL_CATEGORY_LETTER_NUMBER:
            case HB_UNICODE_GENERAL_CATEGORY_OTHER_NUMBER:
                return true;
            default:
                return false;
        }
    }

    // proportional spacing applies to kana, CJK punctuation and full-width forms, plus other
    // full-width symbols; kanji, Latin letters and digits keep their metrics
    bool tsumeApplies(uint32_t cp, double advanceFontUnits, double upem) {
        if ((cp >= 0x3000 && cp <= 0x30ff) || (cp >= 0x31f0 && cp <= 0x31ff) || (cp >= 0xff01 && cp <= 0xff60) ||
            (cp >= 0xffe0 && cp <= 0xffe6)) {
            return true;
        }
        return advanceFontUnits >= 0.9 * upem && !isLetterOrNumber(cp);
    }

    // tate-chu-yoko candidates: printable ASCII and Latin-1 (digits, !?, x)
    bool halfWidth(uint32_t cp) { return (cp > 0x20 && cp < 0x7f) || (cp >= 0xa1 && cp <= 0xff); }

    // what draws nothing and needs no glyph (controls, default ignorables)
    bool invisible(uint32_t cp) { return cp < 0x20 || (cp >= 0x7f && cp < 0xa0) || TextShaper::isDefaultIgnorable(cp); }

    struct InkBox {
        double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
        bool empty() const { return x1 < x0; }
        void add(double x, double y) {
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
        TitleRect rect() const { return empty() ? TitleRect{} : TitleRect{x0, y0, x1 - x0, y1 - y0}; }
    };

    void addControlPoints(InkBox& box, const geometry::Path& path) {
        for (const TitleVec2& p : path.pointList()) box.add(p.x, p.y);
    }

    // ---------- faces ----------

    std::shared_ptr<FontFace> builtinFace() {
        static std::shared_ptr<FontFace> face =
            FontFace::loadMemory(roboto_regular_ttf, sizeof(roboto_regular_ttf), 0, false, "built-in (Roboto)");
        return face;
    }

    struct Face {
        std::shared_ptr<FontFace> face;
        hb_font_t* font = nullptr;      // the sub-font with the layer's variations
        double upem = 1000;
        std::string assetPath;          // "" for the built-in face
        bool palt = false, vpal = false;
    };

    bool hasGposFeature(hb_face_t* face, hb_tag_t tag) {
        unsigned int count = 0;
        const unsigned int total = hb_ot_layout_table_get_feature_tags(face, HB_OT_TAG_GPOS, 0, &count, nullptr);
        std::vector<hb_tag_t> tags(total);
        count = total;
        if (total) hb_ot_layout_table_get_feature_tags(face, HB_OT_TAG_GPOS, 0, &count, tags.data());
        return std::find(tags.begin(), tags.begin() + count, tag) != tags.begin() + count;
    }

    // ---------- layout working data ----------

    // one character's factors: size multiplier (line x char scale) and stretches
    struct Item {
        uint32_t index = 0;
        uint32_t cp = 0;
        double s = 1, hx = 1, vy = 1;
    };

    // one glyph to draw, before the per-character transforms
    struct Placed {
        uint32_t glyph = 0;
        uint16_t face = 0;
        double size = 0;                // em size, layer units (size x s)
        double sx = 1, sy = 1;          // stretch about the origin
        double ox = 0, oy = 0;          // origin, y up
    };

    // one cluster, before the per-character transforms
    struct Cell {
        uint32_t index = 0;             // first code point
        uint32_t end = 0;               // one past its last code point
        uint32_t line = 0;
        uint16_t face = 0;
        bool missing = false;
        std::vector<Placed> glyphs;
        double penX = 0, penY = 0;
        double advance = 0;
        double cx = 0, cy = 0;          // em-cell centre (the per-character pivot)
        double cellW = 0, cellH = 0;    // em-cell size
    };

    struct LineBox {
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };

}

struct TitleTextLayout::Impl {
    const AssetResolver* assets = nullptr;
    // sub-fonts per (face, canonical variations)
    std::map<std::pair<const FontFace*, std::string>, hb_font_t*> subFonts;
    // faces kept alive while their sub-fonts are cached
    std::vector<std::shared_ptr<FontFace>> heldFaces;
    text::GlyphOutlines outlines;
    // layouts by input hash, most recent first
    std::list<std::pair<uint64_t, std::unique_ptr<LaidOutText>>> layouts;
    // the code points each project font covers (missing-glyph problems), by asset path + revision
    std::map<std::string, std::shared_ptr<FontFace>> coverage;

    ~Impl() { releaseFonts(); }

    void releaseFonts() {
        for (auto& [key, font] : subFonts) hb_font_destroy(font);
        subFonts.clear();
        heldFaces.clear();
    }

    hb_font_t* subFont(const std::shared_ptr<FontFace>& face, const Json& variations, std::vector<std::string>* unknownAxes);
    void resolveFaces(const Json& layer, std::vector<Face>& faces, LaidOutText& out, const std::string& layerId);
    void reportMissing(const std::vector<std::pair<uint32_t, uint32_t>>& missing, LaidOutText& out, const std::string& layerId);
    std::unique_ptr<LaidOutText> build(const Json& layer, uint64_t hash);
};

hb_font_t* TitleTextLayout::Impl::subFont(const std::shared_ptr<FontFace>& face, const Json& variations,
                                          std::vector<std::string>* unknownAxes) {
    hb_face_t* hbFace = hb_font_get_face(face->getHbFont());
    unsigned int axisCount = hb_ot_var_get_axis_count(hbFace);
    std::vector<hb_ot_var_axis_info_t> axes(axisCount);
    if (axisCount) hb_ot_var_get_axis_infos(hbFace, 0, &axisCount, axes.data());
    // only the axes this face has, clamped to their ranges (the key is what is applied)
    std::vector<hb_variation_t> applied;
    std::string key;
    if (variations.is_object()) {
        for (auto it = variations.begin(); it != variations.end(); ++it) {
            if (!it->is_number()) continue;
            const std::string& tagText = it.key();
            const hb_tag_t tag = hb_tag_from_string(tagText.c_str(), (int)tagText.size());
            const auto axis = std::find_if(axes.begin(), axes.end(), [tag](const hb_ot_var_axis_info_t& a) { return a.tag == tag; });
            if (axis == axes.end()) {
                if (unknownAxes) unknownAxes->push_back(tagText);
                continue;
            }
            const float value = std::clamp(it->get<float>(), axis->min_value, axis->max_value);
            applied.push_back({tag, value});
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%s=%.9g;", tagText.c_str(), (double)value);
            key += buffer;
        }
    }
    const auto cacheKey = std::make_pair(face.get(), key);
    const auto found = subFonts.find(cacheKey);
    if (found != subFonts.end()) return found->second;
    // a font of its own on the shared face (not hb_font_create_sub_font: a sub-font's default
    // functions forward advances and outlines to the parent at the parent's coordinates, so its
    // variations would reach shaping only); OpenType functions, font units like the face's
    hb_font_t* sub = hb_font_create(hbFace);
    const int upem = std::max(1, face->getUnitsPerEm());
    hb_font_set_scale(sub, upem, upem);
    if (!applied.empty()) hb_font_set_variations(sub, applied.data(), (unsigned int)applied.size());
    subFonts.emplace(cacheKey, sub);
    heldFaces.push_back(face);
    return sub;
}

void TitleTextLayout::Impl::resolveFaces(const Json& layer, std::vector<Face>& faces, LaidOutText& out, const std::string& layerId) {
    const auto addProblem = [&](const char* code, const std::string& path, const std::string& message, Json detail = Json::object()) {
        TitleProblem p;
        p.severity = TitleSeverity::Warning;
        p.code = code;
        p.layer = layerId;
        p.path = path;
        p.message = message;
        p.detail = std::move(detail);
        out.problems.push_back(std::move(p));
    };
    const auto load = [&](const Json& refJson, int faceIndex, const std::string& path) -> std::pair<std::shared_ptr<FontFace>, std::string> {
        const TitleAssetRef ref = decodeAssetRef(refJson);
        if (ref.empty()) return {};
        if (!assets) {
            addProblem("asset_missing", path, "font '" + ref.path + "' not found (no asset resolver)", Json{{"path", ref.path}, {"guid", ref.guid}});
            return {};
        }
        const ResolvedTitleAsset asset = assets->resolve(ref, TitleAssetKind::Font);
        if (!asset.found) {
            addProblem("asset_missing", path, "font '" + ref.path + "' not found", Json{{"path", ref.path}, {"guid", ref.guid}});
            return {};
        }
        std::string error;
        std::shared_ptr<FontFace> face = assets->loadFont(asset, faceIndex, &error);
        if (!face) {
            addProblem("asset_missing", path, "font '" + asset.assetPath + "' did not load: " + error,
                       Json{{"path", asset.assetPath}, {"guid", asset.guid}});
            return {};
        }
        return {face, asset.assetPath};
    };
    const Json variations = layer.contains("variations") ? layer["variations"] : Json::object();
    const auto add = [&](const std::shared_ptr<FontFace>& face, const std::string& assetPath, bool primary) {
        if (!face) return;
        for (const Face& f : faces) {
            if (f.face == face) return;
        }
        std::vector<std::string> unknown;
        Face f;
        f.face = face;
        f.font = subFont(face, variations, primary ? &unknown : nullptr);
        f.upem = std::max(1, face->getUnitsPerEm());
        f.assetPath = assetPath;
        hb_face_t* hbFace = hb_font_get_face(f.font);
        f.palt = hasGposFeature(hbFace, HB_TAG('p', 'a', 'l', 't'));
        f.vpal = hasGposFeature(hbFace, HB_TAG('v', 'p', 'a', 'l'));
        faces.push_back(std::move(f));
        for (const std::string& tag : unknown) {
            addProblem("variation_axis_unknown", "variations." + tag,
                       "the font has no '" + tag + "' axis; the value is kept and ignored", Json{{"axis", tag}});
        }
    };
    const int faceIndex = (int)num(layer, "face_index", 0);
    const auto font = layer.find("font");
    if (font != layer.end() && !font->is_null()) {
        auto [face, path] = load(*font, faceIndex, "font");
        add(face, path, true);
    }
    if (const auto fallbacks = layer.find("fallback_fonts"); fallbacks != layer.end() && fallbacks->is_array()) {
        for (size_t i = 0; i < fallbacks->size(); i++) {
            auto [face, path] = load((*fallbacks)[i], 0, "fallback_fonts[" + std::to_string(i) + "]");
            add(face, path, faces.empty());
        }
    }
    std::shared_ptr<FontFace> builtin = assets ? assets->builtinFont() : nullptr;
    if (!builtin) builtin = builtinFace();
    add(builtin, "", faces.empty());
}

void TitleTextLayout::Impl::reportMissing(const std::vector<std::pair<uint32_t, uint32_t>>& missing, LaidOutText& out,
                                          const std::string& layerId) {
    if (missing.empty()) return;
    Json chars = Json::array();
    std::set<uint32_t> codepoints;
    for (const auto& [index, cp] : missing) {
        std::string utf8;
        if (cp < 0x80) {
            utf8 += (char)cp;
        } else if (cp < 0x800) {
            utf8 += (char)(0xC0 | (cp >> 6));
            utf8 += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            utf8 += (char)(0xE0 | (cp >> 12));
            utf8 += (char)(0x80 | ((cp >> 6) & 0x3F));
            utf8 += (char)(0x80 | (cp & 0x3F));
        } else {
            utf8 += (char)(0xF0 | (cp >> 18));
            utf8 += (char)(0x80 | ((cp >> 12) & 0x3F));
            utf8 += (char)(0x80 | ((cp >> 6) & 0x3F));
            utf8 += (char)(0x80 | (cp & 0x3F));
        }
        chars.push_back(Json{{"index", index}, {"codepoint", cp}, {"char", utf8}});
        codepoints.insert(cp);
    }
    // the project fonts that have them, most of them first, then by path
    std::vector<std::pair<size_t, std::string>> covering;
    if (assets) {
        for (const ResolvedTitleAsset& asset : assets->projectFonts()) {
            const std::string key = asset.path + "#" + std::to_string(asset.revision);
            auto it = coverage.find(key);
            if (it == coverage.end()) it = coverage.emplace(key, assets->loadFont(asset, 0, nullptr)).first;
            const std::shared_ptr<FontFace>& face = it->second;
            if (!face) continue;
            size_t count = 0;
            for (uint32_t cp : codepoints) count += face->hasCodepoint(cp) ? 1 : 0;
            if (count) covering.push_back({count, asset.assetPath});
        }
    }
    std::sort(covering.begin(), covering.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    Json fonts = Json::array();
    for (const auto& [count, path] : covering) fonts.push_back(path);
    TitleProblem p;
    p.severity = TitleSeverity::Warning;
    p.code = "glyph_missing";
    p.layer = layerId;
    p.path = "text";
    p.message = std::to_string(missing.size()) + (missing.size() == 1 ? " character has" : " characters have") +
                " no glyph in the font or its Fallback Fonts (drawn as the font's missing-glyph box)";
    p.detail = Json{{"chars", chars}, {"fonts_with_it", fonts}};
    out.problems.push_back(std::move(p));
}

std::unique_ptr<LaidOutText> TitleTextLayout::Impl::build(const Json& layer, uint64_t hash) {
    auto out = std::make_unique<LaidOutText>();
    LaidOutText& result = *out;
    result.hash = hash;
    const std::string layerId = str(layer, "id");

    std::vector<Face> faces;
    resolveFaces(layer, faces, result, layerId);
    if (faces.empty()) return out;
    for (const Face& f : faces) result.faces.push_back(f.face);

    const double size = num(layer, "size", 200);
    const Face& primary = faces.front();
    const double pscale = size / primary.upem;
    const double asc = primary.face->getAscender() * pscale;
    const double desc = primary.face->getDescender() * pscale;     // negative
    const double emMid = (asc + desc) / 2;                          // em-box centre above the baseline
    const double track = num(layer, "tracking", 0) / 1000 * size;
    const double lineAdv = size * num(layer, "line_height", 1.1);
    const double tsume = std::clamp(num(layer, "proportional", 0), 0.0, 1.0);
    const bool vertical = flag(layer, "vertical", false);
    const bool tcy = flag(layer, "tate_chu_yoko", true);
    const int tcyMax = std::clamp((int)num(layer, "tate_chu_yoko_max", 3), 1, 4);
    const std::string align = str(layer, "align", "center");
    const double alignFrac = align == "start" ? 0 : align == "center" ? 0.5 : 1;
    result.vertical = vertical;

    std::vector<uint32_t> cps;
    const std::string text = str(layer, "text");
    TextShaper::decodeUtf8(text.data(), text.size(), cps);

    // the shaper's options and per-face font scales
    std::vector<const FontFace*> chain;
    std::vector<float> scales;
    ShapeOptions options;
    for (const Face& f : faces) {
        chain.push_back(f.face.get());
        scales.push_back((float)(size / f.upem));
        options.fonts.push_back(f.font);
    }
    options.kerning = flag(layer, "kerning", true);
    options.language = str(layer, "language");
    if (const auto features = layer.find("features"); features != layer.end() && features->is_object()) {
        for (auto it = features->begin(); it != features->end(); ++it) {
            if (!it->is_number_integer()) continue;
            const std::string& tag = it.key();
            options.features.push_back({hb_tag_from_string(tag.c_str(), (int)tag.size()), it->get<uint32_t>()});
        }
    }
    const auto withFeature = [&](ShapeDirection direction, hb_tag_t extra) {
        ShapeOptions o = options;
        o.direction = direction;
        if (extra) o.features.push_back({extra, 1});
        return o;
    };

    // lines with their character factors
    struct Line {
        uint32_t start = 0, end = 0;    // code point range (without the newline)
        double ls = 1, lh = 1, lv = 1;
        std::vector<Item> items;
    };
    std::vector<Line> lines(1);
    for (uint32_t i = 0; i <= cps.size(); i++) {
        if (i == cps.size() || cps[i] == '\n') {
            // a CR of a CR LF pair belongs to the break, never to the line's glyphs
            lines.back().end = (i > lines.back().start && i < cps.size() && cps[i - 1] == '\r') ? i - 1 : i;
            if (i < cps.size()) {
                lines.emplace_back();
                lines.back().start = i + 1;
            }
        }
    }
    const double srcH = pos(num(layer, "h_scale", 1)), srcV = pos(num(layer, "v_scale", 1));
    for (size_t li = 0; li < lines.size(); li++) {
        Line& line = lines[li];
        const Json* la = entry(layer, "lines", (uint32_t)li);
        line.ls = la ? pos(num(*la, "scale", 1)) : 1;
        line.lh = srcH * (la ? pos(num(*la, "h_scale", 1)) : 1);
        line.lv = srcV * (la ? pos(num(*la, "v_scale", 1)) : 1);
        for (uint32_t i = line.start; i < line.end; i++) {
            const Json* ca = entry(layer, "chars", i);
            Item it;
            it.index = i;
            it.cp = cps[i];
            it.s = line.ls * (ca ? pos(num(*ca, "scale", 1)) : 1);
            it.hx = line.lh * (ca ? pos(num(*ca, "h_scale", 1)) : 1);
            it.vy = line.lv * (ca ? pos(num(*ca, "v_scale", 1)) : 1);
            line.items.push_back(it);
        }
    }
    const auto itemAt = [&](const Line& line, uint32_t index) -> const Item& { return line.items[index - line.start]; };

    // proportional adjustment (along the writing direction, layer px at the base size) of a glyph
    // shaped plainly (`a`) and with palt / vpal (`b`), or fitted to its ink
    struct PropAdjust {
        double pla = 0, adv = 0;
        bool any = false;
    };
    const auto fitToInk = [&](const Face& f, uint32_t glyph, bool vert) {
        PropAdjust a;
        hb_glyph_extents_t ext{};
        if (!hb_font_get_glyph_extents(f.font, glyph, &ext) || ext.width <= 0 || ext.height >= 0) return a;
        const double k = size / f.upem;
        const double side = 2 * TSUME_SIDE * f.upem;
        const double bx1 = ext.x_bearing, bx2 = ext.x_bearing + ext.width;
        const double by2 = ext.y_bearing, by1 = ext.y_bearing + ext.height;
        if (!vert) {
            const double full = hb_font_get_glyph_h_advance(f.font, glyph);
            const double ink = bx2 - bx1;
            if (ink + side < full) a = {(side / 2 - bx1) * k, (ink + side - full) * k, true};
        } else {
            // the vertical cell is one em with the em box centred in it; the cell top stays put,
            // so the shorter cell's centre rises by half the saving
            const double ink = by2 - by1;
            if (ink + side < f.upem) {
                const double save = f.upem - ink - side;
                const double fasc = f.face->getAscender(), fdesc = f.face->getDescender();
                a = {(save / 2 - ((by1 + by2) / 2 - (fasc + fdesc) / 2)) * k, -save * k, true};
            }
        }
        return a;
    };

    std::vector<Cell> cells;
    std::vector<std::pair<uint32_t, uint32_t>> missing;
    std::vector<LineBox> lineBoxes(lines.size());

    // the clusters of shaped glyphs (logical code point ranges, glyph runs in visual order)
    struct ShapedCluster {
        size_t first = 0, count = 0;
        uint32_t index = 0, end = 0;
    };
    const auto groupClusters = [&](const std::vector<ShapedGlyph>& glyphs, uint32_t rangeEnd) {
        std::vector<ShapedCluster> out;
        std::set<uint32_t> starts;
        for (const ShapedGlyph& g : glyphs) starts.insert((uint32_t)g.cluster);
        for (size_t i = 0; i < glyphs.size();) {
            size_t j = i + 1;
            while (j < glyphs.size() && glyphs[j].cluster == glyphs[i].cluster) j++;
            ShapedCluster c;
            c.first = i;
            c.count = j - i;
            c.index = (uint32_t)glyphs[i].cluster;
            const auto next = starts.upper_bound(c.index);
            c.end = next == starts.end() ? rangeEnd : *next;
            out.push_back(c);
            i = j;
        }
        return out;
    };
    // a cluster no face covers: every glyph .notdef while it has something visible; its code
    // points are reported
    const auto clusterMissing = [&](const ShapedGlyph* glyphs, size_t count, uint32_t index, uint32_t end) {
        bool none = true;
        for (size_t g = 0; g < count; g++) none = none && glyphs[g].glyphIndex == 0;
        if (!none) return false;
        bool visible = false;
        for (uint32_t i = index; i < end; i++) visible = visible || !invisible(cps[i]);
        if (!visible) return false;
        for (uint32_t i = index; i < end; i++) {
            if (!invisible(cps[i])) missing.push_back({i, cps[i]});
        }
        return true;
    };
    // the same over every cluster of a shaped run (a tate-chu-yoko cell holds several)
    const auto runMissing = [&](const std::vector<ShapedGlyph>& glyphs, uint32_t rangeEnd) {
        bool any = false;
        for (const ShapedCluster& sc : groupClusters(glyphs, rangeEnd)) {
            any = clusterMissing(&glyphs[sc.first], sc.count, sc.index, sc.end) || any;
        }
        return any;
    };

    if (!vertical) {
        struct Row {
            std::vector<size_t> cells;      // indexes into `cells`
            double width = 0, vf = 1;
        };
        std::vector<Row> rows(lines.size());
        for (size_t li = 0; li < lines.size(); li++) {
            const Line& line = lines[li];
            Row& row = rows[li];
            row.vf = line.items.empty() ? line.ls * line.lv : 0;
            for (const Item& it : line.items) row.vf = std::max(row.vf, it.s * it.vy);
            if (line.items.empty()) continue;
            std::vector<ShapedGlyph> shaped, proportional;
            TextShaper::shapeRange(chain, scales, cps, line.start, line.end - line.start, (unsigned)li,
                                   withFeature(ShapeDirection::Horizontal, 0), shaped);
            bool paltMatches = false;
            if (tsume > 0) {
                TextShaper::shapeRange(chain, scales, cps, line.start, line.end - line.start, (unsigned)li,
                                       withFeature(ShapeDirection::Horizontal, HB_TAG('p', 'a', 'l', 't')), proportional);
                paltMatches = proportional.size() == shaped.size();
                for (size_t g = 0; paltMatches && g < shaped.size(); g++) {
                    paltMatches = proportional[g].glyphIndex == shaped[g].glyphIndex && proportional[g].cluster == shaped[g].cluster;
                }
            }
            const std::vector<ShapedCluster> clusters = groupClusters(shaped, line.end);
            double x = 0;
            for (size_t c = 0; c < clusters.size(); c++) {
                const ShapedCluster& sc = clusters[c];
                const Item& it = itemAt(line, sc.index);
                const ShapedGlyph& base = shaped[sc.first];
                const Face& f = faces[base.face];
                double adv = 0;
                for (size_t g = sc.first; g < sc.first + sc.count; g++) adv += shaped[g].xAdvance;
                double pla = 0;
                if (tsume > 0 && tsumeApplies(it.cp, hb_font_get_glyph_h_advance(f.font, base.glyphIndex), f.upem)) {
                    PropAdjust a;
                    if (f.palt && paltMatches) {
                        double advP = 0;
                        for (size_t g = sc.first; g < sc.first + sc.count; g++) advP += proportional[g].xAdvance;
                        a = {proportional[sc.first].xOffset - base.xOffset, advP - adv, true};
                    } else if (!f.palt) {
                        a = fitToInk(f, base.glyphIndex, false);
                    }
                    if (a.any) {
                        adv += a.adv * tsume;
                        pla = a.pla * tsume;
                    }
                }
                if (c > 0) {
                    const Item& prev = itemAt(line, clusters[c - 1].index);
                    x += track * prev.s;
                }
                const double fx = it.s * it.hx, fy = it.s * it.vy;
                Cell cell;
                cell.index = sc.index;
                cell.end = sc.end;
                cell.line = (uint32_t)li;
                cell.face = base.face;
                cell.missing = clusterMissing(&shaped[sc.first], sc.count, sc.index, sc.end);
                cell.penX = x;
                cell.advance = adv * fx;
                double gx = 0;
                for (size_t g = sc.first; g < sc.first + sc.count; g++) {
                    const ShapedGlyph& sg = shaped[g];
                    Placed p;
                    p.glyph = sg.glyphIndex;
                    p.face = sg.face;
                    p.size = size * it.s;
                    p.sx = it.hx;
                    p.sy = it.vy;
                    p.ox = x + (gx + sg.xOffset + pla) * fx;
                    p.oy = sg.yOffset * fy;     // the baseline is added with the row
                    gx += sg.xAdvance;
                    cell.glyphs.push_back(p);
                }
                cell.cellW = cell.advance;
                cell.cellH = size * fy;
                x += cell.advance;
                row.cells.push_back(cells.size());
                cells.push_back(std::move(cell));
            }
            row.width = x;
        }
        double W = 0;
        for (const Row& r : rows) W = std::max(W, r.width);
        // each line box is vf x lineAdv tall with the em box (asc - desc) centred in it, so the
        // baseline step is the previous box's part below its baseline plus this one's above:
        // vf x lineAdv for uniform lines, and a small line over a big one sits tight
        double baseline = 0;
        for (size_t li = 0; li < rows.size(); li++) {
            const Row& row = rows[li];
            if (li > 0) baseline -= (rows[li - 1].vf * (lineAdv - asc - desc) + row.vf * (lineAdv + asc + desc)) / 2;
            const Json* adj = entry(layer, "lines", (uint32_t)li);
            const TitleVec2 off = adj && adj->contains("offset") ? decodeVec2((*adj)["offset"]) : TitleVec2{};
            const double shift = alignFrac * (W - row.width) + off.x / 100 * size;
            const double oy = baseline + off.y / 100 * size;
            for (size_t ci : row.cells) {
                Cell& cell = cells[ci];
                const Item& it = itemAt(lines[li], cell.index);
                cell.penX += shift;
                cell.penY = oy;
                for (Placed& p : cell.glyphs) {
                    p.ox += shift;
                    p.oy += oy;
                }
                cell.cx = cell.penX + cell.advance / 2;
                cell.cy = oy + emMid * it.s * it.vy;
            }
            const double centre = oy + row.vf * emMid;
            lineBoxes[li] = {shift, centre - row.vf * lineAdv / 2, shift + row.width, centre + row.vf * lineAdv / 2};
        }
    } else {
        struct Column {
            std::vector<size_t> cells;      // indexes into `cells`, top to bottom
            std::vector<double> h, f;       // per cell: height and vertical size factor
            double height = 0, wf = 1;
        };
        std::vector<Column> cols(lines.size());
        for (size_t li = 0; li < lines.size(); li++) {
            const Line& line = lines[li];
            Column& col = cols[li];
            col.wf = line.items.empty() ? line.ls * line.lh : 0;
            for (const Item& it : line.items) col.wf = std::max(col.wf, it.s * it.hx);
            size_t i = 0;
            while (i < line.items.size()) {
                size_t j = i;
                if (tcy) {
                    while (j < line.items.size() && halfWidth(line.items[j].cp)) j++;
                }
                if (j > i && (int)(j - i) <= tcyMax) {
                    // tate-chu-yoko: the run set upright side by side, compressed to one column em
                    // when wider, its ink centred in a cell as tall as its tallest character
                    const uint32_t start = line.items[i].index, end = line.items[j - 1].index + 1;
                    std::vector<ShapedGlyph> shaped;
                    TextShaper::shapeRange(chain, scales, cps, start, end - start, (unsigned)li, withFeature(ShapeDirection::Horizontal, 0), shaped);
                    double px = 0;
                    double y0 = std::numeric_limits<double>::infinity(), y1 = -y0;
                    struct At {
                        size_t glyph;
                        double at, adv;
                    };
                    std::vector<At> xs;
                    double f = 0;
                    for (size_t k = i; k < j; k++) f = std::max(f, line.items[k].s * line.items[k].vy);
                    for (size_t g = 0; g < shaped.size(); g++) {
                        const ShapedGlyph& sg = shaped[g];
                        const Item& it = itemAt(line, (uint32_t)sg.cluster);
                        const double k = it.s * it.hx;
                        const double adv = sg.xAdvance * k;
                        xs.push_back({g, px + sg.xOffset * k, adv});
                        px += adv;
                        hb_glyph_extents_t ext{};
                        const Face& fc = faces[sg.face];
                        if (hb_font_get_glyph_extents(fc.font, sg.glyphIndex, &ext) && ext.height < 0) {
                            const double kk = size / fc.upem * it.s * it.vy;
                            y0 = std::min(y0, (ext.y_bearing + ext.height) * kk);
                            y1 = std::max(y1, ext.y_bearing * kk);
                        }
                    }
                    const double q = std::min(1.0, size * line.ls * line.lh / std::max(px, 1e-9));
                    const double h = size * f;
                    const double inkMid = y0 <= y1 ? (y0 + y1) / 2 : emMid * f;
                    Cell cell;
                    cell.index = start;
                    cell.end = end;
                    cell.line = (uint32_t)li;
                    cell.face = shaped.empty() ? 0 : shaped[0].face;
                    cell.missing = runMissing(shaped, end);
                    cell.advance = h;
                    for (const At& a : xs) {
                        const ShapedGlyph& sg = shaped[a.glyph];
                        const Item& it = itemAt(line, (uint32_t)sg.cluster);
                        Placed p;
                        p.glyph = sg.glyphIndex;
                        p.face = sg.face;
                        p.size = size * it.s;
                        p.sx = it.hx * q;
                        p.sy = it.vy;
                        p.ox = (a.at - px / 2) * q;             // from the column centre
                        p.oy = -h / 2 - inkMid + sg.yOffset * it.s * it.vy;   // from the cell top
                        cell.glyphs.push_back(p);
                    }
                    cell.cellW = std::min(px, size * line.ls * line.lh);
                    cell.cellH = h;
                    col.cells.push_back(cells.size());
                    col.h.push_back(h);
                    col.f.push_back(f);
                    cells.push_back(std::move(cell));
                    i = j;
                    continue;
                }
                // one cell per character (runs too long for tate-chu-yoko included): the run up
                // to the next tate-chu-yoko candidate, shaped top to bottom for its vertical forms
                size_t end = std::max(j, i + 1);
                if (end == i + 1 && !(tcy && halfWidth(line.items[i].cp))) {
                    while (end < line.items.size() && !(tcy && halfWidth(line.items[end].cp))) end++;
                }
                const uint32_t start = line.items[i].index, stop = line.items[end - 1].index + 1;
                std::vector<ShapedGlyph> shaped, proportional;
                TextShaper::shapeRange(chain, scales, cps, start, stop - start, (unsigned)li, withFeature(ShapeDirection::Vertical, 0), shaped);
                bool vpalMatches = false;
                if (tsume > 0) {
                    TextShaper::shapeRange(chain, scales, cps, start, stop - start, (unsigned)li,
                                           withFeature(ShapeDirection::Vertical, HB_TAG('v', 'p', 'a', 'l')), proportional);
                    vpalMatches = proportional.size() == shaped.size();
                    for (size_t g = 0; vpalMatches && g < shaped.size(); g++) {
                        vpalMatches = proportional[g].glyphIndex == shaped[g].glyphIndex && proportional[g].cluster == shaped[g].cluster;
                    }
                }
                for (const ShapedCluster& sc : groupClusters(shaped, stop)) {
                    const Item& it = itemAt(line, sc.index);
                    const ShapedGlyph& base = shaped[sc.first];
                    const Face& fc = faces[base.face];
                    const double f = it.s * it.vy;
                    const double em = size * f;
                    // the em box centred in a full em cell; proportional spacing shortens the cell
                    // from the bottom and moves the glyph (vpal: + = up)
                    double h = em, shift = 0;
                    if (tsume > 0 && tsumeApplies(it.cp, hb_font_get_glyph_h_advance(fc.font, base.glyphIndex), fc.upem)) {
                        PropAdjust a;
                        if (fc.vpal && vpalMatches) {
                            const double plain = -base.yAdvance, prop = -proportional[sc.first].yAdvance;
                            a = {proportional[sc.first].yOffset - base.yOffset, prop - plain, true};
                        } else if (!fc.vpal) {
                            a = fitToInk(fc, base.glyphIndex, true);
                        }
                        if (a.any) {
                            h += a.adv * tsume * f;
                            shift = a.pla * tsume * f;
                        }
                    }
                    const double w = hb_font_get_glyph_h_advance(fc.font, base.glyphIndex) * (size / fc.upem) * it.s * it.hx;
                    Cell cell;
                    cell.index = sc.index;
                    cell.end = sc.end;
                    cell.line = (uint32_t)li;
                    cell.face = base.face;
                    cell.missing = clusterMissing(&shaped[sc.first], sc.count, sc.index, sc.end);
                    cell.advance = h;
                    for (size_t g = sc.first; g < sc.first + sc.count; g++) {
                        const ShapedGlyph& sg = shaped[g];
                        Placed p;
                        p.glyph = sg.glyphIndex;
                        p.face = sg.face;
                        p.size = size * it.s;
                        p.sx = it.hx;
                        p.sy = it.vy;
                        // marks keep their offsets from the cluster's base glyph
                        p.ox = -w / 2 + (sg.xOffset - base.xOffset) * it.s * it.hx;
                        p.oy = -em / 2 - emMid * f + shift + (sg.yOffset - base.yOffset) * f;
                        cell.glyphs.push_back(p);
                    }
                    cell.cellW = size * it.s * it.hx;
                    cell.cellH = h;
                    col.cells.push_back(cells.size());
                    col.h.push_back(h);
                    col.f.push_back(f);
                    cells.push_back(std::move(cell));
                }
                i = end;
            }
            for (size_t k = 0; k < col.cells.size(); k++) col.height += col.h[k] + (k > 0 ? track * col.f[k - 1] : 0);
            col.height = std::max(0.0, col.height);
        }
        double H = 0;
        for (const Column& c : cols) H = std::max(H, c.height);
        // columns run right to left; the pitch follows both columns' widths
        double colX = 0;
        for (size_t ci = 0; ci < cols.size(); ci++) {
            const Column& col = cols[ci];
            if (ci > 0) colX -= (cols[ci - 1].wf + col.wf) / 2 * lineAdv;
            const Json* adj = entry(layer, "lines", (uint32_t)ci);
            const TitleVec2 off = adj && adj->contains("offset") ? decodeVec2((*adj)["offset"]) : TitleVec2{};
            const double x = colX + off.x / 100 * size;
            double top = -(alignFrac * (H - col.height) - off.y / 100 * size);
            const double colTop = top;
            for (size_t k = 0; k < col.cells.size(); k++) {
                if (k > 0) top -= track * col.f[k - 1];
                Cell& cell = cells[col.cells[k]];
                cell.penX = x;
                cell.penY = top;
                cell.cx = x;
                cell.cy = top - col.h[k] / 2;
                for (Placed& p : cell.glyphs) {
                    p.ox += x;
                    p.oy += top;
                }
                top -= col.h[k];
            }
            lineBoxes[ci] = {x - col.wf * lineAdv / 2, colTop - col.height, x + col.wf * lineAdv / 2, colTop};
        }
    }

    // ---- outlines, the frame and the per-character transforms ----
    // the frame is the block before per-character offsets and rotations (scales reflow, so they
    // are part of it): centred vertically, sheared about its centre line, then the sheared frame
    // centred horizontally. Offsets and rotations never move the frame.
    struct Drawn {
        size_t cell, glyph;
        geometry::Path local;       // the outline before the per-character transforms
        geometry::Affine toLocal;   // font units -> that space
    };
    std::vector<Drawn> drawn;
    InkBox raw;
    for (size_t c = 0; c < cells.size(); c++) {
        for (size_t g = 0; g < cells[c].glyphs.size(); g++) {
            const Placed& p = cells[c].glyphs[g];
            const Face& f = faces[p.face];
            const geometry::Path& outline = outlines.outline(f.font, p.glyph);
            const double k = p.size / f.upem;
            const geometry::Affine m{k * p.sx, 0, 0, k * p.sy, p.ox, p.oy};
            Drawn d{c, g, outline, m};
            d.local.transform(m);
            const bool ink = std::any_of(outline.verbList().begin(), outline.verbList().end(),
                                         [](geometry::PathVerb v) { return v != geometry::PathVerb::Move && v != geometry::PathVerb::Close; });
            if (ink) addControlPoints(raw, d.local);
            drawn.push_back(std::move(d));
        }
    }
    const double k = std::tan(num(layer, "shear", 0) * DEG);
    double my = 0, mx = 0;
    if (!raw.empty()) {
        my = (raw.y0 + raw.y1) / 2;
        double fx0 = raw.x0, fx1 = raw.x1;
        if (k != 0) {
            InkBox sheared;
            for (const Drawn& d : drawn) {
                for (const TitleVec2& p : d.local.pointList()) sheared.add(p.x + k * (p.y - my), p.y);
            }
            fx0 = sheared.x0;
            fx1 = sheared.x1;
        }
        mx = (fx0 + fx1) / 2;
        result.frame = {fx0 - mx, raw.y0 - my, fx1 - fx0, raw.y1 - raw.y0};
    }
    // shear about the centre line and centring: x' = x + k (y - my) - mx, y' = y - my
    const geometry::Affine centring{1, 0, k, 1, -k * my - mx, -my};

    InkBox inkAll;
    std::vector<size_t> drawnOfCell(cells.size(), drawn.size());
    for (size_t i = drawn.size(); i-- > 0;) drawnOfCell[drawn[i].cell] = i;
    for (size_t c = 0; c < cells.size(); c++) {
        const Cell& cell = cells[c];
        const Json* adj = entry(layer, "chars", cell.index);
        const double rotation = adj ? num(*adj, "rotation", 0) : 0;
        const TitleVec2 off = adj && adj->contains("offset") ? decodeVec2((*adj)["offset"]) : TitleVec2{};
        // per-character rotation about the em-cell centre and offset, in pre-shear layout space;
        // clockwise on screen is negative in y-up
        const geometry::Affine adjust = geometry::Affine::translate(cell.cx + off.x / 100 * size, cell.cy + off.y / 100 * size)
                                            .after(geometry::Affine::rotateDegrees(-rotation))
                                            .after(geometry::Affine::translate(-cell.cx, -cell.cy));
        const geometry::Affine place = centring.after(adjust);

        LaidOutCluster out;
        out.index = cell.index;
        for (uint32_t i = cell.index; i < cell.end; i++) out.codepoints.push_back(cps[i]);
        out.line = cell.line;
        out.face = cell.face;
        out.missing = cell.missing;
        out.advance = cell.advance;
        out.pen = centring.apply({cell.penX, cell.penY});
        const TitleVec2 centre = place.apply({cell.cx, cell.cy});
        out.emCell = {centre.x - cell.cellW / 2, centre.y - cell.cellH / 2, cell.cellW, cell.cellH};
        InkBox ink;
        for (size_t g = 0; g < cell.glyphs.size(); g++) {
            const Placed& p = cell.glyphs[g];
            const Drawn& d = drawn[drawnOfCell[c] + g];
            LaidOutGlyph glyph;
            glyph.glyphIndex = p.glyph;
            glyph.face = p.face;
            glyph.position = place.apply({p.ox, p.oy});
            glyph.scaleX = d.toLocal.a;
            glyph.scaleY = d.toLocal.d;
            glyph.rotation = rotation;
            glyph.upright = vertical;
            glyph.transform = place.after(d.toLocal);
            glyph.outline = d.local;
            glyph.outline.transform(place);
            if (!glyph.outline.empty()) addControlPoints(ink, glyph.outline);
            out.glyphs.push_back(std::move(glyph));
        }
        out.inkBox = ink.rect();
        if (!ink.empty()) {
            inkAll.add(ink.x0, ink.y0);
            inkAll.add(ink.x1, ink.y1);
        }
        result.clusters.push_back(std::move(out));
    }
    result.inkBounds = inkAll.rect();

    for (size_t li = 0; li < lines.size(); li++) {
        LaidOutLine line;
        line.firstCluster = (uint32_t)result.clusters.size();
        for (size_t c = 0; c < result.clusters.size(); c++) {
            if (result.clusters[c].line == li) {
                line.firstCluster = std::min(line.firstCluster, (uint32_t)c);
                line.clusterCount++;
            }
        }
        if (line.clusterCount == 0) line.firstCluster = 0;
        const LineBox& b = lineBoxes[li];
        InkBox box;
        for (const TitleVec2& p : {TitleVec2{b.x0, b.y0}, TitleVec2{b.x1, b.y0}, TitleVec2{b.x0, b.y1}, TitleVec2{b.x1, b.y1}}) {
            const TitleVec2 q = centring.apply(p);
            box.add(q.x, q.y);
        }
        line.box = box.rect();
        result.lines.push_back(line);
    }

    reportMissing(missing, result, layerId);
    return out;
}

TitleTextLayout::TitleTextLayout(const AssetResolver* assets) : impl(std::make_unique<Impl>()) {
    impl->assets = assets;
}

TitleTextLayout::~TitleTextLayout() = default;

const LaidOutText& TitleTextLayout::layout(const Json& layer, const std::vector<FieldOverride>& overrides) {
    const std::string layerId = str(layer, "id");
    Json copy;
    const Json* source = &layer;
    for (const FieldOverride& o : overrides) {
        if (o.layerId != layerId) continue;
        if (source == &layer) {
            copy = layer;
            source = &copy;
        }
        setTitleArtField(copy, "layer", o.path, o.value);
    }
    // the inputs: the layout members and the content revisions of the fonts they name
    std::string key = layoutKey(*source);
    if (impl->assets) {
        const auto revision = [&](const Json& ref) {
            const TitleAssetRef r = decodeAssetRef(ref);
            if (r.empty()) return;
            const ResolvedTitleAsset a = impl->assets->resolve(r, TitleAssetKind::Font);
            key += "|" + a.path + "#" + std::to_string(a.revision);
        };
        if (const auto f = source->find("font"); f != source->end()) revision(*f);
        if (const auto f = source->find("fallback_fonts"); f != source->end() && f->is_array()) {
            for (const Json& ref : *f) revision(ref);
        }
    }
    const uint64_t hash = fnv1a64(key);
    auto& layouts = impl->layouts;
    for (auto it = layouts.begin(); it != layouts.end(); ++it) {
        if (it->first == hash) {
            layouts.splice(layouts.begin(), layouts, it);
            return *layouts.front().second;
        }
    }
    layouts.emplace_front(hash, impl->build(*source, hash));
    while (layouts.size() > LAYOUT_CACHE_ENTRIES) layouts.pop_back();
    return *layouts.front().second;
}

hb_font_t* TitleTextLayout::subFont(const FontFace& face, const Json& variations) {
    // only faces this object holds (those of its layouts): a sub-font is cached by its face, which
    // must stay alive as long as the entry
    for (const std::shared_ptr<FontFace>& f : impl->heldFaces) {
        if (f.get() == &face) return impl->subFont(f, variations, nullptr);
    }
    return nullptr;
}

void TitleTextLayout::clear() {
    impl->layouts.clear();
    impl->outlines.clear();
    impl->coverage.clear();
    impl->releaseFonts();
}
