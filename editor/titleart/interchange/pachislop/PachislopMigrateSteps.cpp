// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/io/migrate/{generatorParams,effectDedupe,materialEffects,
// contentEffects}.ts, src/generators/fromEffect.ts, src/model/companions.ts (placeCompanions),
// src/effects/a/recipes.ts, src/effects/globalLight.ts (angleFromGlobal) and the preset effects of
// src/generators (lightGlow, particleGlow, halftoneFade, vignette) by SafEight, used with the
// author's permission. The step headers there document the maths; it is kept unchanged.

#include "PachislopMigrate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pachislop;

namespace {

    constexpr double PI = 3.14159265358979323846;
    constexpr double DEG = PI / 180.0;

    double clamp(double x, double lo, double hi) { return std::min(hi, std::max(lo, x)); }

    std::string hexByte(double v) {
        char b[4];
        std::snprintf(b, sizeof(b), "%02x", (int)std::lround(clamp(v, 0, 255)));
        return b;
    }

    std::string hexOf(const std::vector<double>& rgb01) {
        std::string out = "#";
        for (double v : rgb01) out += hexByte(std::round(clamp(v, 0, 1) * 255));
        return out;
    }

    std::vector<double> hexToRgb(const std::string& hex) {
        std::string h = hex.size() && hex[0] == '#' ? hex.substr(1) : hex;
        if (h.size() == 3) h = std::string{h[0], h[0], h[1], h[1], h[2], h[2]};
        const unsigned long n = std::strtoul(h.c_str(), nullptr, 16);
        return {((n >> 16) & 255) / 255.0, ((n >> 8) & 255) / 255.0, (n & 255) / 255.0};
    }

    double toLin(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
    double toSrgb(double c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055; }
    double smoothstep(double a, double b, double x) {
        const double t = clamp((x - a) / (b - a), 0, 1);
        return t * t * (3 - 2 * t);
    }

    Json gradientOf(std::initializer_list<std::pair<double, const char*>> stops) {
        Json list = Json::array();
        for (const auto& s : stops) list.push_back(Json{{"pos", s.first}, {"color", s.second}});
        return Json{{"stops", list}};
    }

    // ---- effects the steps make ----

    Json makeEffect(IdMint& ids, const std::string& type, Json params) {
        return Json{{"id", ids.next()}, {"type", type}, {"enabled", true}, {"params", std::move(params)}};
    }

    Json presetEffect(const std::string& type, Json params) {
        return Json{{"id", type}, {"type", type}, {"enabled", true}, {"params", std::move(params)}, {"fromPreset", true}};
    }

    Json lightGlow(double size, double intensity = 1) {
        return presetEffect("outerGlow", Json{{"fill", "layer"}, {"technique", "softer"}, {"size", size}, {"intensity", intensity}, {"spread", 0},
                                              {"opacity", 1}, {"mode", "add"}, {"onFace", 1}});
    }
    // src/generators/particles.ts GLOW_AMOUNT
    Json particleGlow(double size) { return lightGlow(size, 0.8); }
    Json halftoneFade(double cell) {
        return presetEffect("halftone", Json{{"source", "alpha"}, {"size", cell}, {"angle", 45}, {"fade", 4 * cell}});
    }
    std::string greyHex(double v) {
        const double s = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
        const std::string h = hexByte(std::round(clamp(s, 0, 1) * 255));
        return "#" + h + h + h;
    }
    Json vignette(double amount) {
        const auto at = [&](double x) {
            const double t = x <= 0.25 ? 0 : [](double u) { return u * u * (3 - 2 * u); }((x - 0.25) / 0.75);
            return Json{{"pos", x}, {"color", greyHex(1 - amount * t)}};
        };
        Json stops = Json::array({at(0)});
        for (int i = 0; i < 13; i++) stops.push_back(at(0.25 + (0.75 * i) / 12));
        return presetEffect("gradientOverlay", Json{{"style", "radial"}, {"gradient", Json{{"stops", stops}}}, {"mode", "shade"}, {"scale", 1},
                                                    {"offset", 0}, {"wrap", "clamp"}, {"amount", 1}});
    }
    // a copy of a preset effect for a layer (registry.ts lookEffect: the defaults come with the
    // S83 conversion, PachislopMigrate.h)
    Json lookEffect(const Json& fx, IdMint& ids) {
        Json out = fx;
        out["id"] = ids.next();
        out["fromPreset"] = true;
        return out;
    }

    // ---- recipes (src/effects/a/recipes.ts) ----

    Json hueWheel(double saturation) {
        const double s = clamp(saturation, 0, 1);
        const double corners[7][3] = {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0}};
        Json stops = Json::array();
        for (int i = 0; i < 7; i++) {
            std::vector<double> c;
            for (double v : corners[i]) c.push_back(1 - s + s * v);
            stops.push_back(Json{{"pos", i / 6.0}, {"color", hexOf(c)}});
        }
        return Json{{"stops", stops}};
    }

    Json rainbowOverlay(double bandsIn, double angle, double phase, double saturation, double amount, const std::string& mode) {
        const double bands = std::max(0.1, bandsIn);
        const double off = 0.5 * bands - 0.5 - phase;
        return Json{{"gradient", hueWheel(saturation)}, {"style", "linear"}, {"angle", angle}, {"scale", 1 / bands},
                    {"offset", off - std::floor(off)}, {"wrap", "repeat"}, {"amount", amount}, {"mode", mode}};
    }

    Json glintOverlay(double angle, double position, double widthIn, double softness, bool twin, const std::string& color, double intensity) {
        const double soft = std::min(0.999, std::max(0.0, softness / 100));
        const double w = std::max(0.5, widthIn);
        const double len = 5 * w;
        const double k = std::max(0.0, intensity / 100);
        const double amount = std::min(1.0, k);
        const double gain = amount > 0 ? k / amount : 1;
        std::vector<double> col;
        for (double c : hexToRgb(color)) col.push_back(toLin(c));
        struct Band { double m, h, peak; };
        std::vector<Band> bands = {{0.5, w / len, 1}};
        if (twin) bands.push_back({0.5 - (2.1 * w) / len, (0.32 * w) / len, 0.8});
        const auto value = [&](double x) {
            double v = 0;
            for (const Band& b : bands) v += b.peak * (1 - smoothstep(b.h * (1 - soft), b.h, std::abs(x - b.m)));
            return v;
        };
        std::set<double> xs = {0, 1};
        const int SEG = 16;
        for (const Band& b : bands) {
            const double hi = b.h * (1 - soft);
            for (int side : {-1, 1}) for (int i = 0; i <= SEG; i++) xs.insert(b.m + side * (hi + ((b.h - hi) * i) / SEG));
        }
        Json stops = Json::array();
        for (double x : xs) {
            if (x < 0 || x > 1) continue;
            const double v = value(x) * gain;
            std::vector<double> c;
            for (double l : col) c.push_back(toSrgb(std::min(1.0, l * v)));
            stops.push_back(Json{{"pos", std::round(x * 1e5) / 1e5}, {"color", hexOf(c)}});
        }
        return Json{{"gradient", Json{{"stops", stops}}}, {"style", "sweep"}, {"angle", angle}, {"length", len}, {"position", position},
                    {"wrap", "clamp"}, {"amount", amount}, {"mode", "light"}};
    }

    Json longShadowExtrude(double angle, double length, double fade, const std::string& color, double opacity) {
        return Json{{"angle", angle}, {"length", length}, {"gradient", Json{{"stops", Json::array({Json{{"pos", 0}, {"color", color}}})}}},
                    {"shading", 0}, {"edge", 0}, {"fade", fade}, {"opacity", opacity}};
    }

    Json levelsChannel(double inBlack = 0, double inWhite = 255, double gamma = 1, double outBlack = 0, double outWhite = 255) {
        return Json{{"inBlack", inBlack}, {"inWhite", inWhite}, {"gamma", gamma}, {"outBlack", outBlack}, {"outWhite", outWhite}};
    }

    Json levels(double inBlack, double inWhite, double gamma, double outBlack = 0, double outWhite = 255) {
        return Json{{"type", "levels"},
                    {"channels", Json{{"rgb", levelsChannel(inBlack, inWhite, gamma, outBlack, outWhite)}, {"r", levelsChannel()},
                                      {"g", levelsChannel()}, {"b", levelsChannel()}}}};
    }

    double wrap180(double d) {
        double r = std::fmod(std::fmod(d, 360) + 360, 360);
        return r > 180 ? r - 360 : r;
    }

    // ---- reading old params ----

    struct Reader {
        const Json& p;
        double n(const char* k, double def) const { return p.contains(k) && p[k].is_number() ? p[k].get<double>() : def; }
        std::string s(const char* k, const std::string& def) const { return p.contains(k) && p[k].is_string() ? p[k].get<std::string>() : def; }
        bool b(const char* k, bool def) const { return p.contains(k) && p[k].is_boolean() ? p[k].get<bool>() : def; }
        Json g(const char* k, const Json& def) const { return p.contains(k) && p[k].is_object() && p[k].contains("stops") ? p[k] : def; }
    };

    Json paramsOf(const Json& fx) { return fx.contains("params") && fx["params"].is_object() ? fx["params"] : Json::object(); }

    // ---- step 1: generator params ----

    struct Removed {
        const char* type;
        Json params;
    };

    const std::vector<Removed>& removedGeneratorParams() {
        static const std::vector<Removed> table = {
            {"particles", Json{{"glow", 8}}},
            {"speedLines", Json{{"edgeBlur", 0}, {"glow", 0}, {"halftone", false}, {"dotSize", 6}, {"fillBurst", true}, {"burstColor", "#ffffff"}}},
            {"sunburst", Json{{"vignette", 45}}},
            {"shockwave", Json{{"glow", 20}}},
            {"magicCircle", Json{{"glow", 14}}},
            {"brushSwirl", Json{{"glow", 0}}},
            {"patternFill", Json{{"opacity", 100}, {"fade", "none"}, {"fadeX", 0}, {"fadeY", 0}, {"fadeRadius", 600}, {"fadeAngle", 90},
                                 {"fadeDistance", 300}, {"feather", 400}}},
        };
        return table;
    }

    Json gradientMask(IdMint& ids, const std::string& shape, double x, double y, double angle, double length, double inner) {
        return Json{{"id", ids.next()}, {"enabled", true}, {"invert", false}, {"feather", 0}, {"density", 1}, {"linked", true},
                    {"kind", "gradient"}, {"mid", 0.5}, {"shape", shape}, {"x", x}, {"y", y}, {"angle", angle}, {"length", length},
                    {"inner", inner}};
    }

    Json defaultTransform() {
        return Json{{"x", 0}, {"y", 0}, {"z", 0}, {"rotX", 0}, {"rotY", 0}, {"rotZ", 0}, {"scale", 1}, {"flipX", false}, {"flipY", false}, {"opacity", 1}};
    }

    Json layerBase(IdMint& ids, const std::string& name) {
        return Json{{"id", ids.next()}, {"name", name}, {"visible", true}, {"solo", false}, {"locked", false}, {"guide", false},
                    {"blend", "normal"}, {"transform", defaultTransform()}, {"effects", Json::array()}, {"clip", false}, {"masks", Json::array()}};
    }

    // the layer, and the layer to insert below it (the ベタフラ burst fill), if any
    std::pair<Json, Json> migrateGenerator(const Json& l, IdMint& ids, bool& changed) {
        const Json gen = l.value("generator", Json::object());
        const std::string type = str(gen, "type");
        const Json stored = gen.value("params", Json::object());
        const Json* removed = nullptr;
        for (const Removed& r : removedGeneratorParams()) if (type == r.type) removed = &r.params;
        bool any = false;
        if (removed) for (auto it = removed->begin(); it != removed->end(); ++it) any = any || stored.contains(it.key());
        if (!any) return {l, Json()};
        changed = true;
        Json params = stored;
        for (auto it = removed->begin(); it != removed->end(); ++it) params.erase(it.key());
        const auto n = [&](const char* k) { return stored.contains(k) && stored[k].is_number() ? stored[k].get<double>() : (*removed)[k].get<double>(); };
        const auto b = [&](const char* k) { return stored.contains(k) && stored[k].is_boolean() ? stored[k].get<bool>() : (*removed)[k].get<bool>(); };
        const auto s = [&](const char* k) { return stored.contains(k) && stored[k].is_string() ? stored[k].get<std::string>() : (*removed)[k].get<std::string>(); };
        const Json transform = l.value("transform", defaultTransform());
        const double scale = num(transform, "scale", 1);
        Json fx = Json::array();
        double opacity = 1;
        Json below;
        Json masks = l.value("masks", Json::array());
        if (type == "particles") {
            if (n("glow") > 0) fx.push_back(particleGlow(n("glow")));
        } else if (type == "speedLines") {
            const bool beta = (stored.contains("type") && stored["type"].is_string() ? stored["type"].get<std::string>() : "focus") == "beta";
            const double edge = std::max(0.0, n("edgeBlur")) * scale;
            if (edge > 0) fx.push_back(presetEffect("gaussianBlur", Json{{"radius", beta ? edge / 3 : 0.4 * edge}}));
            if (beta && b("halftone")) fx.push_back(halftoneFade(std::max(2.0, n("dotSize")) * scale));
            if (n("glow") > 0) fx.push_back(lightGlow(n("glow")));
            if (beta && b("fillBurst")) {
                const std::string burst = s("burstColor");
                const double own = std::min(1.0, std::max(0.0, (stored.contains("opacity") && stored["opacity"].is_number() ? stored["opacity"].get<double>() : 85) / 100));
                Json layer = layerBase(ids, str(l, "name") + " burst");
                layer["kind"] = "generator";
                layer["generator"] = Json{{"type", "sunburst"}, {"params", Json{{"colorMode", "ab"}, {"colorA", burst}, {"colorB", burst}, {"glow", 0}}}};
                layer["visible"] = l.value("visible", true);
                layer["guide"] = l.value("guide", false);
                layer["blend"] = l.value("blend", Json("normal"));
                layer["clip"] = l.value("clip", false);
                if (l.contains("parentId")) layer["parentId"] = l["parentId"];
                Json t = transform;
                t["opacity"] = num(transform, "opacity", 1) * own;
                layer["transform"] = t;
                Json copies = Json::array();
                for (Json m : l.value("masks", Json::array())) {
                    m["id"] = ids.next();
                    copies.push_back(m);
                }
                layer["masks"] = copies;
                below = layer;
            }
        } else if (type == "sunburst") {
            if (n("vignette") > 0) fx.push_back(vignette(std::min(1.0, n("vignette") / 100)));
        } else if (type == "shockwave" || type == "magicCircle" || type == "brushSwirl") {
            const double ring = type == "brushSwirl" ? 0.6 : 1;
            if (n("glow") > 0) fx.push_back(lightGlow(n("glow") * scale, ring));
        } else if (type == "patternFill") {
            opacity = std::min(1.0, std::max(0.0, n("opacity") / 100));
            const double half = std::max(0.0, n("feather")) / 3;
            if (s("fade") == "radial") {
                const double length = std::max(0.0, n("fadeRadius")) + half;
                masks.push_back(gradientMask(ids, "radial", n("fadeX"), n("fadeY"), 0, length, length > 0 ? std::max(0.0, length - 2 * half) / length : 0));
            } else if (s("fade") == "linear") {
                const double a = n("fadeAngle") * DEG;
                const double start = n("fadeDistance") - half;
                masks.push_back(gradientMask(ids, "linear", std::cos(a) * start, std::sin(a) * start, n("fadeAngle"), 2 * half, 0));
            }
        }
        Json out = l;
        Json g = gen;
        g["params"] = params;
        out["generator"] = g;
        Json t = transform;
        t["opacity"] = num(transform, "opacity", 1) * opacity;
        out["transform"] = t;
        Json effects = Json::array();
        for (const Json& e : fx) effects.push_back(lookEffect(e, ids));
        for (const Json& e : l.value("effects", Json::array())) effects.push_back(e);
        out["effects"] = effects;
        out["masks"] = masks;
        return {out, below};
    }

    // ---- step 2: effect dedupe ----

    const std::set<std::string>& dedupeRemoved() {
        static const std::set<std::string> removed = {"gradientGlow", "stroke", "longShadow", "shine", "rainbow", "hueSaturation", "colorAdjust", "gradientMap"};
        return removed;
    }

    Json silverRamp() { return gradientOf({{0, "#14161c"}, {0.35, "#5d6474"}, {0.7, "#c9cfdb"}, {1, "#ffffff"}}); }

    std::vector<Json> replaceDedupe(const Json& fx, double globalLight, bool first, IdMint& ids) {
        const Json p = paramsOf(fx);
        const Reader r{p};
        const std::string type = str(fx, "type");
        std::vector<Json> out;
        if (type == "gradientGlow") {
            const Json oldGradient = gradientOf({{0, "#ffffff"}, {0.22, "#fff36b"}, {0.55, "#ff8a00"}, {1, "#e01400"}});
            Json common = Json{{"fill", "distance"}, {"gradient", r.g("gradient", oldGradient)}, {"technique", "precise"},
                               {"size", r.n("width", 34)}, {"intensity", 1}, {"opacity", r.n("opacity", 1)}};
            const double soft = clamp(r.n("softness", 0.75), 0, 1);
            if (r.b("inner", false)) {
                common["choke"] = 1 - soft;
                common["mode"] = "normal";
                common["source"] = "edge";
                out.push_back(makeEffect(ids, "innerGlow", common));
            } else {
                common["spread"] = 1 - soft;
                common["mode"] = "behind";
                out.push_back(makeEffect(ids, "outerGlow", common));
            }
        } else if (type == "stroke") {
            const double opacity = r.n("opacity", 1);
            const std::vector<std::pair<double, std::string>> rings = {
                {r.n("width1", 5), r.s("color1", "#ffffff")}, {r.n("width2", 9), r.s("color2", "#d90000")}, {r.n("width3", 4), r.s("color3", "#2a0000")}};
            if (first) {
                double edge = 0;
                std::vector<std::pair<double, std::string>> cumulative;
                for (const auto& [w, color] : rings) {
                    edge += std::max(0.0, w);
                    if (w > 0) cumulative.push_back({edge, color});
                }
                std::reverse(cumulative.begin(), cumulative.end());
                for (const auto& [width, color] : cumulative) {
                    out.push_back(makeEffect(ids, "keyline", Json{{"width", width}, {"color", color}, {"opacity", opacity}, {"softness", 0},
                                                                   {"threshold", 0.5}, {"follow", "layer"}}));
                }
            } else {
                const double threshold = std::max(0.05, std::min(0.5, opacity / 2));
                for (const auto& [w, color] : rings) {
                    if (w <= 0) continue;
                    out.push_back(makeEffect(ids, "keyline", Json{{"width", w}, {"color", color}, {"opacity", opacity}, {"softness", 0},
                                                                   {"threshold", threshold}, {"follow", "stack"}}));
                }
            }
        } else if (type == "longShadow") {
            // angleFromGlobal({convention: cwFromRight, relation: away}, G)
            const bool followed = p.contains("useGlobalLight") && p["useGlobalLight"] == true;
            const double angle = followed ? wrap180(-(globalLight + 180)) : r.n("angle", 45);
            out.push_back(makeEffect(ids, "extrude2d", longShadowExtrude(angle, r.n("length", 260), r.n("fade", 1), r.s("color", "#000000"), r.n("opacity", 0.55))));
        } else if (type == "shine") {
            out.push_back(makeEffect(ids, "gradientOverlay", glintOverlay(-r.n("angle", -30), r.n("position", -20), r.n("width", 70), r.n("softness", 85),
                                                                          r.b("double", true), r.s("color", "#ffffff"), r.n("intensity", 90))));
        } else if (type == "rainbow") {
            const std::string mode = r.s("mode", "tint");
            out.push_back(makeEffect(ids, "gradientOverlay", rainbowOverlay(r.n("scale", 1), r.n("angle", 20), r.n("phase", 0), r.n("saturation", 0.9),
                                                                            r.n("amount", 1), mode == "tint" || mode == "color" ? "color" : mode)));
        } else if (type == "hueSaturation") {
            const double hue = r.n("hue", 0), sat = r.n("saturation", 0.25), light = r.n("lightness", 0) * 100;
            const bool colorize = r.b("colorize", false);
            Json adjustment;
            if (colorize) {
                adjustment = Json{{"type", "hueSaturation"}, {"hue", std::fmod(std::fmod(hue, 360) + 360, 360)},
                                  {"saturation", std::min(100.0, std::max(0.0, (0.5 + 0.5 * sat) * 100))}, {"lightness", light}, {"colorize", true}};
            } else {
                adjustment = Json{{"type", "hueSaturation"}, {"hue", std::fmod(std::fmod(hue + 180, 360) + 360, 360) - 180},
                                  {"saturation", sat < 0 ? sat * 100 : (150 * sat) / (1 + 1.5 * sat)}, {"lightness", light}, {"colorize", false}};
            }
            out.push_back(makeEffect(ids, "adjust", Json{{"adjustment", adjustment}}));
        } else if (type == "colorAdjust") {
            const double exposure = r.n("exposure", 0), black = r.n("black", 0), white = r.n("white", 1);
            const double gamma = std::max(0.05, r.n("gamma", 1));
            const double A = 1 + r.n("contrast", 0.15);
            const double B = (r.n("brightness", 0) - 0.5) * A + 0.5;
            std::vector<Json> chain;
            if (exposure != 0) chain.push_back(Json{{"type", "exposure"}, {"exposure", exposure}, {"offset", 0}, {"gamma", 1}});
            if (black != 0 || white != 1 || gamma != 1) chain.push_back(levels(black * 255, std::max(white, black + 1 / 255.0) * 255, gamma));
            if (A != 1 || B != 0) {
                const auto f = [&](double v) { return std::min(1.0, std::max(0.0, A * v + B)); };
                const double lo = A > 0 ? std::min(1.0, std::max(0.0, -B / A)) : 0;
                const double hi = A > 0 ? std::min(1.0, std::max(0.0, (1 - B) / A)) : 1;
                const double outLo = lo > 0 ? 0 : f(0);
                const double outHi = hi < 1 ? 1 : f(1);
                chain.push_back(hi > lo ? levels(lo * 255, hi * 255, 1, outLo * 255, outHi * 255) : levels(0, 255, 1, f(0) * 255, f(0) * 255));
            }
            for (const Json& adjustment : chain) out.push_back(makeEffect(ids, "adjust", Json{{"adjustment", adjustment}}));
        } else if (type == "gradientMap") {
            const double amount = clamp(r.n("amount", 1), 0, 1);
            Json gradient = r.g("gradient", silverRamp());
            if (amount < 1) {
                for (Json& st : gradient["stops"]) st["alpha"] = (st.contains("alpha") && st["alpha"].is_number() ? st["alpha"].get<double>() : 1.0) * amount;
            }
            out.push_back(makeEffect(ids, "adjust", Json{{"adjustment", Json{{"type", "gradientMap"}, {"gradient", gradient}, {"reverse", r.b("reverse", false)}}}}));
        } else {
            return {fx};
        }
        // the first replacement keeps the instance's id
        for (size_t i = 0; i < out.size(); i++) {
            if (i == 0) out[i]["id"] = fx.value("id", Json(""));
            out[i]["enabled"] = fx.value("enabled", Json(true));
        }
        return out;
    }

    // ---- step 3: material effects ----

    const double SIZE = 200;
    double pct(double px) { return (px / SIZE) * 100; }

    double roundP(double x) { return std::sqrt(std::max(0.0, 1 - std::pow(1 - clamp(x, 0, 1), 2))); }

    struct OldProfile {
        double (*f)(double);
        std::vector<double> corners;
    };

    const std::map<std::string, OldProfile>& oldProfiles() {
        static const std::map<std::string, OldProfile> table = {
            {"button", {[](double x) { return roundP(x); }, {}}},
            {"mesa", {[](double x) { return x * x * (3 - 2 * x); }, {}}},
            {"ridge", {[](double x) { return x < 0.5 ? roundP(2 * x) : 1 - 0.45 * smoothstep(0.5, 1, x); }, {0.5}}},
            {"stepped", {[](double x) { return 0.55 * roundP(x / 0.45) + 0.45 * roundP((x - 0.55) / 0.45); }, {0.45, 0.55}}},
            {"cove", {[](double x) { return 1 - std::sqrt(std::max(0.0, 1 - x * x)); }, {}}},
        };
        return table;
    }

    void oldProfile(const std::string& name, Json& bevel) {
        if (name == "chamfer") {
            bevel["profile"] = "chamfer";
            return;
        }
        auto it = oldProfiles().find(name);
        const OldProfile& p = it == oldProfiles().end() ? oldProfiles().at("button") : it->second;
        std::set<double> xs = {0, 0.01, 0.03, 0.06, 0.1, 0.16, 0.24, 0.33, 0.45, 0.5, 0.55, 0.65, 0.78, 0.9, 1};
        for (double c : p.corners) xs.insert(c);
        Json points = Json::array();
        for (double x : xs) {
            const bool corner = std::find(p.corners.begin(), p.corners.end(), x) != p.corners.end();
            points.push_back(Json{{"x", x}, {"y", clamp(p.f(x), 0, 1)}, {"corner", corner}});
        }
        bevel["profile"] = "custom";
        bevel["profileCurve"] = Json{{"points", points}};
    }

    double srgbShadow(double d) { return clamp(1 - std::pow(1 - clamp(d, 0, 1), 1 / 2.2), 0, 1); }

    Json bevel2D() {
        return Json{{"enabled", true}, {"profile", "smooth"},
                    {"profileCurve", Json{{"points", Json::array({Json{{"x", 0}, {"y", 0}, {"corner", false}}, Json{{"x", 0.5}, {"y", 0.75}, {"corner", false}},
                                                                  Json{{"x", 1}, {"y", 1}, {"corner", false}}})}}},
                    {"size", 3}, {"sizeMode", "fixed"}, {"fit", 70}, {"depth", 3}, {"soften", 0}, {"lightAngle", 120}, {"lightAltitude", 30},
                    {"useGlobalLight", true}, {"gloss", "ring"},
                    {"glossCurve", Json{{"points", Json::array({Json{{"x", 0}, {"y", 0}, {"corner", false}}, Json{{"x", 1}, {"y", 1}, {"corner", false}}})}}},
                    {"highlightColor", "#ffffff"}, {"highlightOpacity", 0.85}, {"shadowColor", "#000000"}, {"shadowOpacity", 0.6}};
    }

    template<class Shade>
    Json oldBevel(const Reader& r, double width, double depth, double elevationDef, double highlight, Shade shade) {
        Json b = bevel2D();
        oldProfile(r.s("profile", "button"), b);
        const double elevation = r.n("elevation", elevationDef);
        b["size"] = pct(std::max(0.5, r.n("width", width)));
        b["sizeMode"] = "fixed";
        b["depth"] = clamp(r.n("depth", depth) / 100 / 0.35, 0, 10);
        b["soften"] = pct(2 * r.n("smooth", 2));
        b["lightAngle"] = r.n("angle", 135);
        b["lightAltitude"] = elevation;
        b["useGlobalLight"] = r.b("useGlobalLight", false);
        b["gloss"] = "linear";
        b["highlightColor"] = r.s("highlightColor", "#ffffff");
        b["highlightOpacity"] = clamp(0.85 * (r.n("highlight", highlight) / 100), 0, 1);
        b["shadowColor"] = "#000000";
        b["shadowOpacity"] = srgbShadow(shade(std::sin(clamp(elevation, 0, 90) * DEG)));
        return b;
    }

    Json solidFill(const std::string& color) {
        return Json{{"kind", "solid"}, {"color", color}, {"gradient", gradientOf({{0, color.c_str()}, {1, color.c_str()}})}, {"angle", 90},
                    {"scale", 1}, {"offset", 0}, {"span", "layer"}};
    }

    Json material2D(Json fill) {
        Json bevel = bevel2D();
        bevel["enabled"] = false;
        return Json{{"fill", std::move(fill)}, {"opacity", 1}, {"bevel", bevel},
                    {"reflection", Json{{"enabled", false}, {"map", "highway"}, {"amount", 1}, {"blur", 75}, {"rotation", 0}, {"colorize", 0}}},
                    {"finish", Json{{"enabled", false},
                                    {"gradient", gradientOf({{0, "#3a0c00"}, {0.25, "#a83800"}, {0.5, "#f08c00"}, {0.75, "#ffd23a"}, {1, "#fff8d6"}})},
                                    {"amount", 1}}},
                    {"pattern", Json{{"kind", "none"}, {"color", "#000000"}, {"color2", "#ffffff"}, {"opacity", 1}, {"scale", 12}, {"angle", 0},
                                     {"seed", 1}, {"shade", 0.6}, {"relief", 0}}},
                    {"satin", Json{{"enabled", false}, {"color", "#000000"}, {"blend", "multiply"}, {"opacity", 0.5}, {"angle", 19},
                                   {"useGlobalLight", false}, {"distance", 5}, {"size", 7},
                                   {"contour", Json{{"points", Json::array({Json{{"x", 0}, {"y", 0}, {"corner", false}}, Json{{"x", 0.5}, {"y", 1}, {"corner", false}},
                                                                            Json{{"x", 1}, {"y", 0}, {"corner", false}}})}}},
                                   {"invert", true}}},
                    {"texture", Json{{"kind", "none"}, {"amount", 0.3}, {"scale", 4}, {"blend", "overlay"}, {"seed", 1}, {"target", "color"}, {"angle", 0}}},
                    {"innerGlow", Json{{"enabled", false}, {"color", "#ffffff"}, {"size", 2}, {"opacity", 0.6}, {"noise", 0}}},
                    {"gloss", Json{{"enabled", false}, {"opacity", 0.55}, {"position", 0.45}, {"softness", 0.15}, {"curve", 0.25}, {"angle", 0}}},
                    {"refraction", 0},
                    {"glass", Json{{"enabled", false}, {"color", "#3cc8ff"}, {"tint", 1}, {"opacity", 0.7}, {"edge", 0.7}, {"caustic", 0.7}}},
                    {"sheen", Json{{"enabled", false}, {"color", "#ffffff"}, {"amount", 0.7}, {"position", -0.25}, {"width", 0.35}}}};
    }

    Json chromeMap(const std::string& name) {
        if (name == "gold") {
            return gradientOf({{0, "#a35c0c"}, {0.2, "#ffcf5a"}, {0.44, "#fffbe2"}, {0.5, "#ffe9a0"}, {0.506, "#3d1c00"}, {0.62, "#8a4a00"}, {0.82, "#eaa628"},
                               {1, "#fff3b8"}});
        }
        if (name == "rainbow") {
            return gradientOf({{0, "#ff3df2"}, {0.12, "#5a7bff"}, {0.25, "#38e8ff"}, {0.42, "#ffffff"}, {0.5, "#fff5a0"}, {0.506, "#300c40"}, {0.6, "#ff2a2a"},
                               {0.72, "#ff9b00"}, {0.86, "#fff200"}, {1, "#2bff3a"}});
        }
        if (name == "dark") {
            return gradientOf({{0, "#2a303c"}, {0.3, "#7f8899"}, {0.47, "#e8eef8"}, {0.5, "#ffffff"}, {0.506, "#000000"}, {0.78, "#141519"}, {1, "#5a5e66"}});
        }
        return gradientOf({{0, "#36568e"}, {0.24, "#a6c1ea"}, {0.45, "#ffffff"}, {0.5, "#eef3ff"}, {0.506, "#101219"}, {0.62, "#38373e"}, {0.82, "#a29d96"},
                           {1, "#f7f4ee"}});
    }

    Json tinted(Json gr, const std::string& tint, double amount) {
        if (!(amount > 0)) return gr;
        const std::vector<double> t = hexToRgb(tint);
        for (Json& s : gr["stops"]) {
            const std::vector<double> c = hexToRgb(str(s, "color", "#000000"));
            std::string out = "#";
            for (int k = 0; k < 3; k++) out += hexByte(std::round(clamp(c[k] * 255 * (1 + (t[k] - 1) * amount), 0, 255)));
            s["color"] = out;
        }
        return gr;
    }

    Json bevelMaterial(const Reader& r) {
        const double shadow = r.n("shadow", 70) / 100;
        Json m = material2D(solidFill("#ffffff"));
        m["bevel"] = oldBevel(r, 16, 100, 40, 110, [&](double lz) { return (shadow * lz) / (1 - shadow + shadow * lz); });
        return m;
    }

    Json chromeMaterial(const Reader& r, const Json& params) {
        const std::string map = r.s("map", "silver");
        const Json custom = params.contains("gradient") ? params["gradient"] : Json();
        const Json base = map == "custom" ? (custom.is_object() && custom.contains("stops") ? custom : chromeMap("silver")) : chromeMap(map);
        const double shading = r.n("shading", 50) / 100;
        const Json gradient = tinted(base, r.s("tint", "#ffffff"), r.n("tintAmount", 0) / 100);
        Json fill = Json{{"kind", "linear"}, {"color", gradient["stops"].empty() ? Json("#ffffff") : gradient["stops"][0]["color"]}, {"gradient", gradient},
                         {"angle", 90}, {"scale", 1 / 0.85}, {"offset", -0.425 * (r.n("horizon", 0) / 100)},
                         {"span", std::lround(r.n("repeat", 1)) > 1 ? "line" : "layer"}};
        Json m = material2D(fill);
        m["bevel"] = oldBevel(r, 14, 100, 45, 120, [&](double lz) { return shading * (1 - 0.45 / (0.45 + 0.55 * lz)); });
        m["refraction"] = -pct((0.5 * (r.n("distortion", 70) / 100) * SIZE) / 0.85);
        return m;
    }

    Json glassMaterial(const Reader& r) {
        Json m = material2D(solidFill("#ffffff"));
        Json bevel = oldBevel(r, 18, 120, 50, 200, [](double) { return 0.0; });
        bevel["shadowOpacity"] = 0;
        m["bevel"] = bevel;
        m["refraction"] = pct(r.n("refraction", 12));
        m["glass"] = Json{{"enabled", true}, {"color", r.s("color", "#3cc8ff")}, {"tint", clamp(r.n("tint", 100) / 100, 0, 1)},
                          {"opacity", clamp(r.n("opacity", 70) / 100, 0, 1)}, {"edge", clamp(r.n("edge", 70) / 100, 0, 1)},
                          {"caustic", clamp(r.n("caustic", 70) / 100, 0, 2)}};
        return m;
    }

    Json metalMaterial(const Reader& r) {
        const std::string mode = r.s("mode", "brushed");
        const double grain = std::max(0.25, r.n("scale", 3)) * (mode == "hairline" ? 0.45 : 1);
        static const std::map<std::string, std::string> kinds = {{"brushed", "hairline"}, {"hairline", "hairline"}, {"radial", "spun"}, {"flake", "flakes"}};
        Json m = material2D(solidFill("#ffffff"));
        auto kind = kinds.find(mode);
        m["texture"]["kind"] = kind == kinds.end() ? "hairline" : kind->second;
        m["texture"]["amount"] = clamp(r.n("strength", 30) / 100, 0, 1);
        m["texture"]["scale"] = pct(grain);
        m["texture"]["blend"] = "overlay";
        m["texture"]["target"] = "color";
        m["texture"]["angle"] = r.n("angle", 0);
        m["sheen"] = Json{{"enabled", r.b("sheen", true)}, {"color", r.s("sheenColor", "#ffffff")}, {"amount", clamp(r.n("sheenAmount", 70) / 100, 0, 3)},
                          {"position", clamp(r.n("sheenPosition", -25) / 100, -1, 1)}, {"width", clamp(r.n("sheenWidth", 35) / 100, 0.05, 2)}};
        return m;
    }

    const std::set<std::string>& materialRemoved() {
        static const std::set<std::string> removed = {"bevel", "chrome", "glass", "metalTexture"};
        return removed;
    }

    Json replaceMaterial(const Json& fx) {
        const std::string type = str(fx, "type");
        if (!materialRemoved().count(type)) return fx;
        const Json p = paramsOf(fx);
        const Reader r{p};
        const Json material = type == "bevel" ? bevelMaterial(r) : type == "chrome" ? chromeMaterial(r, p) : type == "glass" ? glassMaterial(r) : metalMaterial(r);
        return Json{{"id", fx.value("id", Json(""))}, {"type", "material"}, {"enabled", fx.value("enabled", Json(true))},
                    {"params", Json{{"colorMode", type == "chrome" ? "fill" : "layer"}, {"size", SIZE}, {"material", material}}}};
    }

    // a project step over every layer's and part's effect stack
    template<class Fn>
    Json eachStack(const Json& project, Fn&& fn) {
        Json out = project;
        Json layers = Json::array();
        for (Json l : project.value("layers", Json::array())) {
            l["effects"] = fn(l.value("effects", Json::array()));
            if (l.contains("partEffects") && l["partEffects"].is_object()) {
                Json parts = Json::object();
                for (auto it = l["partEffects"].begin(); it != l["partEffects"].end(); ++it) parts[it.key()] = it->is_array() ? fn(*it) : *it;
                l["partEffects"] = parts;
            }
            layers.push_back(std::move(l));
        }
        out["layers"] = layers;
        return out;
    }

    // ---- step 4: content effects ----

    struct FromEffectLayer {
        std::string type;
        Json params;
        std::string place;      // above | below
        std::string blend;      // "" = normal
        double opacity = 1;
        bool hasOpacity = false;
        Json effects = Json::array();
    };

    Json withDefaults(Json defaults, const Json& p) { return filled(std::move(defaults), p); }

    const std::set<std::string>& contentTypes() {
        static const std::set<std::string> types = {"sparkles", "starGlint", "burstRays", "particleBurst", "fire", "smoke", "corona", "lightning",
                                                    "inkSplatter", "backlight", "lensFlare"};
        return types;
    }

    bool isContent(const Json& fx) { return contentTypes().count(str(fx, "type")) > 0; }

    std::vector<FromEffectLayer> fromEffect(const Json& fx) {
        const std::string type = str(fx, "type");
        const Json p = paramsOf(fx);
        std::vector<FromEffectLayer> out;
        if (type == "sparkles") {
            const Json sparkles = Json{{"placement", "highlights"}, {"count", 40}, {"large", 5}, {"small", 6}, {"heroWidth", 80}, {"heroThickness", 100},
                                       {"heroCross", 22}, {"heroAngle", 0}, {"heroStars", 3}, {"size", 50}, {"sizeVariation", 60}, {"threshold", 30},
                                       {"spacing", 80}, {"faceBias", 70}, {"jitter", 50}, {"spread", 60}, {"inside", true}, {"rays", "4"},
                                       {"spectral", false}, {"color", "#fff4c0"}, {"intensity", 130}, {"brightnessVariation", 50}, {"seed", 1}};
            out.push_back({"glints", withDefaults(sparkles, p), "above", "add"});
        } else if (type == "starGlint") {
            const Json o = withDefaults(Json{{"placement", "auto"}, {"count", 5}, {"threshold", 75}, {"x", 0}, {"y", 0}, {"size", 110}, {"rays", "8"},
                                             {"spectral", false}, {"thickness", 100}, {"color", "#fff1b8"}, {"intensity", 130}, {"rotation", 0},
                                             {"variation", 50}},
                                        p);
            Json params = Json{{"placement", o["placement"] == "manual" ? "point" : "peaks"}, {"stars", o["count"]}, {"threshold", o["threshold"]},
                               {"x", o["x"]}, {"y", o["y"]}, {"size", o["size"]}, {"rays", o["rays"]}, {"spectral", o["spectral"]},
                               {"thickness", o["thickness"]}, {"color", o["color"]}, {"intensity", o["intensity"]}, {"rotation", o["rotation"]},
                               {"sizeVariation", o["variation"]}};
            out.push_back({"glints", params, "above", "add"});
        } else if (type == "burstRays") {
            const Json o = withDefaults(Json{{"style", "burst"}, {"placement", "clip"}, {"x", 0}, {"y", 0}, {"count", 28}, {"width", 50},
                                             {"widthVariation", 40}, {"lengthVariation", 60}, {"inner", 240}, {"innerSoftness", 60}, {"rotation", 0},
                                             {"color", "#fff3c4"}, {"alternate", false}, {"color2", "#ff4a1c"}, {"opacity", 70}, {"seed", 1}},
                                        p);
            const std::string style = str(o, "style"), placement = str(o, "placement");
            Json params = Json{{"style", style == "focus" ? "focus" : style == "rings" ? "rings" : "wedges"}, {"centre", "point"},
                               {"onLayer", placement == "behind" ? "all" : placement == "over" ? "over" : "clip"}, {"x", o["x"]}, {"y", o["y"]},
                               {"rays", o["count"]}, {"width", o["width"]}, {"widthVariation", o["widthVariation"]},
                               {"lengthVariation", o["lengthVariation"]}, {"inner", o["inner"]}, {"innerSoftness", o["innerSoftness"]},
                               {"rotation", o["rotation"]}, {"colorA", o["color"]}, {"alternate", o["alternate"]}, {"colorB", o["color2"]},
                               {"opacity", o["opacity"]}, {"seed", o["seed"]}};
            out.push_back({"sunburst", params, placement == "behind" ? "below" : "above", ""});
        } else if (type == "particleBurst") {
            Json o = withDefaults(Json{{"source", "edge"}, {"edgeWidth", 4}, {"direction", "normal"}, {"x", 0}, {"y", 0}, {"angle", 90}, {"distance", 180},
                                       {"distanceRandom", 100}, {"cone", 30}, {"place", "both"}, {"shape", "dot"}, {"glintMix", 5}, {"count", 700},
                                       {"size", 6}, {"sizeRandom", 60}, {"depth", 50}, {"streak", 40}, {"ring", 30}, {"rotation", 20}, {"tumble", 0},
                                       {"sheen", 50}, {"colorMode", "radial"}, {"color", "#ffd166"}, {"opacity", 100}, {"opacityRandom", 40},
                                       {"glow", 10}, {"seed", 1}},
                                  p);
            const std::string place = str(o, "place");
            const Json distance = o["distance"], angle = o["angle"];
            const double glow = num(o, "glow", 0);
            Json rest = o;
            for (const char* k : {"place", "distance", "angle", "glow"}) rest.erase(k);
            Json params = rest;
            params["emitter"] = "layer";
            params["travel"] = distance;
            params["heading"] = angle;
            params["split"] = "all";
            Json effects = glow > 0 ? Json::array({particleGlow(glow)}) : Json::array();
            const std::string shape = str(rest, "shape");
            const bool solid = shape == "confetti" || shape == "petal" || shape == "heart";
            FromEffectLayer front{"particles", params, "above", solid ? "normal" : "add"};
            front.effects = effects;
            FromEffectLayer back{"particles", params, "below", ""};
            back.effects = effects;
            if (place == "front") {
                out.push_back(front);
            } else if (place == "behind") {
                out.push_back(back);
            } else {
                back.params["split"] = "back";
                front.params["split"] = "front";
                out.push_back(back);
                out.push_back(front);
            }
        } else if (type == "fire") {
            Json params = p;
            const bool over = str(p, "mode", "behind") == "around";
            params.erase("mode");
            params["overEdge"] = over;
            out.push_back({"fire", params, over ? "above" : "below", ""});
        } else if (type == "smoke") {
            Json params = p;
            const double opacity = num(p, "opacity", 75);
            params.erase("opacity");
            FromEffectLayer l{"smoke", params, "below", ""};
            l.opacity = opacity / 100;
            l.hasOpacity = true;
            out.push_back(l);
        } else if (type == "corona") {
            Json params = p;
            const bool behind = !(p.contains("behind") && p["behind"] == false);
            params.erase("behind");
            out.push_back(behind ? FromEffectLayer{"corona", params, "below", ""} : FromEffectLayer{"corona", params, "above", "add"});
        } else if (type == "lightning") {
            out.push_back({"lightning", p, "above", "add"});
        } else if (type == "inkSplatter") {
            Json params = p;
            const std::string placement = str(p, "placement", "behind");
            const double opacity = num(p, "opacity", 100);
            params.erase("placement");
            params.erase("opacity");
            FromEffectLayer l{"inkSplatter", params, placement == "over" ? "above" : "below", ""};
            l.opacity = opacity / 100;
            l.hasOpacity = true;
            out.push_back(l);
        } else if (type == "backlight") {
            out.push_back({"backlight", p, "below", ""});
        } else if (type == "lensFlare") {
            Json params = p;
            params["follow"] = "";
            out.push_back({"lensFlare", params, "above", "add"});
        }
        return out;
    }

    std::string generatorLabel(const std::string& type) {
        static const std::map<std::string, std::string> labels = {
            {"glints", "Glints"}, {"sunburst", "Sunburst"}, {"particles", "Particles"}, {"fire", "Fire"}, {"smoke", "Smoke"}, {"corona", "Corona"},
            {"lightning", "Lightning"}, {"inkSplatter", "Ink Splatter"}, {"backlight", "Backlight"}, {"lensFlare", "Lens Flare"}};
        auto it = labels.find(type);
        return it == labels.end() ? type : it->second;
    }

    Json companionLayer(const FromEffectLayer& r, const std::string& follow, IdMint& ids) {
        Json base = layerBase(ids, generatorLabel(r.type));
        base["kind"] = "generator";
        Json params = r.params;
        if (!params.contains("follow")) params["follow"] = follow;
        base["generator"] = Json{{"type", r.type}, {"params", params}};
        base["blend"] = r.blend.empty() ? "normal" : r.blend;
        base["transform"]["opacity"] = r.hasOpacity ? r.opacity : 1.0;
        Json effects = Json::array();
        for (Json fx : r.effects) {
            fx["id"] = ids.next();
            effects.push_back(fx);
        }
        base["effects"] = effects;
        return base;
    }

    // ---- companions.ts: blockOf, ownParent, placeCompanions ----

    int indexOf(const Json& layers, const std::string& id) {
        for (size_t i = 0; i < layers.size(); i++) if (str(layers[i], "id") == id) return (int)i;
        return -1;
    }

    bool hasParent(const Json& l) { return l.contains("parentId") && l["parentId"].is_string(); }
    std::string parentOf(const Json& l) { return hasParent(l) ? l["parentId"].get<std::string>() : std::string(); }

    std::pair<int, int> blockOf(const Json& layers, int i) {
        const Json& l = layers[i];
        const bool hasPid = hasParent(l);
        const std::string pid = parentOf(l);
        std::set<std::string> inside = {str(l, "id")};
        for (bool grew = str(l, "kind") == "group"; grew;) {
            grew = false;
            for (const Json& x : layers) {
                if (hasParent(x) && inside.count(parentOf(x)) && !inside.count(str(x, "id"))) {
                    inside.insert(str(x, "id"));
                    grew = true;
                }
            }
        }
        int from = i;
        while (from > 0 && inside.count(str(layers[from - 1], "id"))) from--;
        int to = i;
        const auto sameParent = [&](const Json& x) { return hasParent(x) == hasPid && parentOf(x) == pid; };
        for (int k = i + 1; k < (int)layers.size();) {
            int m = k;
            while (m < (int)layers.size() && !sameParent(layers[m]) && !(hasPid && str(layers[m], "id") == pid)) m++;
            if (m >= (int)layers.size() || (hasPid && str(layers[m], "id") == pid) || !layers[m].value("clip", false)) break;
            to = m;
            k = m + 1;
        }
        return {from, to};
    }

    const Json* ownParent(const Json& layers, int i, std::pair<int, int> block) {
        const Json& layer = layers[i];
        if (!hasParent(layer)) return nullptr;
        const int p = indexOf(layers, parentOf(layer));
        if (p < 0 || str(layers[p], "kind") != "group") return nullptr;
        const Json& parent = layers[p];
        std::set<std::string> inBlock;
        for (int k = block.first; k <= block.second; k++) inBlock.insert(str(layers[k], "id"));
        for (const Json& l : layers) {
            if (parentOf(l) != str(parent, "id") || !hasParent(l)) continue;
            if (inBlock.count(str(l, "id"))) continue;
            if (str(l, "kind") == "generator" && str(l, "companionOf") == str(layer, "id")) continue;
            return nullptr;
        }
        return &parent;
    }

    struct GroupInit {
        Json effects = Json::array();
        bool fresh = false;
        bool given = false;
    };

    Json placeCompanions(const Json& layers, const std::string& id, const std::vector<Json>& below, const std::vector<Json>& above,
                         const GroupInit& init, IdMint& ids) {
        Json kept = Json::array();
        for (const Json& l : layers) {
            if (!(str(l, "kind") == "generator" && str(l, "companionOf") == id)) kept.push_back(l);
        }
        if (below.empty() && above.empty() && init.effects.empty()) return kept;
        const int i = indexOf(kept, id);
        if (i < 0) return kept;
        const Json layer = kept[i];
        const auto [from, to] = blockOf(kept, i);
        const Json* own = init.fresh ? nullptr : ownParent(kept, i, {from, to});
        std::string groupId;
        Json out = Json::array();
        if (own) {
            groupId = str(*own, "id");
            out = kept;
        } else {
            Json g = layerBase(ids, str(layer, "name"));
            g["kind"] = "group";
            g["collapsed"] = false;
            g["companionGroupOf"] = id;
            if (init.given) g["effects"] = init.effects;
            g["clip"] = layer.value("clip", false);
            g["blend"] = layer.value("blend", Json("normal"));
            g["transform"] = defaultTransform();
            g["transform"]["opacity"] = num(layer.value("transform", Json::object()), "opacity", 1);
            if (hasParent(layer)) g["parentId"] = layer["parentId"];
            groupId = str(g, "id");
            for (int k = 0; k < from; k++) out.push_back(kept[k]);
            for (int k = from; k <= to; k++) {
                Json l = kept[k];
                if (str(l, "id") == id) {
                    l["parentId"] = groupId;
                    l["clip"] = false;
                    l["blend"] = "normal";
                    l["transform"]["opacity"] = 1;
                } else if (hasParent(l) == hasParent(layer) && parentOf(l) == parentOf(layer)) {
                    l["parentId"] = groupId;
                }
                out.push_back(l);
            }
            out.push_back(g);
            for (int k = to + 1; k < (int)kept.size(); k++) out.push_back(kept[k]);
        }
        const auto mark = [&](Json c) {
            c["companionOf"] = id;
            c["parentId"] = groupId;
            c["visible"] = layer.value("visible", true);
            c["guide"] = layer.value("guide", false);
            return c;
        };
        Json result = Json::array();
        for (int k = 0; k < from; k++) result.push_back(out[k]);
        for (const Json& c : below) result.push_back(mark(c));
        for (int k = from; k <= to; k++) result.push_back(out[k]);
        for (const Json& c : above) result.push_back(mark(c));
        for (int k = to + 1; k < (int)out.size(); k++) result.push_back(out[k]);
        return result;
    }

    struct Run {
        std::vector<Json> content;
        Json after = Json::array();
    };

    struct Cut {
        Json first = Json::array();
        std::vector<Run> runs;
    };

    Cut cut(const Json& effects) {
        Cut out;
        for (const Json& fx : effects) {
            if (isContent(fx)) {
                if (!out.runs.empty() && out.runs.back().after.empty()) out.runs.back().content.push_back(fx);
                else out.runs.push_back(Run{{fx}, Json::array()});
            } else if (!out.runs.empty()) {
                out.runs.back().after.push_back(fx);
            } else {
                out.first.push_back(fx);
            }
        }
        return out;
    }

    Json convertLayer(const Json& layersIn, const std::string& id, const Json& partContent, IdMint& ids) {
        const int ownerIndex = indexOf(layersIn, id);
        if (ownerIndex < 0) return layersIn;
        const Json owner = layersIn[ownerIndex];
        Json all = partContent;
        for (const Json& fx : owner.value("effects", Json::array())) all.push_back(fx);
        const Cut c = cut(all);
        if (c.runs.empty()) return layersIn;
        const Json ownerMasks = owner.value("masks", Json::array());
        bool movable = !ownerMasks.empty();
        for (const Json& m : ownerMasks) movable = movable && (!m.value("linked", true) || str(m, "kind") == "layer");
        const Json masks = movable ? ownerMasks : Json::array();
        Json out = layersIn;
        out[ownerIndex]["effects"] = c.first;
        if (!masks.empty()) out[ownerIndex]["masks"] = Json::array();
        std::string target = id;
        for (size_t i = 0; i < c.runs.size(); i++) {
            const Run& run = c.runs[i];
            std::vector<Json> below, above;
            std::set<std::string> hidden;
            for (const Json& fx : run.content) {
                for (const FromEffectLayer& r : fromEffect(fx)) {
                    Json g = companionLayer(r, target, ids);
                    if (!fx.value("enabled", true)) hidden.insert(str(g, "id"));
                    if (r.place == "below") below.insert(below.begin(), g);
                    else above.push_back(g);
                }
            }
            std::set<std::string> before;
            for (const Json& l : out) before.insert(str(l, "id"));
            const bool fresh = !run.after.empty() || i + 1 < c.runs.size();
            GroupInit init;
            if (fresh) {
                init.effects = run.after;
                init.fresh = true;
                init.given = true;
            }
            out = placeCompanions(out, target, below, above, init, ids);
            const int t = indexOf(out, target);
            std::string groupId;
            if (t >= 0 && hasParent(out[t])) {
                const int gi = indexOf(out, parentOf(out[t]));
                if (gi >= 0) groupId = str(out[gi], "id");
            }
            for (Json& l : out) {
                if (str(l, "kind") == "generator" && (hidden.count(str(l, "id")) || str(l, "companionOf") == target)) {
                    l["companionOf"] = id;
                    if (hidden.count(str(l, "id"))) l["visible"] = false;
                } else if (i == 0 && !masks.empty() && !groupId.empty() && str(l, "id") == groupId && !before.count(str(l, "id"))) {
                    l["masks"] = masks;
                }
            }
            if (!groupId.empty()) target = groupId;
        }
        return out;
    }

    Json migrateEmitter(const Json& l) {
        if (str(l, "kind") != "generator") return l;
        const Json gen = l.value("generator", Json::object());
        if (str(gen, "type") != "particles" || !gen.contains("params") || !gen["params"].contains("layer")) return l;
        Json params = gen["params"];
        const Json layer = params["layer"];
        params.erase("layer");
        params["follow"] = layer.is_string() ? layer : Json("");
        params["edgeWidth"] = 6;
        Json out = l;
        out["generator"]["params"] = params;
        return out;
    }

    bool isTitleLayer(const Json& l) {
        const std::string k = str(l, "kind");
        return k == "title2d" || k == "title3d";
    }

}

// ==================================================================== the steps

Json pachislop::generatorParams(const Json& project, IdMint& ids) {
    bool changed = false;
    Json layers = Json::array();
    for (const Json& l : project.value("layers", Json::array())) {
        if (str(l, "kind") != "generator") {
            layers.push_back(l);
            continue;
        }
        auto [layer, below] = migrateGenerator(l, ids, changed);
        if (below.is_object()) layers.push_back(below);
        layers.push_back(layer);
    }
    if (!changed) return project;
    Json out = project;
    out["layers"] = layers;
    return out;
}

Json pachislop::dedupeEffects(const Json& effects, double globalLight, IdMint& ids) {
    bool any = false;
    for (const Json& fx : effects) any = any || dedupeRemoved().count(str(fx, "type"));
    if (!any) return effects;
    Json out = Json::array();
    for (size_t i = 0; i < effects.size(); i++) {
        bool first = true;
        for (size_t k = 0; k < i; k++) first = first && !effects[k].value("enabled", true);
        for (const Json& r : replaceDedupe(effects[i], globalLight, first, ids)) out.push_back(r);
    }
    return out;
}

Json pachislop::effectDedupe(const Json& project, IdMint& ids) {
    const double angle = num(project.value("comp", Json::object()).value("globalLight", Json::object()), "angle", 120);
    return eachStack(project, [&](const Json& effects) { return dedupeEffects(effects, angle, ids); });
}

Json pachislop::migrateMaterialEffects(const Json& effects) {
    bool any = false;
    for (const Json& fx : effects) any = any || materialRemoved().count(str(fx, "type"));
    if (!any) return effects;
    Json out = Json::array();
    for (const Json& fx : effects) out.push_back(replaceMaterial(fx));
    return out;
}

Json pachislop::materialEffects(const Json& project) {
    return eachStack(project, [](const Json& effects) { return migrateMaterialEffects(effects); });
}

Json pachislop::contentEffects(const Json& project, IdMint& ids) {
    Json layers = Json::array();
    for (const Json& l : project.value("layers", Json::array())) layers.push_back(migrateEmitter(l));
    std::vector<std::string> owners;
    for (const Json& l : layers) {
        bool has = false;
        for (const Json& fx : l.value("effects", Json::array())) has = has || isContent(fx);
        if (isTitleLayer(l) && l.contains("partEffects") && l["partEffects"].is_object()) {
            for (auto it = l["partEffects"].begin(); it != l["partEffects"].end(); ++it) {
                if (it->is_array()) for (const Json& fx : *it) has = has || isContent(fx);
            }
        }
        if (has) owners.push_back(str(l, "id"));
    }
    for (const std::string& id : owners) {
        const int index = indexOf(layers, id);
        if (index < 0) continue;
        Json owner = layers[index];
        if (str(owner, "kind") == "adjustment") {
            Json kept = Json::array();
            for (const Json& fx : owner.value("effects", Json::array())) if (!isContent(fx)) kept.push_back(fx);
            layers[index]["effects"] = kept;
            continue;
        }
        Json partContent = Json::array();
        if (isTitleLayer(owner) && owner.contains("partEffects") && owner["partEffects"].is_object()) {
            Json parts = Json::object();
            for (auto it = owner["partEffects"].begin(); it != owner["partEffects"].end(); ++it) {
                Json keep = Json::array();
                if (it->is_array()) {
                    for (const Json& fx : *it) {
                        if (isContent(fx)) partContent.push_back(fx);
                        else keep.push_back(fx);
                    }
                }
                parts[it.key()] = keep;
            }
            if (!partContent.empty()) layers[index]["partEffects"] = parts;
        }
        layers = convertLayer(layers, id, partContent, ids);
    }
    Json out = project;
    out["layers"] = layers;
    return out;
}
