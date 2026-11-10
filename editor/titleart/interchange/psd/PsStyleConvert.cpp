// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/io/psStyle.ts by SafEight, used with the author's permission. The
// effect descriptors are read with our own PsdDescriptor (written from the Adobe document) instead
// of ag-psd's parseEffects; the improvements of S87 §6.9 are marked where they differ.

#include "PsStyleConvert.h"

#include "PsColor.h"
#include "PsdEngineData.h"
#include "PsdReader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <regex>
#include <set>

using namespace jasperengine::editor;
using namespace jasperengine::editor::psd;

namespace {

    constexpr double DEFAULT_PX = 100;

    double round2(double v) {
        return std::round(v * 100) / 100;
    }
    double clampd(double v, double lo, double hi) {
        return std::min(hi, std::max(lo, v));
    }
    double wrap360(double v) {
        return std::fmod(std::fmod(v, 360) + 360, 360);
    }

    void unique(std::vector<std::string>& list) {
        std::set<std::string> seen;
        std::vector<std::string> out;
        for (std::string& s : list) {
            if (seen.insert(s).second) out.push_back(std::move(s));
        }
        list = std::move(out);
    }

    bool enabled(const Json& e) {
        return descBool(e, "enab", true);
    }

    // the instances of one effect: the multi-effect list when present (lmfx), else the single one
    std::vector<const Json*> instances(const Json& fx, const char* single, const char* multi) {
        std::vector<const Json*> out;
        if (const Json* list = multi ? descList(fx, multi) : nullptr) {
            for (const Json& e : *list) {
                if (e.is_object() && enabled(e)) out.push_back(&e);
            }
            return out;
        }
        if (const Json* e = descObject(fx, single); e && enabled(*e)) out.push_back(e);
        return out;
    }

    // a value in px (layer styles store px; points and physical units occur in hand-made files)
    double toPx(const Json& d, const char* key, double ppi, double fallback) {
        if (!d.contains(key)) return fallback;
        const double v = descNumber(d, key, fallback);
        const std::string unit = descUnit(d, key);
        if (unit == "#Pnt") return v * ppi / 72;
        if (unit == "#Mlm") return v * ppi / 25.4;
        if (unit == "#Cntm") return v * ppi / 2.54;
        if (unit == "#Inch") return v * ppi;
        if (unit == "#Pica") return v * ppi / 6;
        return v;
    }

    double percent(const Json& d, const char* key, double fallback) {
        return d.contains(key) ? descNumber(d, key, fallback * 100) / 100 : fallback;
    }

    std::string colorOf(const Json& d, const char* key, const std::string& fallback) {
        const Json* c = descObject(d, key);
        return c ? psColorHex(*c, fallback) : fallback;
    }

    // ---- contours ----

    Json curveOf(std::vector<std::pair<double, double>> pts) {
        std::sort(pts.begin(), pts.end());
        if (pts.empty() || pts.front().first > 0) pts.insert(pts.begin(), {0, pts.empty() ? 0 : pts.front().second});
        if (pts.back().first < 1) pts.push_back({1, pts.back().second});
        Json points = Json::array();
        for (const auto& p : pts) points.push_back(Json{{"x", p.first}, {"y", p.second}, {"corner", false}});
        return Json{{"points", points}};
    }

    std::vector<std::pair<double, double>> contourPoints(const Json* contour) {
        std::vector<std::pair<double, double>> pts;
        if (!contour) return pts;
        if (const Json* list = descList(*contour, "Crv ")) {
            for (const Json& p : *list) {
                if (!p.is_object()) continue;
                pts.push_back({clampd(descNumber(p, "Hrzn", 0) / 255, 0, 1), clampd(descNumber(p, "Vrtc", 0) / 255, 0, 1)});
            }
        }
        return pts;
    }

    bool linearContour(const Json* contour) {
        if (!contour) return true;
        for (const auto& p : contourPoints(contour)) {
            if (std::abs(p.first - p.second) > 2.0 / 255) return false;
        }
        return true;
    }

    struct GlossName {
        const char* pattern;
        const char* gloss;
    };
    // Photoshop's preset gloss contours (English and Japanese names) with a named equivalent
    const GlossName GLOSS_NAMES[] = {
        {"^(linear|線形)$", "linear"},
        {"^(ring|リング)$", "ring"},
        {"^(ring - double|リング - ダブル)$", "double_ring"},
        {"^(cove - (deep|shallow)|くぼみ)", "cove"},
        {"^(rounded steps|ステップ)", "rounded_steps"},
        {"^(sawtooth|のこぎり)", "sawtooth"},
    };

    std::string trim(std::string s) {
        while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
        size_t a = 0;
        while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
        return s.substr(a);
    }

    void glossOf(const Json* contour, Json& bevel) {
        const std::string name = contour ? trim(descText(*contour, "Nm  ")) : std::string();
        for (const GlossName& g : GLOSS_NAMES) {
            if (!name.empty() && std::regex_search(name, std::regex(g.pattern, std::regex::icase))) {
                bevel["gloss"] = g.gloss;
                bevel["gloss_curve"] = curveOf(contourPoints(contour));
                return;
            }
        }
        if (linearContour(contour)) {
            bevel["gloss"] = "linear";
            bevel["gloss_curve"] = curveOf({{0, 0}, {1, 1}});
            return;
        }
        bevel["gloss"] = "custom";
        bevel["gloss_curve"] = curveOf(contourPoints(contour));
    }

    // ---- patterns ----

    struct PatternName {
        const char* pattern;
        const char* kind;
    };
    const PatternName PATTERN_NAMES[] = {
        {"zebra|ゼブラ|シマウマ", "zebra"},     {"tiger|タイガー|虎|トラ", "tiger"},   {"giraffe|キリン", "giraffe"},
        {"leopard|豹|ヒョウ|レオパード", "leopard"}, {"check|市松|チェック", "checker"}, {"hazard|danger|caution|警告", "hazard"},
        {"stripe|ストライプ|縞", "stripes"},     {"dot|polka|ドット|水玉", "dots"},      {"sakura|cherry|桜", "sakura"},
        {"seigaiha|青海波", "seigaiha"},         {"asanoha|麻の葉", "asanoha"},           {"shippo|七宝", "shippo"},
        {"kikko|亀甲", "kikko"},                 {"uroko|鱗", "uroko"},
    };

    std::string patternKind(const std::string& name) {
        if (name.empty()) return "";
        for (const PatternName& p : PATTERN_NAMES) {
            if (std::regex_search(name, std::regex(p.pattern, std::regex::icase))) return p.kind;
        }
        return "";
    }

    const PsPattern* findPattern(const std::vector<PsPattern>& patterns, const Json* ref) {
        if (!ref) return nullptr;
        const std::string id = descText(*ref, "Idnt"), name = descText(*ref, "Nm  ");
        for (const PsPattern& p : patterns) if (!id.empty() && p.id == id) return &p;
        for (const PsPattern& p : patterns) if (!name.empty() && p.name == name) return &p;
        return nullptr;
    }

    // ---- gradients ----

    struct Stop {
        double pos;
        std::string color;
        double alpha = 1;
        double mid = 0.5;
    };

    double alphaAt(const std::vector<std::pair<double, double>>& opacity, double pos) {
        if (opacity.empty()) return 1;
        if (pos <= opacity.front().first) return opacity.front().second;
        for (size_t i = 0; i + 1 < opacity.size(); i++) {
            if (pos <= opacity[i + 1].first) {
                const double span = opacity[i + 1].first - opacity[i].first;
                const double t = span > 0 ? (pos - opacity[i].first) / span : 0;
                return opacity[i].second + (opacity[i + 1].second - opacity[i].second) * t;
            }
        }
        return opacity.back().second;
    }

    std::string colorAt(const std::vector<Stop>& stops, double pos) {
        if (pos <= stops.front().pos) return stops.front().color;
        for (size_t i = 0; i + 1 < stops.size(); i++) {
            if (pos <= stops[i + 1].pos) {
                const double span = stops[i + 1].pos - stops[i].pos;
                return mixHex(stops[i].color, stops[i + 1].color, span > 0 ? (pos - stops[i].pos) / span : 0);
            }
        }
        return stops.back().color;
    }

    // Photoshop gradient (colour stops; each stop's midpoint shapes the segment before it)
    Json gradientOf(const Json& g, bool literal, std::vector<std::string>& notes) {
        std::vector<Stop> src;
        if (const Json* colors = descList(g, "Clrs")) {
            for (const Json& c : *colors) {
                if (!c.is_object()) continue;
                Stop s;
                s.pos = clampd(descNumber(c, "Lctn", 0) / 4096, 0, 1);
                s.color = colorOf(c, "Clr ", "#000000");
                s.mid = clampd(descNumber(c, "Mdpn", 50) / 100, 0.05, 0.95);
                src.push_back(s);
            }
        }
        std::stable_sort(src.begin(), src.end(), [](const Stop& a, const Stop& b) { return a.pos < b.pos; });
        std::vector<std::pair<double, double>> opacity;
        if (const Json* list = descList(g, "Trns")) {
            for (const Json& t : *list) {
                if (t.is_object()) opacity.push_back({clampd(descNumber(t, "Lctn", 0) / 4096, 0, 1), clampd(percent(t, "Opct", 1), 0, 1)});
            }
        }
        std::sort(opacity.begin(), opacity.end());
        std::vector<Stop> stops;
        if (src.empty()) src.push_back({0, "#ffffff"});
        if (literal) {
            for (size_t i = 0; i < src.size(); i++) {
                if (i > 0 && std::abs(src[i].mid - 0.5) > 0.02 && src[i].pos > src[i - 1].pos) {
                    stops.push_back({src[i - 1].pos + src[i].mid * (src[i].pos - src[i - 1].pos), mixHex(src[i - 1].color, src[i].color, 0.5)});
                }
                stops.push_back({src[i].pos, src[i].color});
            }
            bool transparent = false;
            for (const auto& o : opacity) transparent = transparent || o.second < 0.99;
            if (transparent) notes.push_back("gradient “" + descText(g, "Nm  ") + "”: transparency stops ignored");
        } else {
            // S87: a stop's `mid` is the midpoint to the next stop (Photoshop keeps it on the next)
            for (size_t i = 0; i < src.size(); i++) {
                Stop s = src[i];
                s.mid = i + 1 < src.size() ? src[i + 1].mid : 0.5;
                s.alpha = alphaAt(opacity, s.pos);
                stops.push_back(s);
            }
            // opacity stops between colour stops become stops of their own (the colour there)
            for (const auto& o : opacity) {
                bool near = false;
                for (const Stop& s : stops) near = near || std::abs(s.pos - o.first) < 0.002;
                if (!near && stops.size() < 32) stops.push_back({o.first, colorAt(src, o.first), o.second, 0.5});
            }
            std::stable_sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.pos < b.pos; });
        }
        if (stops.size() == 1) stops.push_back({1, stops[0].color, stops[0].alpha, 0.5});
        if (stops.size() > 32) stops.resize(32);
        Json list = Json::array();
        for (const Stop& s : stops) list.push_back(Json{{"pos", s.pos}, {"color", s.color}, {"alpha", s.alpha}, {"mid", s.mid}});
        return Json{{"stops", list}, {"interp", "rgb"}};
    }

    const char* fillKindOf(const std::string& type) {
        if (type == "Rdl ") return "radial";
        if (type == "Angl") return "angle";
        if (type == "Rflc") return "reflected";
        if (type == "Dmnd") return "diamond";
        if (type == "shapeburst" || type == "shapeBurst") return "shape_burst";
        return "linear";
    }

    // a Fill for a gradient descriptor holder (GrFl, a stroke with PntT GrFl, a fill layer's GdFl)
    bool gradientFill(const Json& holder, bool literal, std::vector<std::string>& notes, Json& fill) {
        const Json* g = descObject(holder, "Grad");
        if (!g) return false;
        if (descEnum(*g, "GrdF") == "ClNs") {
            notes.push_back("noise gradient “" + descText(*g, "Nm  ") + "” is not supported");
            return false;
        }
        Json gradient = gradientOf(*g, literal, notes);
        if (descBool(holder, "Rvrs", false)) {
            Json reversed = Json::array();
            const Json& stops = gradient["stops"];
            for (size_t i = stops.size(); i-- > 0;) {
                Json s = stops[i];
                s["pos"] = 1 - s["pos"].get<double>();
                // the midpoint moves to the segment's other side
                s["mid"] = i > 0 ? 1 - stops[i - 1]["mid"].get<double>() : 0.5;
                reversed.push_back(s);
            }
            gradient["stops"] = reversed;
        }
        const double psAngle = descNumber(holder, "Angl", 90);
        const double a = psAngle * 3.14159265358979323846 / 180;
        double along = 0;
        if (const Json* offset = descObject(holder, "Ofst")) {
            along = descNumber(*offset, "Hrzn", 0) / 100 * std::cos(a) - descNumber(*offset, "Vrtc", 0) / 100 * std::sin(a);
        }
        fill = Json{{"kind", fillKindOf(descEnum(holder, "Type"))}, {"color", gradient["stops"][0]["color"]}, {"gradient", gradient},
                    {"angle", wrap360(360 - psAngle)}, {"scale", clampd(percent(holder, "Scl ", 1), 0.05, 4)}, {"offset", clampd(along, -1, 1)},
                    {"span", "layer"}};
        return true;
    }

    Json solidFill(const std::string& color) {
        return Json{{"kind", "solid"}, {"color", color}};
    }

    // `over` painted on `under` at `opacity` (normal blend): a solid over a gradient mixes every stop
    Json overFill(const Json& under, const Json& over, double opacity) {
        if (opacity >= 0.99) return over;
        const bool underSolid = under.value("kind", std::string("solid")) == "solid";
        if (over.value("kind", std::string("solid")) == "solid") {
            const std::string color = over.value("color", std::string("#000000"));
            if (underSolid) return solidFill(mixHex(under.value("color", std::string("#ffffff")), color, opacity));
            Json out = under;
            for (Json& s : out["gradient"]["stops"]) s["color"] = mixHex(s["color"].get<std::string>(), color, opacity);
            out["color"] = out["gradient"]["stops"][0]["color"];
            return out;
        }
        const std::string base = underSolid ? under.value("color", std::string("#ffffff")) : under["gradient"]["stops"][0]["color"].get<std::string>();
        Json out = over;
        for (Json& s : out["gradient"]["stops"]) s["color"] = mixHex(base, s["color"].get<std::string>(), opacity);
        out["color"] = out["gradient"]["stops"][0]["color"];
        return out;
    }

    std::string satinBlend(const std::string& mode) {
        if (mode == "Nrml") return "normal";
        if (mode == "Mltp" || mode == "Drkn" || mode == "CBrn" || mode == "linearBurn") return "multiply";
        if (mode == "Scrn" || mode == "Lghn" || mode == "CDdg" || mode == "linearDodge") return "screen";
        if (mode == "Ovrl" || mode == "SftL" || mode == "HrdL") return "overlay";
        return "normal";
    }

    bool lightBlend(const std::string& mode) {
        return mode == "Scrn" || mode == "linearDodge" || mode == "CDdg" || mode == "Lghn" || mode == "lighterColor";
    }

    std::string modeName(const std::string& mode) {
        const std::string blend = titleBlendOfPsEnum(mode);
        return blend.empty() ? mode : blend;
    }

    void blendNote(const char* what, const Json& e, std::vector<std::string>& notes) {
        const std::string mode = descEnum(e, "Md  ");
        if (!mode.empty() && mode != "Nrml") notes.push_back(std::string(what) + ": blend mode “" + modeName(mode) + "” approximated as normal");
    }

    Json effect(const char* type, Json params) {
        return Json{{"type", type}, {"enabled", true}, {"params", std::move(params)}};
    }

    struct Light {
        double angle, altitude;
    };

}

// ---- blend tables ----

std::string psd::titleBlendOfPsKey(const std::string& key) {
    static const std::pair<const char*, const char*> table[] = {
        {"norm", "normal"}, {"lddg", "add"}, {"scrn", "screen"}, {"mul ", "multiply"}, {"over", "overlay"}, {"sLit", "soft_light"},
        {"hLit", "hard_light"}, {"dark", "darken"}, {"lite", "lighten"}, {"div ", "color_dodge"}, {"idiv", "color_burn"},
        {"lbrn", "linear_burn"}, {"diff", "difference"}, {"smud", "exclusion"}, {"hue ", "hue"}, {"sat ", "saturation"},
        {"colr", "color"}, {"lum ", "luminosity"}};
    for (const auto& e : table) if (key == e.first) return e.second;
    return "";
}

std::string psd::titleBlendOfPsEnum(const std::string& value) {
    static const std::pair<const char*, const char*> table[] = {
        {"Nrml", "normal"}, {"linearDodge", "add"}, {"Scrn", "screen"}, {"Mltp", "multiply"}, {"Ovrl", "overlay"}, {"SftL", "soft_light"},
        {"HrdL", "hard_light"}, {"Drkn", "darken"}, {"Lghn", "lighten"}, {"CDdg", "color_dodge"}, {"CBrn", "color_burn"},
        {"linearBurn", "linear_burn"}, {"Dfrn", "difference"}, {"Xclu", "exclusion"}, {"H   ", "hue"}, {"Strt", "saturation"},
        {"Clr ", "color"}, {"Lmns", "luminosity"}};
    for (const auto& e : table) if (value == e.first) return e.second;
    return "";
}

std::string psd::psKeyOfTitleBlend(const std::string& blend) {
    static const std::pair<const char*, const char*> table[] = {
        {"normal", "norm"}, {"add", "lddg"}, {"screen", "scrn"}, {"multiply", "mul "}, {"overlay", "over"}, {"soft_light", "sLit"},
        {"hard_light", "hLit"}, {"darken", "dark"}, {"lighten", "lite"}, {"color_dodge", "div "}, {"color_burn", "idiv"},
        {"linear_burn", "lbrn"}, {"difference", "diff"}, {"exclusion", "smud"}, {"hue", "hue "}, {"saturation", "sat "},
        {"color", "colr"}, {"luminosity", "lum "}};
    for (const auto& e : table) if (blend == e.first) return e.second;
    return "";
}

// ---- reading ----

bool psd::psEffectsEnabled(const Json& fx) {
    if (!fx.is_object() || !descBool(fx, "masterFXSwitch", true)) return false;
    for (auto it = fx.begin(); it != fx.end(); ++it) {
        if (it->is_object() && it->contains("enab") && descBool(*it, "enab", false)) return true;
        if (it->is_array()) {
            for (const Json& e : *it) if (e.is_object() && descBool(e, "enab", false)) return true;
        }
    }
    return false;
}

double psd::psReferenceSize(const PsStyleSource& source) {
    return source.fontPx > 0 ? source.fontPx : source.suggestedPx;
}

namespace {

    void layerSource(const PsdFile& file, const PsdLayerRecord& l, PsStyleSource& s) {
        s.name = trim(l.name).empty() ? std::string("PS style") : trim(l.name);
        s.effects = l.effects;
        s.fillOpacity = l.fillOpacity / 255.0;
        s.knockout = l.knockout;
        s.ppi = file.dpi;
        s.globalAngle = file.globalAngle;
        s.globalAltitude = file.globalAltitude;
        const int height = l.bounds.height();
        s.suggestedPx = height > 0 ? height : DEFAULT_PX;
        s.origin = "layer";
        s.sizeNote = height > 0 ? "layer height" : "unknown size";
        if (!l.text) return;
        PsTextStyle text;
        Json engine;
        std::string error;
        const std::vector<uint8_t> raw = descRaw(l.textDescriptor, "EngineData");
        if (raw.empty() || !parseEngineData(raw, engine, error) || !firstTextStyle(engine, text)) return;
        if (text.hasColor) s.baseColor = rgbHex(text.color[0], text.color[1], text.color[2]);
        if (!(text.fontSize > 0)) return;
        // type size is in points; free-transforming a text layer scales its matrix, not the size
        const double k = std::hypot(l.transform[2], l.transform[3]) > 0 ? std::hypot(l.transform[2], l.transform[3]) : 1;
        s.fontPx = std::round(text.fontSize * k * (file.dpi / 72) * 10) / 10;
        s.suggestedPx = s.fontPx;
        s.origin = "text layer";
        char note[96];
        const bool scaled = std::abs(k - 1) > 0.005;
        if (std::abs(file.dpi - 72) > 1e-6) {
            std::snprintf(note, sizeof(note), scaled ? "font %g pt x %g at %g ppi" : "font %g pt at %g ppi", round2(text.fontSize),
                          scaled ? round2(k) : round2(file.dpi), round2(file.dpi));
        } else {
            std::snprintf(note, sizeof(note), scaled ? "font %g pt x %g" : "font %g pt", round2(text.fontSize), round2(k));
        }
        s.sizeNote = note;
    }

    void walk(const PsdFile& file, const std::vector<PsdNode>& nodes, PsStyleFile& out) {
        // nodes are bottom first; the Layers panel (and this list) goes top to bottom
        for (size_t i = nodes.size(); i-- > 0;) {
            const PsdLayerRecord& l = file.layers[(size_t)nodes[i].layer];
            if (psEffectsEnabled(l.effects)) {
                PsStyleSource s;
                layerSource(file, l, s);
                out.styles.push_back(std::move(s));
            }
            walk(file, nodes[i].children, out);
        }
    }

}

bool psd::readPsStyles(const uint8_t* data, size_t size, PsStyleFile& out, std::string& error) {
    out = PsStyleFile();
    if (size >= 4 && std::string(reinterpret_cast<const char*>(data), 4) == "8BPS") {
        PsdFile file;
        PsdReadOptions options;
        options.layerPixels = false;
        options.mergedImage = false;
        if (!readPsd(data, size, file, error, options)) return false;
        out.kind = "psd";
        walk(file, psdTree(file), out);
        readPatterns(file.patterns.data(), file.patterns.size(), out.patterns, &out.skipped);
        if (out.styles.empty()) {
            error = "no layer in this PSD has layer effects";
            return false;
        }
        return true;
    }
    if (size >= 6 && std::string(reinterpret_cast<const char*>(data) + 2, 4) == "8BSL") {
        AslFile asl;
        if (!readAsl(data, size, asl, error)) return false;
        out.kind = "asl";
        out.patterns = std::move(asl.patterns);
        out.skipped = std::move(asl.skipped);
        for (AslStyle& a : asl.styles) {
            if (!psEffectsEnabled(a.effects)) {
                out.skipped.push_back(a.name + ": no layer effects (blending options only)");
                continue;
            }
            PsStyleSource s;
            s.name = a.name;
            s.origin = "style library";
            s.effects = std::move(a.effects);
            s.suggestedPx = DEFAULT_PX;
            s.sizeNote = "unknown size";
            out.styles.push_back(std::move(s));
        }
        if (out.styles.empty()) {
            error = out.skipped.empty() ? "the style library is empty" : "no usable styles (" + out.skipped.front() + ")";
            return false;
        }
        return true;
    }
    error = "not a Photoshop file: choose a .psd or an .asl style library";
    return false;
}

// ---- conversion ----

bool psd::convertPsStyle(const PsStyleSource& src, double sizePx, const std::vector<PsPattern>& patterns, const PsConvertOptions& options,
                         ConvertedPsStyle& out, std::string& error) {
    if (!(sizePx > 0)) {
        error = "the reference size must be above 0 px";
        return false;
    }
    out = ConvertedPsStyle();
    out.refSize = sizePx;
    const Json& e = src.effects;
    std::vector<std::string>& notes = out.notes;
    std::vector<std::string>& summary = out.summary;
    const bool literal = options.pachislopLiteral;
    const double k = percent(e, "Scl ", 1);     // Scale Effects
    const auto px = [&](const Json& d, const char* key, double fallback) { return toPx(d, key, src.ppi, fallback) * k; };
    const auto pct = [&](const Json& d, const char* key, double fallback) { return round2(px(d, key, fallback) / sizePx * 100); };
    const auto light = [&](const Json& d) {
        const bool global = descBool(d, "uglg", false);
        return Light{global ? src.globalAngle : descNumber(d, "lagl", 120), global ? src.globalAltitude : descNumber(d, "Lald", 30)};
    };

    // the face fill: the base colour, then (bottom to top) gradient overlays and colour overlays
    Json fill = solidFill(src.baseColor);
    {
        std::vector<const Json*> gradients = instances(e, "GrFl", "gradientFillMulti");
        for (size_t i = gradients.size(); i-- > 0;) {
            Json f;
            if (!gradientFill(*gradients[i], literal, notes, f)) continue;
            blendNote("gradient overlay", *gradients[i], notes);
            fill = overFill(fill, f, percent(*gradients[i], "Opct", 1));
            summary.push_back("gradient face");
        }
        std::vector<const Json*> colors = instances(e, "SoFi", "solidFillMulti");
        for (size_t i = colors.size(); i-- > 0;) {
            blendNote("colour overlay", *colors[i], notes);
            fill = overFill(fill, solidFill(colorOf(*colors[i], "Clr ", "#000000")), percent(*colors[i], "Opct", 1));
            summary.push_back("colour face");
        }
    }
    Json face = Json{{"fill", fill}, {"opacity", src.fillOpacity}};
    if (src.fillOpacity < 0.99) summary.push_back("fill opacity " + std::to_string((int)std::lround(src.fillOpacity * 100)) + " %");

    if (const Json* po = descObject(e, "patternFill"); po && enabled(*po)) {
        const Json* ref = descObject(*po, "Ptrn");
        const std::string name = ref ? descText(*ref, "Nm  ") : std::string();
        const std::string kind = patternKind(name);
        const double scale = percent(*po, "Scl ", 1);
        if (!kind.empty()) {
            face["pattern"] = Json{{"kind", kind}, {"opacity", percent(*po, "Opct", 1)}, {"scale", round2(12 * scale)}, {"angle", -descNumber(*po, "Angl", 0)}};
            summary.push_back(kind + " pattern");
        } else if (!literal && findPattern(patterns, ref) && options.savePattern) {
            // S87: the bitmap itself becomes a custom pattern
            const std::string path = options.savePattern(*findPattern(patterns, ref));
            if (!path.empty()) {
                face["pattern"] = Json{{"kind", "custom"}, {"image", Json{{"path", path}, {"guid", ""}}}, {"opacity", percent(*po, "Opct", 1)},
                                       {"scale", round2(12 * scale)}, {"angle", -descNumber(*po, "Angl", 0)}};
                summary.push_back("custom pattern");
            }
        } else {
            notes.push_back("pattern overlay “" + (name.empty() ? std::string("?") : name) + "”: bitmap patterns have no equivalent; skipped");
        }
    }

    if (const Json* sa = descObject(e, "ChFX"); sa && enabled(*sa)) {
        Json satin = Json{{"enabled", true}, {"color", colorOf(*sa, "Clr ", "#000000")}, {"blend", satinBlend(descEnum(*sa, "Md  "))},
                          {"opacity", percent(*sa, "Opct", 0.5)}, {"angle", descNumber(*sa, "lagl", 19)}, {"distance", pct(*sa, "Dstn", 11)},
                          {"size", pct(*sa, "blur", 14)}, {"invert", descBool(*sa, "Invr", true)}};
        if (const Json* contour = descObject(*sa, "MpgS")) satin["contour"] = curveOf(contourPoints(contour));
        face["satin"] = satin;
        summary.push_back("satin");
    }

    // strokes -> rims: top-most first, a stroke reaching no further than one above it is hidden
    struct Shown {
        const Json* stroke;
        double reach;
    };
    std::vector<Shown> shown;
    for (const Json* s : instances(e, "FrFX", "frameFXMulti")) {
        const double size = px(*s, "Sz  ", 3);
        const double reach = descEnum(*s, "Styl") == "CtrF" ? size / 2 : size;
        bool hidden = false;
        for (const Shown& o : shown) hidden = hidden || o.reach >= reach;
        if (reach > 0 && !hidden) shown.push_back({s, reach});
    }
    std::stable_sort(shown.begin(), shown.end(), [](const Shown& a, const Shown& b) { return a.reach < b.reach; });
    Json rims = Json::array();
    double inner = 0;
    for (const Shown& sh : shown) {
        const Json& s = *sh.stroke;
        if (descEnum(s, "Styl") == "InsF") notes.push_back("inside stroke drawn outside the face (rims grow outward)");
        const double opacity = percent(s, "Opct", 1);
        if (opacity < 0.99 && literal) notes.push_back("stroke opacity " + std::to_string((int)std::lround(opacity * 100)) + " % drawn opaque");
        blendNote("stroke", s, notes);
        const std::string type = descEnum(s, "PntT");
        Json rimFill = solidFill(colorOf(s, "Clr ", "#000000"));
        if (type == "GrFl") {
            Json g;
            if (gradientFill(s, literal, notes, g)) rimFill = g;
        }
        Json material = Json{{"fill", rimFill}};
        if (!literal) material["opacity"] = opacity;
        if (type == "Ptrn") {
            const Json* ref = descObject(s, "Ptrn");
            const std::string name = ref ? descText(*ref, "Nm  ") : std::string();
            const std::string kind = patternKind(name);
            if (!kind.empty()) {
                material["pattern"] = Json{{"kind", kind}, {"scale", round2(12 * percent(s, "Scl ", 1))}};
            } else if (!literal && findPattern(patterns, ref) && options.savePattern) {
                const std::string path = options.savePattern(*findPattern(patterns, ref));
                if (!path.empty()) material["pattern"] = Json{{"kind", "custom"}, {"image", Json{{"path", path}, {"guid", ""}}}, {"scale", round2(12 * percent(s, "Scl ", 1))}};
            } else {
                notes.push_back("stroke pattern “" + (name.empty() ? std::string("?") : name) + "”: bitmap patterns have no equivalent; drawn " +
                                rimFill.value("color", std::string("#000000")));
            }
        }
        rims.push_back(Json{{"width", round2((sh.reach - inner) / sizePx * 100)}, {"join", "round"}, {"fill_counters", false}, {"material", material}});
        inner = sh.reach;
    }
    if (!rims.empty()) summary.insert(summary.begin(), rims.size() == 1 ? std::string("1 rim") : std::to_string(rims.size()) + " rims");
    if (src.knockout) summary.push_back("knockout");

    if (const Json* bv = descObject(e, "ebbl"); bv && enabled(*bv)) {
        const bool down = descEnum(*bv, "bvlD") == "Out ";
        const Light l = light(*bv);
        const std::string technique = descEnum(*bv, "bvlT");
        const Json* shape = descBool(*bv, "useShape", false) ? descObject(*bv, "MpgS") : nullptr;
        if (shape && linearContour(shape)) shape = nullptr;
        if (technique == "Slmt") notes.push_back("bevel: Chisel Soft drawn as a chamfer");
        Json bevel = Json{{"enabled", true}, {"profile", shape ? "custom" : (technique == "PrBL" || technique == "Slmt") ? "chamfer" : "smooth"},
                          {"size", std::max(0.1, pct(*bv, "blur", 5))}, {"depth", clampd(percent(*bv, "srgR", 1), 0, 10)},
                          {"soften", pct(*bv, "Sftn", 0)}, {"light_altitude", clampd(l.altitude, 0, 90)},
                          {"highlight_color", colorOf(*bv, "hglC", "#ffffff")}, {"highlight_opacity", percent(*bv, "hglO", 0.75)},
                          {"shadow_color", colorOf(*bv, "sdwC", "#000000")}, {"shadow_opacity", percent(*bv, "sdwO", 0.75)}};
        if (shape) bevel["profile_curve"] = curveOf(contourPoints(shape));
        if (literal) {
            // Pachislop: Down is the light from the opposite side (which also moves the highlight colour)
            bevel["light_angle"] = std::fmod(l.angle + (down ? 180 : 0), 360);
            bevel["use_global_light"] = descBool(*bv, "uglg", false) && !down;
        } else {
            bevel["light_angle"] = l.angle;
            bevel["direction"] = down ? "down" : "up";
            bevel["use_global_light"] = descBool(*bv, "uglg", false);
        }
        glossOf(descObject(*bv, "TrnS"), bevel);
        const std::string style = descEnum(*bv, "bvlS");
        if (style == "strokeEmboss") {
            if (!rims.empty()) {
                for (Json& r : rims) r["material"]["bevel"] = bevel;
            } else {
                notes.push_back("stroke emboss without a stroke has no effect");
            }
            summary.push_back("rim bevel");
        } else if (style == "OtrB" && !rims.empty()) {
            rims.back()["material"]["bevel"] = bevel;
            summary.push_back("bevel");
        } else if (!literal && (style == "Embs" || style == "PlEb")) {
            // S87 / S83: emboss raises outside the outline too, which a face bevel cannot: a
            // material effect over the layer's own colours
            Json m = bevel;
            if (style == "PlEb") {
                m["direction"] = "down";
                notes.push_back("pillow emboss drawn as a material emboss with the bevel down (approximated)");
            }
            out.effects.push_back(effect("material", Json{{"color_mode", "layer"}, {"size", round2(sizePx)},
                                                          {"material", Json{{"fill", solidFill("#ffffff")}, {"bevel", m}}}}));
            summary.push_back(style == "PlEb" ? "pillow emboss" : "emboss");
        } else {
            if (style == "OtrB") notes.push_back("outer bevel without a stroke applied to the face");
            face["bevel"] = bevel;
            summary.push_back("bevel");
        }
    }

    if (const Json* ig = descObject(e, "IrGl"); ig && enabled(*ig)) {
        std::string color = colorOf(*ig, "Clr ", "");
        if (color.empty()) {
            const Json* g = descObject(*ig, "Grad");
            const Json* colors = g ? descList(*g, "Clrs") : nullptr;
            color = colors && !colors->empty() ? colorOf((*colors)[0], "Clr ", "#ffffbe") : "#ffffbe";
        }
        const bool center = descEnum(*ig, "glwS") == "SrcC";
        if (literal) {
            face["inner_glow"] = Json{{"enabled", true}, {"color", color}, {"size", pct(*ig, "blur", 5)}, {"opacity", percent(*ig, "Opct", 0.75)},
                                      {"noise", percent(*ig, "Nose", 0)}};
            if (center) notes.push_back("inner glow: “Center” source drawn from the edge");
        } else {
            const std::string mode = descEnum(*ig, "Md  ");
            const std::string glowMode = mode == "Nrml" ? "normal" : (mode == "linearDodge" || mode == "CDdg") ? "add" : "screen";
            if (!mode.empty() && mode != "Nrml" && mode != "Scrn" && mode != "linearDodge" && mode != "CDdg") {
                notes.push_back("inner glow: blend mode “" + modeName(mode) + "” drawn as screen");
            }
            out.effects.push_back(effect("inner_glow", Json{{"fill", "solid"}, {"color", color}, {"size", round2(std::max(1.0, px(*ig, "blur", 5)))},
                                                            {"choke", clampd(percent(*ig, "Ckmt", 0), 0, 1)}, {"opacity", percent(*ig, "Opct", 0.75)},
                                                            {"mode", glowMode}, {"source", center ? "center" : "edge"}, {"intensity", 1}}));
        }
        summary.push_back("inner glow");
    }

    {
        std::vector<const Json*> list = instances(e, "IrSh", "innerShadowMulti");
        for (size_t i = list.size(); i-- > 0;) {
            const Json& s = *list[i];
            const Light l = light(s);
            out.effects.push_back(effect("inner_shadow", Json{{"angle", wrap360(180 - l.angle)}, {"use_global_light", descBool(s, "uglg", false)},
                                                              {"distance", round2(px(s, "Dstn", 5))}, {"size", round2(px(s, "blur", 5))},
                                                              {"choke", clampd(percent(s, "Ckmt", 0), 0, 0.95)}, {"color", colorOf(s, "Clr ", "#000000")},
                                                              {"opacity", percent(s, "Opct", 0.75)}}));
            blendNote("inner shadow", s, notes);
            summary.push_back("inner shadow");
        }
    }
    {
        std::vector<const Json*> list = instances(e, "DrSh", "dropShadowMulti");
        for (size_t i = list.size(); i-- > 0;) {
            const Json& s = *list[i];
            const Light l = light(s);
            out.effects.push_back(effect("drop_shadow", Json{{"angle", wrap360(180 - l.angle)}, {"use_global_light", descBool(s, "uglg", false)},
                                                             {"distance", round2(px(s, "Dstn", 5))}, {"size", round2(px(s, "blur", 5))},
                                                             {"spread", clampd(percent(s, "Ckmt", 0), 0, 0.95)}, {"color", colorOf(s, "Clr ", "#000000")},
                                                             {"opacity", percent(s, "Opct", 0.75)}}));
            if (!literal) blendNote("drop shadow", s, notes);
            summary.push_back("drop shadow");
        }
    }
    if (const Json* og = descObject(e, "OrGl"); og && enabled(*og)) {
        std::string color = colorOf(*og, "Clr ", "");
        if (color.empty()) {
            const Json* g = descObject(*og, "Grad");
            const Json* colors = g ? descList(*g, "Clrs") : nullptr;
            color = colors && !colors->empty() ? colorOf((*colors)[0], "Clr ", "#ffffbe") : "#ffffbe";
        }
        out.effects.push_back(effect("outer_glow", Json{{"fill", "solid"}, {"color", color}, {"size", round2(std::max(1.0, px(*og, "blur", 5)))},
                                                        {"intensity", 1}, {"spread", clampd(percent(*og, "Ckmt", 0), 0, 0.95)},
                                                        {"opacity", percent(*og, "Opct", 0.75)},
                                                        {"mode", lightBlend(descEnum(*og, "Md  ")) ? "add" : "behind"}}));
        summary.push_back("outer glow");
    }

    out.style = Json{{"face", face}, {"rims", rims}, {"knockout", src.knockout}};
    unique(summary);
    unique(notes);
    return true;
}
