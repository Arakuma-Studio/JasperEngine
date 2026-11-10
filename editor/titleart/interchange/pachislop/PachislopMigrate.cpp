// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/io/project.ts, src/io/migrate/bands.ts, src/model/bands.ts and the
// defaults of src/model/defaults.ts by SafEight, used with the author's permission.

#include "PachislopMigrate.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pachislop;

namespace {

    // ---- defaults (src/model/defaults.ts) ----

    Json curve(std::initializer_list<std::pair<double, double>> pts) {
        Json points = Json::array();
        for (const auto& p : pts) points.push_back(Json{{"x", p.first}, {"y", p.second}, {"corner", false}});
        return Json{{"points", points}};
    }

    Json reflection2D() { return Json{{"enabled", false}, {"map", "highway"}, {"amount", 1}, {"blur", 75}, {"rotation", 0}, {"colorize", 0}}; }
    Json glass2D() { return Json{{"enabled", false}, {"color", "#3cc8ff"}, {"tint", 1}, {"opacity", 0.7}, {"edge", 0.7}, {"caustic", 0.7}}; }
    Json sheen2D() { return Json{{"enabled", false}, {"color", "#ffffff"}, {"amount", 0.7}, {"position", -0.25}, {"width", 0.35}}; }
    Json plateShape() { return Json{{"kind", "contour"}, {"close", 0}, {"corner", 4}, {"slant", 0}}; }
    Json erode2D() { return Json{{"enabled", false}, {"amount", 0.5}, {"edge", 6}, {"scale", 12}, {"angle", 0}, {"seed", 1}}; }
    Json imageTrace() { return Json{{"threshold", 128}, {"smooth", 0.5}, {"despeckle", 0.05}}; }
    Json noWarp() { return Json{{"style", "none"}, {"bend", 0}, {"horizontal", 0}, {"vertical", 0}}; }
    Json noDistort() {
        Json z = Json{{"x", 0}, {"y", 0}};
        Json corners = Json::array({z, z, z, z});
        Json mesh = Json::array();
        for (int i = 0; i < 16; i++) mesh.push_back(z);
        return Json{{"mode", "none"}, {"corners", corners}, {"mesh", mesh}};
    }
    Json noLighting() { return Json{{"environment", Json::object()}}; }
    Json bevel3D(double width, double depth, const std::string& profile = "round") {
        return Json{{"width", width}, {"depth", depth}, {"profile", profile}, {"segments", 6}, {"curve", 0}};
    }
    Json surface3D() { return Json{{"kind", "none"}, {"amount", 0.5}, {"scale", 20}, {"seed", 1}}; }
    Json inflateDefault() { return Json{{"height", 12}, {"profile", curve({{0, 0}, {0.35, 0.8}, {1, 1}})}, {"smoothness", 0}, {"detail", 1}}; }
    Json globalLightDefault() { return Json{{"angle", 120}, {"altitude", 30}}; }
    constexpr int DEFAULT_DPI = 72;

    // ---- equality (src/model/equal.ts sameValue, src/model/links.ts withoutLink) ----

    bool sameValue(const Json& a, const Json& b) {
        return nlohmann::json(a) == nlohmann::json(b);
    }

    Json withoutLink(Json m) {
        if (m.is_object()) m.erase("link");
        return m;
    }

    // ---- 2D ----

    Json migrateBevel2D(const Json& b) { return filled(Json{{"useGlobalLight", false}, {"sizeMode", "fixed"}, {"fit", 70}}, b); }
    Json migrateSatin2D(const Json& s) { return filled(Json{{"useGlobalLight", false}}, s); }

    Json migrateMaterial2D(const Json& m) {
        Json out = filled(Json{{"opacity", 1}, {"refraction", 0}}, m);
        const Json src = m.is_object() ? m : Json::object();
        out["bevel"] = migrateBevel2D(src.value("bevel", Json()));
        out["reflection"] = filled(reflection2D(), src.value("reflection", Json()));
        out["satin"] = migrateSatin2D(src.value("satin", Json()));
        out["texture"] = filled(Json{{"target", "color"}, {"angle", 0}}, src.value("texture", Json()));
        out["gloss"] = filled(Json{{"angle", 0}}, src.value("gloss", Json()));
        out["glass"] = filled(glass2D(), src.value("glass", Json()));
        out["sheen"] = filled(sheen2D(), src.value("sheen", Json()));
        return out;
    }

    Json migrateRim2D(const Json& r) {
        Json out = r;
        out["plate"] = filled(plateShape(), r.value("plate", Json()));
        out["material"] = migrateMaterial2D(r.value("material", Json()));
        return out;
    }

    // ---- 3D lighting (the v8 notes) ----

    Json migrateEnvironment(const Json& e) {
        Json out = filled(Json{{"blur", 0}, {"repeatX", 1}, {"repeatY", 1}}, e);
        if (!out.contains("repeat")) return out;
        const Json repeat = out["repeat"];
        out.erase("repeat");
        if (repeat.is_number() && !(e.is_object() && e.contains("repeatX"))) out["repeatX"] = repeat;
        return out;
    }

    struct LegacyLighting {
        double layerBlur = 0;
    };

    Json legacyLayerLighting(const Json& l) {
        if (!l.is_object()) return noLighting();
        Json out = l;
        out["environment"] = migrateEnvironment(l.value("environment", Json()));
        return out;
    }

    Json legacyPartEnvironment(const Json& e, const LegacyLighting& legacy) {
        if (!e.is_object()) return Json::object();
        Json out = migrateEnvironment(e);
        if (num(out, "blur", 0) <= legacy.layerBlur) out.erase("blur");
        if (out.contains("repeatX") && out["repeatX"] == 1) out.erase("repeatX");
        if (out.contains("repeatY") && out["repeatY"] == 1) out.erase("repeatY");
        return out;
    }

    Json migrateMaterial3D(const Json& m, const LegacyLighting* legacy) {
        Json out = filled(Json{{"gradientAxis", "xy"}, {"gradientBend", 0}, {"glassDepth", 0}, {"environment", Json::object()}}, m);
        if (legacy) out["environment"] = legacyPartEnvironment(m.is_object() ? m.value("environment", Json()) : Json(), *legacy);
        out["surface"] = filled(surface3D(), m.is_object() ? m.value("surface", Json()) : Json());
        return out;
    }

    Json migrateBand3D(const Json& b, const LegacyLighting* legacy) {
        Json out = b;
        out["shape"] = filled(plateShape(), b.value("shape", Json()));
        out["material"] = migrateMaterial3D(b.value("material", Json()), legacy);
        if (b.contains("side") && b["side"].is_object()) out["side"] = migrateMaterial3D(b["side"], legacy);
        return out;
    }

    std::optional<LegacyLighting> legacyParts(const Json& lighting, const double* legacyDocBlur) {
        if (!legacyDocBlur) return std::nullopt;
        LegacyLighting l;
        const Json env = lighting.is_object() ? lighting.value("environment", Json::object()) : Json::object();
        l.layerBlur = env.contains("blur") && env["blur"].is_number() ? env["blur"].get<double>() : *legacyDocBlur;
        return l;
    }

    // ---- bands (src/model/bands.ts) ----

    const double* bevelOutsideShare(const std::string& position) {
        static const double outside = 1, center = 0.5, inside = 0;
        return position == "outside" ? &outside : position == "center" ? &center : &inside;
    }

    double endDrop(const std::string& profile) {
        if (profile == "inset") return -1;
        if (profile == "tube") return 0;
        return 1;
    }

    Json inheritSides(Json s) {
        Json bands = Json::array();
        for (Json b : s["bands"]) {
            if (b.contains("side") && sameValue(b["side"], b["material"])) b.erase("side");
            bands.push_back(std::move(b));
        }
        s["bands"] = bands;
        if (s.contains("side") && (!s["side"].is_object() || sameValue(s["side"], s["face"]))) s.erase("side");
        return s;
    }

    Json charFace(const Json& s, const Json& a) { return a.is_object() && a.contains("face3d") ? a["face3d"] : s["face"]; }
    Json charSideNeighbour(const Json& s, const Json& a) { return s.contains("side") ? s["side"] : charFace(s, a); }

    Json withCharSide(const Json& s, Json a, const Json& m) {
        a.erase("side3d");
        if (m.is_object() && !sameValue(m, charSideNeighbour(s, a))) a["side3d"] = m;
        return a;
    }

    Json migrateChars(const Json& chars, bool is3D, const LegacyLighting* legacy, bool flipTiltY) {
        Json out = Json::object();
        if (!chars.is_object()) return out;
        for (auto it = chars.begin(); it != chars.end(); ++it) {
            Json c = it.value();
            if (!c.is_object()) continue;
            if (!is3D && c.contains("face") && c["face"].is_object()) c["face"] = migrateMaterial2D(c["face"]);
            if (c.contains("face3d") && c["face3d"].is_object()) c["face3d"] = migrateMaterial3D(c["face3d"], legacy);
            if (c.contains("bands3d") && c["bands3d"].is_array()) {
                for (Json& m : c["bands3d"]) if (m.is_object()) m = migrateMaterial3D(m, legacy);
            }
            if (c.contains("side3d") && c["side3d"].is_object()) c["side3d"] = migrateMaterial3D(c["side3d"], legacy);
            if (flipTiltY && c.contains("tiltY") && c["tiltY"].is_number() && c["tiltY"].get<double>() != 0) {
                c["tiltY"] = -c["tiltY"].get<double>();
            }
            out[it.key()] = std::move(c);
        }
        return out;
    }

    // convertPartEffects: part effects keyed by the parts of a converted style
    Json convertPartEffects(const Json& fx, const Json& old, const BandsConversion& conv) {
        const auto keyOf = [&](const Json& to) -> std::string {
            if (to.is_number()) return "band:" + str(conv.style3d["bands"][to.get<size_t>()], "id");
            return to.get<std::string>();
        };
        Json out = Json::object();
        for (auto it = fx.begin(); it != fx.end(); ++it) {
            const std::string key = it.key();
            int ring = -1, plate = -1;
            for (size_t i = 0; i < old["bands"].size(); i++) if (key == "band:" + str(old["bands"][i], "id")) ring = (int)i;
            for (size_t i = 0; i < old["backplates"].size(); i++) if (key == "plate:" + str(old["backplates"][i], "id")) plate = (int)i;
            const std::string to = ring >= 0 ? keyOf(conv.part("band", ring)) : plate >= 0 ? keyOf(conv.part("plate", plate))
                                   : key == "side" ? keyOf(conv.part("side", 0)) : key;
            if (!out.contains(to)) out[to] = Json::array();
            if (it->is_array()) for (const Json& e : *it) out[to].push_back(e);
        }
        return out;
    }

    Json migrateLayer(const Json& l, const double* legacyDocBlur, bool flipTiltY) {
        Json out = l;
        out["effects"] = migrateEffects(l.value("effects", Json::array()));
        out["clip"] = l.value("clip", false);
        out["masks"] = l.contains("masks") && l["masks"].is_array() ? l["masks"] : Json::array();
        Json transform = l.value("transform", Json::object());
        if (!transform.contains("flipX")) transform["flipX"] = false;
        if (!transform.contains("flipY")) transform["flipY"] = false;
        out["transform"] = transform;
        const std::string kind = str(l, "kind");
        if (kind == "background" || kind == "group" || kind == "adjustment" || kind == "generator") return out;
        out["warp"] = filled(noWarp(), l.value("warp", Json()));
        out["distort"] = filled(noDistort(), l.value("distort", Json()));
        out["weight"] = l.contains("weight") ? l["weight"] : Json(0);
        const Json source = l.value("source", Json::object());
        std::optional<Json> filledStyle;
        std::optional<LegacyLighting> charLegacy;
        if (kind == "title3d") {
            filledStyle = migrateStyle3D(l.value("style3d", Json::object()), legacyDocBlur);
            charLegacy = legacyParts((*filledStyle)["lighting"], legacyDocBlur);
        }
        Json chars = str(source, "kind") == "text"
                         ? migrateChars(source.value("chars", Json::object()), kind == "title3d", charLegacy ? &*charLegacy : nullptr, flipTiltY)
                         : Json::object();
        Json partEffects = Json::object();
        if (l.contains("partEffects") && l["partEffects"].is_object()) {
            for (auto it = l["partEffects"].begin(); it != l["partEffects"].end(); ++it) {
                partEffects[it.key()] = migrateEffects(it->is_array() ? *it : Json::array());
            }
        }
        if (filledStyle && isLegacyStyle3D(*filledStyle)) {
            Json charList = Json::array();
            for (auto it = chars.begin(); it != chars.end(); ++it) charList.push_back(it.value());
            const bool orb = str(source, "kind") == "shape" && str(source, "shape") == "orb";
            const BandsConversion conv = convertBands(*filledStyle, orb, charList);
            Json converted = Json::object();
            for (auto it = chars.begin(); it != chars.end(); ++it) converted[it.key()] = conv.charAdjust(it.value());
            chars = converted;
            partEffects = convertPartEffects(partEffects, *filledStyle, conv);
            filledStyle = conv.style3d;
        }
        Json src = source;
        if (str(source, "kind") == "text") {
            if (!src.contains("hScale")) src["hScale"] = 1;
            if (!src.contains("vScale")) src["vScale"] = 1;
            if (!src.contains("tsume")) src["tsume"] = 0;
            if (!src.contains("tateChuYoko")) src["tateChuYoko"] = false;
            src["chars"] = chars;
            if (!src.contains("lines")) src["lines"] = Json::object();
        } else if (str(source, "kind") == "image") {
            src["trace"] = filled(imageTrace(), source.value("trace", Json()));
        }
        out["source"] = src;
        out["partEffects"] = partEffects;
        if (kind == "title2d") {
            out["style"] = migrateStyle2D(l.value("style", Json::object()));
        } else {
            out["style3d"] = *filledStyle;
        }
        return out;
    }

}

bool pachislop::isSupportedVersion(const Json& v) {
    if (!v.is_number_integer() && !(v.is_number() && std::floor(v.get<double>()) == v.get<double>())) return false;
    const double n = v.get<double>();
    return n >= 4 && n <= PROJECT_VERSION;
}

std::string pachislop::unsupportedVersionMessage(const Json& version) {
    const bool old = version.is_number() && (version.get<double>() == 1 || version.get<double>() == 2 || version.get<double>() == 3);
    const std::string shown = version.is_number_integer() ? std::to_string(version.get<int64_t>())
                              : version.is_string() ? version.get<std::string>() : version.dump();
    if (old) {
        return "This is a version " + shown + " project; this version of the app opens version 4 to " + std::to_string(PROJECT_VERSION) +
               " projects only";
    }
    return "Unsupported project version " + shown + " (expected 4 to " + std::to_string(PROJECT_VERSION) + ")";
}

Json pachislop::migrateStyle2D(const Json& style) {
    Json out = style.is_object() ? style : Json::object();
    out["face"] = migrateMaterial2D(out.value("face", Json()));
    Json rims = Json::array();
    if (out.contains("rims") && out["rims"].is_array()) for (const Json& r : out["rims"]) rims.push_back(migrateRim2D(r));
    out["rims"] = rims;
    if (!out.contains("knockout") || out["knockout"].is_null()) out["knockout"] = false;
    out["erode"] = filled(erode2D(), out.value("erode", Json()));
    return out;
}

Json pachislop::migrateStyle3D(const Json& s, const double* legacyDocBlur) {
    const Json lighting = legacyDocBlur ? legacyLayerLighting(s.value("lighting", Json()))
                                        : (s.contains("lighting") ? s["lighting"] : Json());
    const std::optional<LegacyLighting> parts = legacyParts(lighting, legacyDocBlur);
    const LegacyLighting* p = parts ? &*parts : nullptr;
    Json out = s;
    out["mode"] = s.contains("mode") && !s["mode"].is_null() ? s["mode"] : Json("extrude");
    out["inflate"] = filled(inflateDefault(), s.value("inflate", Json()));
    out["envLock"] = s.value("envLock", false);
    out["bevelPosition"] = s.contains("bevelPosition") && !s["bevelPosition"].is_null() ? s["bevelPosition"] : Json("inside");
    out["faceImage"] = s.contains("faceImage") && !s["faceImage"].is_null() ? s["faceImage"] : Json(1);
    out["lighting"] = lighting;
    out["edgeImage"] = s.contains("edgeImage") && !s["edgeImage"].is_null() ? s["edgeImage"] : Json(0);
    out["face"] = migrateMaterial3D(s.value("face", Json()), p);
    if (s.contains("side") && s["side"].is_object()) out["side"] = migrateMaterial3D(s["side"], p);
    Json bands = Json::array();
    if (!isLegacyStyle3D(s)) {
        if (s.contains("bands") && s["bands"].is_array()) for (const Json& b : s["bands"]) bands.push_back(migrateBand3D(b, p));
        out["bands"] = bands;
        return out;
    }
    if (s.contains("bands") && s["bands"].is_array()) {
        for (Json b : s["bands"]) {
            b["material"] = migrateMaterial3D(b.value("material", Json()), p);
            bands.push_back(std::move(b));
        }
    }
    out["bands"] = bands;
    Json plates = Json::array();
    if (s.contains("backplates") && s["backplates"].is_array()) {
        for (Json b : s["backplates"]) {
            b["plate"] = filled(plateShape(), b.value("plate", Json()));
            b["face"] = migrateMaterial3D(b.value("face", Json()), p);
            if (b.contains("side") && b["side"].is_object()) b["side"] = migrateMaterial3D(b["side"], p);
            plates.push_back(std::move(b));
        }
    }
    out["backplates"] = plates;
    return out;
}

// Deep Glow before the light model (two Gaussian blurs with a flat tint) -> the light glow (see
// project.ts migrateDeepGlow for the derivation)
Json pachislop::migrateDeepGlow(const Json& fx) {
    static const char* OLD_KEYS[] = {"innerRadius", "outerRadius", "innerIntensity", "outerIntensity", "tint"};
    const Json params = fx.value("params", Json::object());
    bool old = false;
    for (const char* k : OLD_KEYS) old = old || params.contains(k);
    if (str(fx, "type") != "deepGlow" || !old) return fx;
    const auto n = [&](const char* k, double def) { return num(params, k, def); };
    const double rIn = std::max(1.0, n("innerRadius", 14));
    const double rOut = std::max(rIn * 1.01, n("outerRadius", 240));
    const double iIn = n("innerIntensity", 1);
    const double iOut = n("outerIntensity", 0.7);
    const double p = iIn > 0 && iOut > 0 ? 2 - std::log((3 * iOut) / (1.2 * iIn)) / std::log(rOut / rIn) : iOut > 0 ? 1.6 : 3.2;
    const std::string tint = str(params, "tint", "#ffb43c");
    const double tintAmount = n("tintAmount", 0.6);
    const double t = n("threshold", 0);
    Json next = params;
    for (const char* k : OLD_KEYS) next.erase(k);
    next.erase("exposure");
    next.erase("threshold");
    next["radius"] = std::round(rOut);
    next["falloff"] = std::round(((std::min(3.2, std::max(1.6, p)) - 1.6) / 1.6) * 100);
    next["intensity"] = std::min(8.0, (1.2 * iIn + 3 * iOut) * std::pow(2.0, n("exposure", 0)));
    next["exposure"] = 0;
    next["threshold"] = t <= 0.04045 ? t / 12.92 : std::pow((t + 0.055) / 1.055, 2.4);
    next["knee"] = 0.1;
    if (tintAmount > 0.5) {
        next["colorMode"] = "custom";
        next["color"] = tint;
        next["tintAmount"] = 0;
    } else {
        next["colorMode"] = "layer";
        next["tintAmount"] = tintAmount;
        next["tintInner"] = tint;
        next["tintOuter"] = tint;
    }
    Json out = fx;
    out["params"] = next;
    return out;
}

Json pachislop::migrateEffects(const Json& effects) {
    // the effect types with a `useGlobalLight` param (src/effects/a/shadow.ts, stylize.ts: lit())
    static const std::set<std::string> LIT = {"dropShadow", "innerShadow", "perspectiveShadow", "extrude2d"};
    Json out = Json::array();
    if (!effects.is_array()) return out;
    for (Json fx : effects) {
        if (!fx.is_object()) continue;
        if (!fx.contains("params") || !fx["params"].is_object()) fx["params"] = Json::object();
        const std::string type = str(fx, "type");
        if (LIT.count(type) && !fx["params"].contains("useGlobalLight")) fx["params"]["useGlobalLight"] = false;
        if ((type == "keyline" || type == "contourLines") && !fx["params"].contains("follow")) fx["params"]["follow"] = "stack";
        out.push_back(migrateDeepGlow(fx));
    }
    return out;
}

// ---- the bands conversion ----

bool pachislop::isLegacyStyle3D(const Json& s) {
    return s.is_object() && s.contains("backplates");
}

BandsConversion pachislop::convertBands(const Json& raw, bool orb, const Json& chars) {
    const Json rings = raw.value("bands", Json::array());
    const Json plates = raw.value("backplates", Json::array());
    const Json oldSide = raw.contains("side") ? raw["side"] : Json();
    Json common = raw;
    common.erase("bands");
    common.erase("backplates");
    common.erase("side");
    const int n = (int)rings.size();
    const bool inert = str(raw, "mode") == "inflate" || orb;
    const Json face = raw.value("face", Json::object());
    const Json shownSide = oldSide.is_object() ? oldSide : (n ? rings[n - 1]["material"] : face);
    const auto sameMaterial = [](const Json& a, const Json& b) { return sameValue(withoutLink(a), withoutLink(b)); };
    bool fold = n > 0 && sameMaterial(rings[0]["material"], face);
    if (fold && chars.is_array()) {
        for (const Json& a : chars) {
            const Json ring0 = a.contains("bands3d") && a["bands3d"].is_array() && !a["bands3d"].empty() && a["bands3d"][0].is_object()
                                   ? a["bands3d"][0] : rings[0]["material"];
            if (!sameMaterial(ring0, a.contains("face3d") ? a["face3d"] : face)) fold = false;
        }
    }
    const int start = fold ? 1 : 0;
    const auto width = [](const Json& r) { return std::max(0.0, num(r, "width", 0)); };
    const auto shapeOf = [](const Json& r) {
        return Json{{"width", r.value("width", Json(0))}, {"depth", r.value("depth", Json(0))}, {"profile", r.value("profile", Json("round"))},
                    {"segments", r.value("segments", Json(6))}, {"curve", r.value("curve", Json(0))}};
    };
    const auto dropOf = [&](const Json& b) {
        return (num(b, "width", 0) > 0 || num(b, "depth", 0) > 0) ? std::max(0.0, num(b, "depth", 0)) * endDrop(str(b, "profile", "round")) : 0.0;
    };

    std::vector<double> zEnd;
    for (int k = 0; k < n; k++) zEnd.push_back((k ? zEnd[k - 1] : 0.0) - dropOf(rings[k]));
    const double zLast = n ? zEnd[n - 1] : 0;
    const double depth = std::max(0.0, num(raw, "depth", 0));
    const auto backTo = [&](double top) { return inert ? depth : std::max(0.0, top - zLast + depth); };

    double total = 0;
    for (const Json& r : rings) total += width(r);
    const std::string rawPosition = str(raw, "bevelPosition", "inside");
    const double oldReach = total * *bevelOutsideShare(rawPosition);
    std::string bevelPosition = rawPosition;
    if (fold && !inert && width(rings[0]) > 0) {
        const auto reach = [&](const std::string& pos) { return width(rings[0]) * *bevelOutsideShare(pos) + total - width(rings[0]); };
        std::string best = bevelPosition;
        for (const char* pos : {"outside", "center", "inside"}) {
            if (std::abs(reach(pos) - oldReach) < std::abs(reach(best) - oldReach)) best = pos;
        }
        bevelPosition = best;
    }
    const Json bevel = fold ? shapeOf(rings[0]) : bevel3D(0, 0);
    const double letterDepth = backTo(fold ? zEnd[0] : 0);

    Json converted = Json::array();
    double prevDepth = letterDepth;
    for (int k = start; k < n; k++) {
        const Json& r = rings[k];
        Json band = Json{{"id", r.value("id", Json(""))}, {"enabled", !inert}, {"grow", width(r)}, {"join", "miter"}, {"fillCounters", false},
                         {"shape", plateShape()}, {"depth", backTo(zEnd[k])}, {"offset", -prevDepth}, {"bevel", shapeOf(r)},
                         {"material", r.value("material", Json::object())}};
        prevDepth = band["depth"].get<double>();
        converted.push_back(std::move(band));
    }
    const bool wallOnBand = !inert && !converted.empty();
    if (wallOnBand) converted.back()["side"] = shownSide;

    double newReach = 0;
    if (!inert) {
        newReach = fold ? width(rings[0]) * *bevelOutsideShare(bevelPosition) : 0;
        for (const Json& b : converted) newReach += num(b, "grow", 0);
    }
    double excess = inert || std::abs(newReach - oldReach) < 1e-9 ? 0 : newReach - oldReach;
    Json plateBands = Json::array();
    for (const Json& p : plates) {
        double grow = num(p, "grow", 0);
        if (boolean(p, "enabled", true) && excess != 0) {
            const double take = std::min(std::max(0.0, grow), excess);
            grow -= take;
            excess -= take;
        }
        const Json pb = p.value("bevel", bevel3D(0, 0));
        const bool flatBevel = width(pb) <= 0;
        Json band = Json{{"id", p.value("id", Json(""))}, {"enabled", p.value("enabled", Json(true))}, {"grow", grow},
                         {"join", p.value("join", Json("miter"))}, {"fillCounters", p.value("fillCounters", Json(false))},
                         {"shape", p.value("plate", plateShape())},
                         {"depth", flatBevel ? std::max(0.0, num(p, "depth", 0) + dropOf(pb)) : num(p, "depth", 0)},
                         {"offset", p.value("zGap", Json(0))}, {"bevel", flatBevel ? bevel3D(0, 0, str(pb, "profile", "round")) : pb},
                         {"material", p.value("face", Json::object())}};
        if (p.contains("side") && p["side"].is_object()) band["side"] = p["side"];
        plateBands.push_back(std::move(band));
    }

    Json style = common;
    style["depth"] = letterDepth;
    style["bevel"] = bevel;
    style["bevelPosition"] = bevelPosition;
    if (!wallOnBand) style["side"] = shownSide;
    Json allBands = converted;
    for (const Json& b : plateBands) allBands.push_back(b);
    style["bands"] = allBands;
    style = inheritSides(style);

    BandsConversion out;
    out.style3d = style;
    const int convertedCount = (int)converted.size();
    out.part = [=](const std::string& kind, int index) -> Json {
        if (kind == "side") return wallOnBand ? Json(convertedCount - 1) : Json("side");
        if (kind == "plate") return Json(convertedCount + index);
        return fold && index == 0 ? Json("face") : Json(index - start);
    };
    out.charAdjust = [=](const Json& a) -> Json {
        Json rest = a;
        const Json oldBands = a.contains("bands3d") ? a["bands3d"] : Json();
        rest.erase("bands3d");
        rest.erase("side3d");
        const auto oldBand = [&](int i) { return oldBands.is_array() && i < (int)oldBands.size() ? oldBands[i] : Json(); };
        Json shown;
        if (a.contains("side3d") && a["side3d"].is_object()) shown = a["side3d"];
        else if (oldSide.is_object()) shown = oldSide;
        else if (n) shown = oldBand(n - 1).is_object() ? oldBand(n - 1) : rings[n - 1]["material"];
        else shown = a.contains("face3d") ? a["face3d"] : face;
        Json bands3d = Json::array();
        for (int j = 0; j < convertedCount; j++) bands3d.push_back(oldBand(start + j).is_object() ? oldBand(start + j) : Json());
        while (!bands3d.empty() && bands3d.back().is_null()) bands3d.erase(bands3d.size() - 1);
        if (!bands3d.empty()) rest["bands3d"] = bands3d;
        return wallOnBand ? rest : withCharSide(style, rest, shown);
    };
    return out;
}

// ---- migrateProject ----

bool pachislop::migrateProject(const Json& json, Json& out, std::string& error) {
    if (!json.is_object() || !json.contains("layers") || !json["layers"].is_array() || !json.contains("comp") || !json["comp"].is_object()) {
        error = "Not a Pachislop project";
        return false;
    }
    const Json version = json.value("version", Json());
    if (!isSupportedVersion(version)) {
        error = unsupportedVersionMessage(version);
        return false;
    }
    const int v = (int)version.get<double>();
    IdMint ids;
    Json comp = json["comp"];
    const Json environment = migrateEnvironment(comp.value("environment", Json()));
    const double docBlur = num(environment, "blur", 0);
    const double* legacy = v < 8 ? &docBlur : nullptr;
    const bool flipTiltY = v < 9;
    Json project = json;
    project["version"] = PROJECT_VERSION;
    comp["globalLight"] = filled(globalLightDefault(), comp.value("globalLight", Json()));
    if (!comp.contains("dpi") || comp["dpi"].is_null()) comp["dpi"] = DEFAULT_DPI;
    comp["environment"] = environment;
    project["comp"] = comp;
    Json layers = Json::array();
    for (const Json& l : json["layers"]) {
        if (l.is_object()) layers.push_back(migrateLayer(l, legacy, flipTiltY));
    }
    project["layers"] = layers;
    // the steps of src/io/migrate/, in order
    out = contentEffects(materialEffects(effectDedupe(generatorParams(project, ids), ids)), ids);
    return true;
}
