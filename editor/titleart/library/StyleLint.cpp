// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Rules ported from Pachislop src/presets/lint.ts by SafEight, used with the author's permission.

#include "StyleLint.h"

#include "titleart/effects/TitleFx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titlelibrary;

namespace {

    using Rgb = std::array<double, 3>;

    struct Lab {
        double L = 0.0, a = 0.0, b = 0.0;
    };

    constexpr double PI = 3.14159265358979323846;
    constexpr int SAMPLES = 9;

    std::string str(const Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    double num(const Json& object, const char* key, double fallback) {
        auto it = object.find(key);
        return it != object.end() && it->is_number() ? it->get<double>() : fallback;
    }

    bool flag(const Json& object, const char* key, bool fallback) {
        auto it = object.find(key);
        return it != object.end() && it->is_boolean() ? it->get<bool>() : fallback;
    }

    const Json& member(const Json& object, const char* key) {
        static const Json none = Json::object();
        auto it = object.find(key);
        return it != object.end() && it->is_object() ? *it : none;
    }

    const Json& list(const Json& object, const char* key) {
        static const Json none = Json::array();
        auto it = object.find(key);
        return it != object.end() && it->is_array() ? *it : none;
    }

    std::string fixed(double value, int digits) {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
        return buffer;
    }

    double rounded(double value, double step) {
        return std::round(value / step) * step;
    }

    // ---- colour ----

    double toLinear(double c) {
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    }

    double labF(double t) {
        return t > 216.0 / 24389.0 ? std::cbrt(t) : ((24389.0 / 27.0) * t + 16.0) / 116.0;
    }

    // sRGB 0..1 -> CIE L*a*b* (D65)
    Lab toLab(const Rgb& c) {
        const double r = toLinear(c[0]), g = toLinear(c[1]), b = toLinear(c[2]);
        const double fx = labF((0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047);
        const double fy = labF(0.2126 * r + 0.7152 * g + 0.0722 * b);
        const double fz = labF((0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883);
        return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
    }

    double luma601(const Rgb& c) {
        return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
    }

    Rgb colorOf(const Json& value, const Rgb& fallback) {
        titleart::TitleColor c;
        if (!titleart::parseTitleColor(value, c)) return fallback;
        return {c.r, c.g, c.b};
    }

    Rgb sample(const titleart::TitleGradient& gradient, double pos) {
        float out[4];
        titleart::fx::sampleGradient(gradient, pos, out);
        return {out[0], out[1], out[2]};
    }

    std::vector<Rgb> rampSamples(const Json& gradientJson) {
        const titleart::TitleGradient gradient = titleart::decodeGradient(gradientJson);
        std::vector<Rgb> out;
        for (int i = 0; i < SAMPLES; i++) out.push_back(sample(gradient, static_cast<double>(i) / (SAMPLES - 1)));
        return out;
    }

    bool hasStops(const Json& gradient) {
        return gradient.is_object() && gradient.contains("stops") && gradient["stops"].is_array() && !gradient["stops"].empty();
    }

    // a 2D element's colours: its fill (every non-solid kind samples the gradient), pushed through an
    // enabled gradient-map finish by the finish's amount
    std::vector<Rgb> samples2D(const Json& material) {
        const Json& fill = member(material, "fill");
        std::vector<Rgb> colors;
        if (str(fill, "kind") == "solid") colors.push_back(colorOf(fill.value("color", Json("#ffffff")), {1, 1, 1}));
        else colors = rampSamples(member(fill, "gradient"));
        const Json& finish = member(material, "finish");
        if (!flag(finish, "enabled", false)) return colors;
        const double amount = num(finish, "amount", 1.0);
        const titleart::TitleGradient map = titleart::decodeGradient(member(finish, "gradient"));
        for (Rgb& c : colors) {
            const Rgb f = sample(map, luma601(c));
            for (int k = 0; k < 3; k++) c[k] += (f[k] - c[k]) * amount;
        }
        return colors;
    }

    // a 3D material's colours: its gradient when it has one (it replaces `color`), else the colour
    std::vector<Rgb> samples3D(const Json& material) {
        auto gradient = material.find("gradient");
        if (gradient != material.end() && hasStops(*gradient)) return rampSamples(*gradient);
        return {colorOf(material.value("color", Json("#ffffff")), {1, 1, 1})};
    }

    double hueOf(double a, double b) {
        return std::fmod(std::atan2(b, a) * 180.0 / PI + 360.0, 360.0);
    }

    double hueDist(double x, double y) {
        const double d = std::fabs(x - y);
        return std::min(d, 360.0 - d);
    }

    struct Element {
        std::string name;
        double L = 0.0;             // mean L*
        double spread = 0.0;        // L* range of the samples (metal banding)
        bool hasHue = false;        // false: neutral, or a spectrum
        double hue = 0.0;           // mean a*b* hue of the chromatic samples
        bool spectrum = false;
        double hueSpread = 0.0;     // the widest distance between two chromatic samples
    };

    Element element(const std::string& name, const std::vector<Rgb>& colors) {
        Element e;
        e.name = name;
        std::vector<Lab> labs;
        for (const Rgb& c : colors) labs.push_back(toLab(c));
        if (labs.empty()) return e;
        double sum = 0.0, lo = labs[0].L, hi = labs[0].L;
        for (const Lab& l : labs) {
            sum += l.L;
            lo = std::min(lo, l.L);
            hi = std::max(hi, l.L);
        }
        e.L = sum / labs.size();
        e.spread = hi - lo;
        std::vector<Lab> chroma;
        for (const Lab& l : labs) {
            if (std::hypot(l.a, l.b) > LINT_CHROMATIC) chroma.push_back(l);
        }
        std::vector<double> hues;
        for (const Lab& l : chroma) hues.push_back(hueOf(l.a, l.b));
        for (double h : hues) {
            for (double k : hues) e.hueSpread = std::max(e.hueSpread, hueDist(h, k));
        }
        e.spectrum = e.hueSpread > LINT_SPECTRUM_SPREAD;
        if (e.spectrum || chroma.size() * 2 < labs.size()) return e;
        double a = 0.0, b = 0.0;
        for (const Lab& l : chroma) {
            a += l.a;
            b += l.b;
        }
        e.hasHue = true;
        e.hue = hueOf(a, b);
        return e;
    }

    // ---- the layer ----

    bool isText(const Json& layer) {
        return titleart::layerTypeOf(layer) == titleart::LayerType::Text;
    }

    bool is3D(const Json& layer) {
        auto style = layer.find("style_3d");
        return str(layer, "dimension") == "3d" && style != layer.end() && style->is_object();
    }

    // a line break inside the text (not at its ends)
    bool multiLine(const Json& layer) {
        if (!isText(layer)) return false;
        const std::string text = str(layer, "text");
        const char* space = " \t\n\r\v\f";
        const size_t first = text.find_first_not_of(space);
        if (first == std::string::npos) return false;
        const size_t last = text.find_last_not_of(space);
        return text.find('\n', first) < last;
    }

    std::string layerRef(const Json& layer) {
        return "id:" + str(layer, "id");
    }

    struct Ring {
        Element el;
        long insertAt = -1;         // the rim / band index a separator goes before (-1: the face)
    };

    std::vector<Ring> rings(const Json& layer) {
        std::vector<Ring> out;
        if (!isText(layer)) return out;
        if (!is3D(layer)) {
            const Json& style = member(layer, "style");
            out.push_back({element("face", samples2D(member(style, "face"))), -1});
            const Json& rims = list(style, "rims");
            for (size_t i = 0; i < rims.size(); i++) {
                const titleart::TitleVec2 offset = titleart::decodeVec2(rims[i].value("offset", Json()));
                if (!flag(rims[i], "enabled", true) || offset.x != 0.0 || offset.y != 0.0) continue;
                out.push_back({element("rim#" + std::to_string(i), samples2D(member(rims[i], "material"))), static_cast<long>(i)});
            }
            return out;
        }
        // bands right behind the letter in its own material read as the letter itself
        const Json& s = layer["style_3d"];
        const Json& face = member(s, "face");
        auto same = [&](const Json& m) {
            return str(m, "preset") == str(face, "preset") && m.value("color", Json()) == face.value("color", Json()) &&
                   m.value("gradient", Json()) == face.value("gradient", Json());
        };
        const Json& bands = list(s, "bands");
        size_t first = bands.size();
        for (size_t i = 0; i < bands.size(); i++) {
            if (flag(bands[i], "enabled", true) && !same(member(bands[i], "material"))) {
                first = i;
                break;
            }
        }
        out.push_back({element("face", samples3D(face)), -1});
        for (size_t i = first; i < bands.size(); i++) {
            if (!flag(bands[i], "enabled", true)) continue;
            out.push_back({element("band#" + std::to_string(i), samples3D(member(bands[i], "material"))), static_cast<long>(i)});
        }
        return out;
    }

    StyleLintFinding finding(const Json& layer, const char* rule, const std::string& element, double measured, double threshold,
                             const std::string& message) {
        StyleLintFinding f;
        f.rule = rule;
        f.element = element;
        f.measured = measured;
        f.threshold = threshold;
        f.message = message;
        f.fix = nullptr;
        f.layer = str(layer, "id");
        return f;
    }

    Json fixOf(Json ops) {
        return Json{{"ops", std::move(ops)}};
    }

    Json setOp(const Json& layer, Json values) {
        return Json{{"op", "set"}, {"layer", layerRef(layer)}, {"values", std::move(values)}};
    }

    // ---- rules ----

    // a ring whose lightness is opposite to the pair it separates
    const char* separatorColor(double meanL) {
        return meanL > 50.0 ? "#0a0604" : "#ffffff";
    }

    void contrastIssues(const Json& layer, std::vector<StyleLintFinding>& out) {
        const std::vector<Ring> rs = rings(layer);
        for (size_t i = 1; i < rs.size(); i++) {
            const Element& a = rs[i - 1].el;
            const Element& b = rs[i].el;
            // a spectrum spans every lightness (its neighbours are judged by hue); two blacks are one
            // thick keyline; two banded metals separate through their banding
            if (a.spectrum || b.spectrum || (a.L < LINT_DARK_L && b.L < LINT_DARK_L) || (a.spread > LINT_BANDED_L && b.spread > LINT_BANDED_L)) continue;
            const double dL = std::fabs(a.L - b.L);
            if (dL >= LINT_MIN_DELTA_L) continue;
            const char* color = separatorColor((a.L + b.L) / 2.0);
            const bool light = std::string(color) == "#ffffff";
            StyleLintFinding f = finding(layer, "contrast", b.name, dL, LINT_MIN_DELTA_L,
                                         a.name + " and " + b.name + " differ by only " + fixed(dL, 0) + " in lightness (L*): they merge into one band. " +
                                             "Insert a " + (light ? "light" : "dark") + " line between them, or alternate light and dark outward.");
            const long at = rs[i].insertAt;
            if (is3D(layer)) {
                f.fix = fixOf(Json::array({Json{{"op", "add_band"}, {"layer", layerRef(layer)}, {"index", at},
                                                {"values", Json{{"grow", LINT_SEPARATOR_GROW_3D}, {"material.color", color}}}}}));
            } else {
                f.fix = fixOf(Json::array({Json{{"op", "add_rim"}, {"layer", layerRef(layer)}, {"index", at},
                                                {"values", Json{{"width", LINT_SEPARATOR_WIDTH}, {"material.fill.kind", "solid"}, {"material.fill.color", color}}}}}));
            }
            out.push_back(std::move(f));
        }
    }

    void hueIssues(const Json& layer, std::vector<StyleLintFinding>& out) {
        struct Family {
            double hue;
            std::vector<std::string> names;
        };
        std::vector<Family> families;
        for (const Ring& r : rings(layer)) {
            if (!r.el.hasHue) continue;
            auto it = std::find_if(families.begin(), families.end(), [&](const Family& f) { return hueDist(f.hue, r.el.hue) < LINT_HUE_FAMILY; });
            if (it != families.end()) it->names.push_back(r.el.name);
            else families.push_back({r.el.hue, {r.el.name}});
        }
        if (static_cast<int>(families.size()) < LINT_MAX_HUE_FAMILIES) return;
        std::string groups;
        for (const Family& f : families) {
            if (!groups.empty()) groups += " / ";
            for (size_t i = 0; i < f.names.size(); i++) groups += (i ? " + " : "") + f.names[i];
        }
        out.push_back(finding(layer, "hue_count", families[0].names[0], static_cast<double>(families.size()), LINT_MAX_HUE_FAMILIES,
                              std::to_string(families.size()) + " chromatic hue families (" + groups +
                                  "): the eye cannot find the face. Make the rims neutral or a shade of the face's hue."));
    }

    // how far the letter's own 3D bevel reaches past the outline (% of size)
    double letterOutside(const Json& s) {
        if (str(s, "mode") == "inflate") return 0.0;
        const std::string position = str(s, "bevel_position");
        const double share = position == "inside" ? 0.0 : position == "center" ? 0.5 : 1.0;
        return std::max(0.0, num(member(s, "bevel"), "width", 0.0)) * share;
    }

    double rimTotal(const Json& layer) {
        double total = 0.0;
        if (is3D(layer)) {
            const Json& s = layer["style_3d"];
            total = letterOutside(s);
            for (const Json& b : list(s, "bands")) {
                if (flag(b, "enabled", true)) total += std::max(0.0, num(b, "grow", 0.0));
            }
            return total;
        }
        for (const Json& r : list(member(layer, "style"), "rims")) {
            if (flag(r, "enabled", true)) total += num(r, "width", 0.0);
        }
        return total;
    }

    // every enabled rim (2D), or the letter's bevel and every enabled band (3D), scaled by k
    Json scaledRims(const Json& layer, double k) {
        Json values = Json::object();
        if (is3D(layer)) {
            const Json& s = layer["style_3d"];
            values["style_3d.bevel.width"] = rounded(num(member(s, "bevel"), "width", 0.0) * k, 0.001);
            const Json& bands = list(s, "bands");
            for (size_t i = 0; i < bands.size(); i++) {
                if (!flag(bands[i], "enabled", true)) continue;
                const std::string at = "style_3d.bands[" + std::to_string(i) + "].";
                values[at + "grow"] = rounded(num(bands[i], "grow", 0.0) * k, 0.001);
                values[at + "bevel.width"] = rounded(num(member(bands[i], "bevel"), "width", 0.0) * k, 0.001);
            }
            return values;
        }
        const Json& rims = list(member(layer, "style"), "rims");
        for (size_t i = 0; i < rims.size(); i++) {
            if (flag(rims[i], "enabled", true)) values["rim#" + std::to_string(i) + ".width"] = rounded(num(rims[i], "width", 0.0) * k, 0.001);
        }
        return values;
    }

    void thicknessIssues(const Json& layer, std::vector<StyleLintFinding>& out) {
        if (!isText(layer)) return;
        const double total = rimTotal(layer);
        const std::string element = is3D(layer) ? "bands" : "rims";
        // 3D titles read their edge from the extrusion: only 2D rims can be too thin
        if (!is3D(layer) && total > 0.0 && total < LINT_MIN_RIMS) {
            StyleLintFinding f = finding(layer, "rims_thin", element, total, LINT_MIN_RIMS,
                                         "the rims total " + fixed(total, 1) + " % of the size: too thin to hold the title together on a busy background");
            f.fix = fixOf(Json::array({setOp(layer, scaledRims(layer, LINT_THIN_FIX_TOTAL / total))}));
            out.push_back(std::move(f));
        } else if (total > LINT_STEM) {
            StyleLintFinding f = finding(layer, "rims_thick", element, total, LINT_STEM,
                                         "the rims total " + fixed(total, 1) + " % of the size: wider than the letters' strokes, they eat the letters");
            f.fix = fixOf(Json::array({setOp(layer, scaledRims(layer, LINT_THICK_FIX_TOTAL / total))}));
            out.push_back(std::move(f));
        }
    }

    // opaque, barely blurred and near black
    bool hardShadow(const Json& fx, double size) {
        if (!flag(fx, "enabled", true) || str(fx, "type") != "drop_shadow") return false;
        const Json& p = member(fx, "params");
        titleart::TitleColor c;
        if (!p.contains("color") || !titleart::parseTitleColor(p["color"], c)) return false;
        return num(p, "opacity", 0.75) > LINT_HARD_SHADOW_OPACITY && num(p, "size", 14.0) < size * LINT_HARD_SHADOW_SIZE / 100.0 &&
               luma601({c.r, c.g, c.b}) < LINT_HARD_SHADOW_LUMA;
    }

    void shadowIssues(const Json& layer, std::vector<StyleLintFinding>& out) {
        const double size = titleart::layerSourceSize(layer);
        const Json& effects = list(layer, "effects");
        Json values = Json::object();
        long firstHard = -1;
        std::map<std::string, int> seen;
        std::string element;
        for (size_t i = 0; i < effects.size(); i++) {
            const int nth = seen[str(effects[i], "type")]++;
            if (!hardShadow(effects[i], size)) continue;
            if (firstHard < 0) {
                firstHard = static_cast<long>(i);
                element = nth ? "fx:drop_shadow#" + std::to_string(nth) : "fx:drop_shadow";
            }
            const std::string at = "effects[" + std::to_string(i) + "].params.";
            values[at + "opacity"] = LINT_SOFT_SHADOW_OPACITY;
            values[at + "size"] = std::round(size * LINT_SOFT_SHADOW_SIZE / 100.0);
        }
        if (firstHard < 0) return;
        const Json& p = member(effects[static_cast<size_t>(firstHard)], "params");
        const double sizePct = size > 0.0 ? num(p, "size", 0.0) / size * 100.0 : 0.0;
        StyleLintFinding f = finding(layer, "hard_shadow", element, sizePct, LINT_HARD_SHADOW_SIZE,
                                     "a hard black drop shadow (opacity " + fixed(num(p, "opacity", 0.0), 2) + ", blur " + fixed(sizePct, 1) +
                                         " % of the size): soften it to about 0.6 opacity with a wide blur");
        f.fix = fixOf(Json::array({setOp(layer, std::move(values))}));
        out.push_back(std::move(f));
    }

    void spanIssues(const Json& layer, std::vector<StyleLintFinding>& out) {
        if (!multiLine(layer)) return;
        // (path of the span member, element name, the spread) of every layer-wide spectrum
        struct Bad {
            std::string path, element;
            double spread;
        };
        std::vector<Bad> bad;
        if (is3D(layer)) {
            const Json& s = layer["style_3d"];
            auto check = [&](const Json& m, const std::string& path, const std::string& name) {
                auto gradient = m.find("gradient");
                if (gradient == m.end() || !hasStops(*gradient) || str(m, "gradient_span") != "layer") return;
                const Element e = element(name, rampSamples(*gradient));
                if (e.spectrum) bad.push_back({path, name, e.hueSpread});
            };
            check(member(s, "face"), "style_3d.face.gradient_span", "face");
            const Json& bands = list(s, "bands");
            for (size_t i = 0; i < bands.size(); i++) {
                check(member(bands[i], "material"), "style_3d.bands[" + std::to_string(i) + "].material.gradient_span", "band#" + std::to_string(i));
            }
        } else {
            const Json& style = member(layer, "style");
            auto check = [&](const Json& m, const std::string& path, const std::string& name) {
                const Json& fill = member(m, "fill");
                if (str(fill, "kind") == "solid" || str(fill, "span") != "layer") return;
                const Element e = element(name, rampSamples(member(fill, "gradient")));
                if (e.spectrum) bad.push_back({path, name, e.hueSpread});
            };
            check(member(style, "face"), "face.fill.span", "face");
            const Json& rims = list(style, "rims");
            for (size_t i = 0; i < rims.size(); i++) {
                check(member(rims[i], "material"), "rim#" + std::to_string(i) + ".material.fill.span", "rim#" + std::to_string(i));
            }
        }
        if (bad.empty()) return;
        Json values = Json::object();
        for (const Bad& b : bad) values[b.path] = "line";
        StyleLintFinding f = finding(layer, "spectrum_span", bad[0].element, bad[0].spread, LINT_SPECTRUM_SPREAD,
                                     "a rainbow on " + bad[0].element + " spans the whole block: each line gets only part of the spectrum");
        f.fix = fixOf(Json::array({setOp(layer, std::move(values))}));
        out.push_back(std::move(f));
    }

    // switches that are on and render nothing (2D)
    void noopIssues(const Json& layer, std::vector<StyleLintFinding>& out) {
        if (is3D(layer)) return;
        const Json& style = member(layer, "style");
        const Json& rims = list(style, "rims");
        const Json& face = member(style, "face");
        const bool knockout = flag(style, "knockout", false);
        const bool anyRim = std::any_of(rims.begin(), rims.end(), [](const Json& r) { return flag(r, "enabled", true) && num(r, "width", 0.0) > 0.0; });
        const double faceOpacity = num(face, "opacity", 1.0);
        if (knockout && !anyRim) {
            StyleLintFinding f = finding(layer, "noop_knockout", "rims", 0.0, 0.0,
                                         "Hollow Letters is on without a rim to cut through: the letters disappear");
            Json ops = Json::array({Json{{"op", "add_rim"}, {"layer", layerRef(layer)},
                                         {"values", Json{{"width", LINT_KNOCKOUT_RIM}, {"material.fill.kind", "solid"}, {"material.fill.color", "#ffffff"}}}}});
            // with an opaque face the hollow would stay covered: the same finding on the next lint
            if (faceOpacity >= 1.0) ops.push_back(setOp(layer, Json{{"face.opacity", 0}}));
            f.fix = fixOf(std::move(ops));
            out.push_back(std::move(f));
        } else if (knockout && faceOpacity >= 1.0) {
            StyleLintFinding f = finding(layer, "noop_knockout", "face", faceOpacity, 1.0,
                                         "Hollow Letters is on under an opaque face: the face covers the hollow it cuts");
            f.fix = fixOf(Json::array({setOp(layer, Json{{"face.opacity", 0}})}));
            out.push_back(std::move(f));
        }
        const Json& erode = member(style, "erode");
        const double amount = num(erode, "amount", 0.0), edge = num(erode, "edge", 0.0);
        if (flag(erode, "enabled", false) && !(amount > 0.0 && edge > 0.0)) {
            StyleLintFinding f = finding(layer, "noop_erode", "erode", amount > 0.0 ? edge : amount, 0.0,
                                         std::string("Erode is on with ") + (amount > 0.0 ? "edge" : "amount") + " 0: nothing is eroded");
            Json values = Json::object();
            if (amount <= 0.0) values["style.erode.amount"] = LINT_ERODE_AMOUNT;
            if (edge <= 0.0) values["style.erode.edge"] = LINT_ERODE_EDGE;
            f.fix = fixOf(Json::array({setOp(layer, std::move(values))}));
            out.push_back(std::move(f));
        }
    }

}

const std::vector<std::string>& titlelibrary::styleLintRules() {
    static const std::vector<std::string> rules = {"noop_knockout", "noop_erode", "spectrum_span", "contrast",
                                                   "hue_count",     "rims_thin",  "rims_thick",    "hard_shadow"};
    return rules;
}

const char* titlelibrary::styleLintRuleText(const std::string& rule) {
    if (rule == "contrast") return "Neighbouring rims need different lightness, or they read as one blurred band: alternate light and dark outward.";
    if (rule == "hue_count") return "Keep to two hue families across the face and its rims: make the rims neutral (white, black, silver) or a shade of the face's own hue.";
    if (rule == "rims_thin") return "Rims adding up to less than about 5 % of the size disappear on a busy background; finished titles usually total 7 to 10 %.";
    if (rule == "rims_thick") return "Rims wider than the letters' strokes (about 16 % of the size for heavy faces) eat into the letters and fill the counters: use a heavier font or thinner rims.";
    if (rule == "hard_shadow") return "An opaque, unblurred black shadow looks cheap: keep it near 0.5 to 0.7 opacity with a wide soft blur, a small offset and a tint toward the face.";
    if (rule == "spectrum_span") return "A rainbow should run across each line: spread over the whole block, every line gets only a slice of the spectrum.";
    if (rule == "noop_knockout") return "Hollow Letters cuts the face out of its rims: it needs an enabled rim to show and a face fill opacity below 100 %.";
    if (rule == "noop_erode") return "Erode only roughens the letters when both its amount and its edge width are above zero.";
    return "";
}

std::vector<StyleLintFinding> titlelibrary::lintLayer(const titleart::TitleArtDoc&, const Json& layer) {
    std::vector<StyleLintFinding> out;
    if (!layer.is_object() || !titleart::layerTypeHasStyle(titleart::layerTypeOf(layer))) return out;
    // most visible first, as the Designer lists them
    noopIssues(layer, out);
    spanIssues(layer, out);
    contrastIssues(layer, out);
    hueIssues(layer, out);
    thicknessIssues(layer, out);
    shadowIssues(layer, out);
    return out;
}

std::vector<StyleLintFinding> titlelibrary::lintDocument(const titleart::TitleArtDoc& doc) {
    std::vector<StyleLintFinding> out;
    std::set<std::string> guides;
    titleart::forEachLayer(doc.root, [&](const Json& layer, const Json* parent, int) {
        const bool insideGuide = parent && guides.count(str(*parent, "id"));
        if (flag(layer, "guide", false) || insideGuide) {
            guides.insert(str(layer, "id"));
            return true;
        }
        std::vector<StyleLintFinding> found = lintLayer(doc, layer);
        out.insert(out.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
        return true;
    });
    return out;
}
