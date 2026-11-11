// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The material presets, their ramps and the lighting inheritance are Pachislop's
// (src/model/defaults.ts, src/model/lighting.ts) by SafEight, used with the author's permission,
// renamed snake_case (docs/specs/S86-title-3d.md §6.1).

#include "Title3DStyle.h"

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"

#include <algorithm>
#include <cmath>
#include <map>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    const Json& member(const Json& object, const char* key) {
        static const Json none;
        if (!object.is_object()) return none;
        auto it = object.find(key);
        return it != object.end() ? *it : none;
    }
    double num(const Json& object, const char* key, double fallback) {
        const Json& v = member(object, key);
        return v.is_number() ? v.get<double>() : fallback;
    }
    bool flag(const Json& object, const char* key, bool fallback) {
        const Json& v = member(object, key);
        return v.is_boolean() ? v.get<bool>() : fallback;
    }
    std::string str(const Json& object, const char* key, const char* fallback) {
        const Json& v = member(object, key);
        return v.is_string() ? v.get<std::string>() : std::string(fallback);
    }
    TitleColor color(const Json& object, const char* key, TitleColor fallback) {
        TitleColor c = fallback;
        const Json& v = member(object, key);
        if (!v.is_null()) parseTitleColor(v, c);
        return c;
    }
    std::array<double, 2> vec2(const Json& object, const char* key, std::array<double, 2> fallback) {
        const Json& v = member(object, key);
        if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number()) return fallback;
        return {v[0].get<double>(), v[1].get<double>()};
    }

    Json ramp(std::initializer_list<std::pair<double, const char*>> stops) {
        Json list = Json::array();
        for (const auto& s : stops) list.push_back(Json{{"pos", s.first}, {"color", s.second}, {"alpha", 1}, {"mid", 0.5}});
        return Json{{"stops", list}, {"interp", "rgb"}};
    }

    // Pachislop's metal bandings (top -> bottom)
    Json goldBanding() {
        return ramp({{0, "#fff4b0"}, {0.18, "#ffd400"}, {0.42, "#ff9800"}, {0.5, "#b83000"}, {0.58, "#ff7c00"}, {0.8, "#ffb800"}, {1, "#ffe070"}});
    }
    Json polishedGold() {
        return ramp({{0, "#fff8d8"}, {0.06, "#ffe890"}, {0.16, "#ffc838"}, {0.3, "#f0a018"}, {0.4, "#c06a06"}, {0.47, "#5a2602"},
                     {0.51, "#1a0800"}, {0.55, "#120500"}, {0.6, "#5a2804"}, {0.72, "#c87810"}, {0.84, "#f8b830"}, {0.94, "#ffe080"},
                     {1, "#fff4c8"}});
    }
    Json rainbowBanding() {
        return ramp({{0, "#ff7080"}, {0.1, "#ff2a3a"}, {0.22, "#ff7a00"}, {0.34, "#ffd400"}, {0.44, "#30e040"}, {0.5, "#0a8a3a"},
                     {0.53, "#003a5a"}, {0.57, "#0070c0"}, {0.66, "#20b8ff"}, {0.82, "#3040ff"}, {0.94, "#9020ff"}, {1, "#e0a0ff"}});
    }
    // the seven-stop banding of the coloured metals: top, upper, mid, horizon, lower mid, lower, bottom
    Json band7(const char* a, const char* b, const char* c, const char* d, const char* e, const char* f, const char* g) {
        return ramp({{0, a}, {0.22, b}, {0.45, c}, {0.52, d}, {0.6, e}, {0.8, f}, {1, g}});
    }

    const std::map<std::string, Json>& presetTable() {
        static const std::map<std::string, Json> table = [] {
            std::map<std::string, Json> t;
            // polished gold: a near-mirror whose banding bends with the bevels; no clearcoat (an
            // untinted coat lays a pale, unsaturated reflection over the metal)
            t["gold"] = {{"color", "#ffc23a"}, {"metallic", 1}, {"roughness", 0.06}, {"clearcoat", 0}, {"gradient", polishedGold()},
                         {"gradient_span", "line"}, {"gradient_bend", 0.6}};
            t["punchy_gold"] = {{"color", "#ffc000"}, {"metallic", 1}, {"roughness", 0.2}, {"env_multiplier", 2}, {"specular_multiplier", 2},
                                {"clearcoat", 0}, {"gradient", goldBanding()}};
            t["pale_gold"] = {{"color", "#f3dc9a"}, {"metallic", 1}, {"roughness", 0.1}, {"clearcoat", 0.3},
                              {"gradient", band7("#fffef5", "#fbeec0", "#d2b26a", "#7d6430", "#c9a85f", "#f7e7b0", "#fffbe8")}};
            t["rose_gold"] = {{"color", "#ffb59a"}, {"metallic", 1}, {"roughness", 0.1}, {"clearcoat", 0.3},
                              {"gradient", band7("#fff5f0", "#ffd0bd", "#d98a6e", "#7d3f2c", "#c97c62", "#ffcbb6", "#fff0ea")}};
            t["chrome"] = {{"color", "#ffffff"}, {"metallic", 1}, {"roughness", 0.04},
                           {"gradient", band7("#ffffff", "#e8eef5", "#8a94a3", "#2e343d", "#7d8794", "#dde4ee", "#ffffff")}};
            t["silver"] = {{"color", "#d8dde6"}, {"metallic", 1}, {"roughness", 0.14}, {"clearcoat", 0.3},
                           {"gradient", band7("#ffffff", "#e3e7ee", "#a3abb8", "#555c68", "#9aa2af", "#dfe4eb", "#f7f9fc")}};
            t["copper"] = {{"color", "#ff8a52"}, {"metallic", 1}, {"roughness", 0.12}, {"clearcoat", 0.3},
                           {"gradient", band7("#fff0e0", "#ffb48a", "#d0652a", "#6e2a0c", "#c05a24", "#ffa77a", "#ffe6d6")}};
            t["red_metal"] = {{"color", "#ff1d2d"}, {"metallic", 0.9}, {"roughness", 0.15}, {"clearcoat", 1},
                              {"gradient", band7("#ff8a8a", "#ff3b3b", "#d0101c", "#5a0008", "#b80c18", "#ff3030", "#ff9a9a")}};
            t["blue_metal"] = {{"color", "#2a6bff"}, {"metallic", 0.9}, {"roughness", 0.15}, {"clearcoat", 1},
                               {"gradient", band7("#8cc4ff", "#3d8bff", "#1048d0", "#03154f", "#0e3fb8", "#3a85ff", "#86beff")}};
            t["green_metal"] = {{"color", "#18d63a"}, {"metallic", 0.9}, {"roughness", 0.15}, {"clearcoat", 1},
                                {"gradient", band7("#96ffa2", "#38f052", "#10b02a", "#03420c", "#0e9a24", "#34e84e", "#90ff9c")}};
            t["purple_metal"] = {{"color", "#a43aff"}, {"metallic", 0.9}, {"roughness", 0.15}, {"clearcoat", 1},
                                 {"gradient", band7("#dcaeff", "#ad55ff", "#7d1ed0", "#2c0552", "#6a18b8", "#a84cff", "#d6a6ff")}};
            t["black_gloss"] = {{"color", "#050505"}, {"metallic", 0.2}, {"roughness", 0.08}, {"clearcoat", 1}};
            t["white_gloss"] = {{"color", "#ffffff"}, {"metallic", 0}, {"roughness", 0.15}, {"clearcoat", 1}};
            // candy metal: the spectrum as the tint of a metallic base under a thin coat, with a
            // small self-lit floor in the hues' own colours
            t["rainbow"] = {{"color", "#ffffff"}, {"metallic", 0.8}, {"roughness", 0.2}, {"env_multiplier", 0.55},
                            {"specular_multiplier", 1.4}, {"clearcoat", 0.4}, {"emission", "#ffffff"}, {"emission_intensity", 0.08},
                            {"gradient_span", "line"}, {"gradient", rainbowBanding()}};
            t["glass"] = {{"color", "#d4f2ff"}, {"metallic", 0}, {"roughness", 0.02}, {"opacity", 0.22}, {"clearcoat", 1}};
            t["plastic"] = {{"color", "#ff3b3b"}, {"metallic", 0}, {"roughness", 0.35}, {"clearcoat", 0.8}};
            t["emissive"] = {{"color", "#000000"}, {"metallic", 0}, {"roughness", 0.5}, {"emission", "#ffe066"}, {"emission_intensity", 3}};
            t["candy"] = {{"color", "#e0102a"}, {"metallic", 0.85}, {"roughness", 0.35}, {"clearcoat", 1}, {"flakes", 0.8},
                          {"flake_scale", 0.8}, {"env_multiplier", 1.4}, {"specular_multiplier", 1.5}};
            // gems: flakes act as facets on flat faces
            t["diamond"] = {{"color", "#f4f9ff"}, {"metallic", 0}, {"roughness", 0.02}, {"transmission", 1}, {"ior", 2.333},
                            {"dispersion", 0.8}, {"env_multiplier", 1.6}, {"specular_multiplier", 2}, {"flakes", 0.5}, {"flake_scale", 2}};
            t["ruby"] = {{"color", "#ff1030"}, {"metallic", 0}, {"roughness", 0.03}, {"transmission", 1}, {"ior", 1.77}, {"dispersion", 0.3},
                         {"env_multiplier", 1.5}, {"specular_multiplier", 2}};
            t["sapphire"] = {{"color", "#1650ff"}, {"metallic", 0}, {"roughness", 0.03}, {"transmission", 1}, {"ior", 1.77},
                             {"dispersion", 0.3}, {"env_multiplier", 1.5}, {"specular_multiplier", 2}};
            t["emerald"] = {{"color", "#10c050"}, {"metallic", 0}, {"roughness", 0.04}, {"transmission", 1}, {"ior", 1.58},
                            {"dispersion", 0.2}, {"env_multiplier", 1.4}, {"specular_multiplier", 2}};
            return t;
        }();
        return table;
    }

    Json normalized(Json object, const char* kind) {
        NormalizeContext context;
        std::vector<TitleWarning> warnings;
        context.warnings = &warnings;
        normalizeTitleArtObject(object, kind, "", context);
        return object;
    }

    const char* const ENV_KEYS[] = {"preset", "intensity", "rotation", "tint", "contrast", "saturation", "blur", "repeat_x", "repeat_y"};

    // `base` with the values `overrides` sets (the preset carries its HDRI and matcap with it)
    Environment3D overrideEnvironment(const Environment3D& base, const Json& overrides, LightingLevel level,
                                      std::vector<std::pair<std::string, LightingLevel>>* sources) {
        Environment3D out = base;
        if (!overrides.is_object()) return out;
        for (const char* key : ENV_KEYS) {
            if (!overrides.contains(key)) continue;
            if (sources) {
                for (auto& s : *sources) {
                    if (s.first == key) s.second = level;
                }
            }
            const std::string k = key;
            if (k == "preset") {
                out.preset = str(overrides, "preset", "studio");
                out.hdri = decodeAssetRef(member(overrides, "hdri"));
                out.matcap = decodeAssetRef(member(overrides, "matcap"));
            } else if (k == "intensity") {
                out.intensity = num(overrides, key, out.intensity);
            } else if (k == "rotation") {
                out.rotation = num(overrides, key, out.rotation);
            } else if (k == "tint") {
                out.tint = color(overrides, key, out.tint);
            } else if (k == "contrast") {
                out.contrast = num(overrides, key, out.contrast);
            } else if (k == "saturation") {
                out.saturation = num(overrides, key, out.saturation);
            } else if (k == "blur") {
                out.blur = num(overrides, key, out.blur);
            } else if (k == "repeat_x") {
                out.repeatX = num(overrides, key, out.repeatX);
            } else {
                out.repeatY = num(overrides, key, out.repeatY);
            }
        }
        return out;
    }

    GradientKind3D gradientKindOf(const std::string& s) {
        return s == "radial" ? GradientKind3D::Radial : s == "reflected" ? GradientKind3D::Reflected : GradientKind3D::Linear;
    }
    GradientSpan3D spanOf(const std::string& s) {
        return s == "line" ? GradientSpan3D::Line : s == "char" ? GradientSpan3D::Char : s == "part" ? GradientSpan3D::Part : GradientSpan3D::Layer;
    }

    // a 2D fill as the colour members of a 3D material (R2)
    void fillToMaterial(const Json& fill, Json& material) {
        const std::string kind = str(fill, "kind", "solid");
        material["color"] = str(fill, "color", "#ffffff");
        if (kind == "linear" || kind == "radial" || kind == "reflected" || kind == "angle" || kind == "diamond" || kind == "shape_burst") {
            material["gradient"] = member(fill, "gradient");
            material["gradient_kind"] = kind == "radial" || kind == "shape_burst" ? "radial" : kind == "reflected" ? "reflected" : "linear";
            material["gradient_angle"] = num(fill, "angle", 90);
            material["gradient_span"] = str(fill, "span", "layer");
            material["gradient_scale"] = num(fill, "scale", 1);
            material["gradient_offset"] = num(fill, "offset", 0);
            // a gradient's colour is white so the ramp is the colour
            material["color"] = "#ffffff";
        } else if (kind == "image" || kind == "source") {
            material["color"] = "#ffffff";
        }
    }

    Json derivedMaterial(const Json& material2d) {
        Json m = Json::object();
        m["preset"] = "custom";
        fillToMaterial(member(material2d, "fill"), m);
        m["metallic"] = 0;
        m["roughness"] = 0.35;
        m["clearcoat"] = 0.5;
        return m;
    }

    // a colour at half its HSV value
    std::string halfValue(const TitleColor& c) {
        TitleColor out{c.r * 0.5f, c.g * 0.5f, c.b * 0.5f};
        return formatTitleColor(out);
    }

}

const char* titleart::lightingLevelName(LightingLevel level) {
    switch (level) {
        case LightingLevel::Layer: return "layer";
        case LightingLevel::Part: return "part";
        default: return "document";
    }
}

ToneMapping3D titleart::toneMappingFromName(const std::string& name) {
    if (name == "none") return ToneMapping3D::None;
    if (name == "aces_filmic") return ToneMapping3D::AcesFilmic;
    if (name == "agx") return ToneMapping3D::AgX;
    if (name == "punch") return ToneMapping3D::Punch;
    return ToneMapping3D::Neutral;
}

const char* titleart::toneMappingName(ToneMapping3D tone) {
    switch (tone) {
        case ToneMapping3D::None: return "none";
        case ToneMapping3D::AcesFilmic: return "aces_filmic";
        case ToneMapping3D::AgX: return "agx";
        case ToneMapping3D::Punch: return "punch";
        default: return "neutral";
    }
}

bool TitleMaterial3D::hasPattern() const {
    // Pachislop's patternActive: a kind, some ink or relief, and a custom kind's image
    if (!pattern.is_object()) return false;
    const std::string kind = pattern.value("kind", std::string("none"));
    if (kind == "none") return false;
    if (!(pattern.value("opacity", 1.0) > 0 || pattern.value("relief", 0.0) > 0)) return false;
    return kind != "custom" || !decodeAssetRef(pattern.contains("image") ? pattern["image"] : Json()).empty();
}

TitleMaterial3D titleart::decodeMaterial3D(const Json& m) {
    TitleMaterial3D out;
    if (!m.is_object()) return out;
    out.preset = str(m, "preset", "custom");
    out.color = color(m, "color", out.color);
    const Json& gradient = member(m, "gradient");
    if (gradient.is_object()) out.gradient = decodeGradient(gradient);
    out.gradientKind = gradientKindOf(str(m, "gradient_kind", "linear"));
    out.gradientAngle = num(m, "gradient_angle", 90);
    out.gradientSpan = spanOf(str(m, "gradient_span", "layer"));
    out.gradientScale = num(m, "gradient_scale", 1);
    out.gradientOffset = num(m, "gradient_offset", 0);
    out.gradientDepth = str(m, "gradient_axis", "xy") == "depth";
    out.gradientBend = num(m, "gradient_bend", 0);
    out.metallic = num(m, "metallic", 0);
    out.roughness = num(m, "roughness", 0.3);
    out.envMultiplier = num(m, "env_multiplier", 1);
    out.specularMultiplier = num(m, "specular_multiplier", 1);
    out.clearcoat = num(m, "clearcoat", 0);
    out.clearcoatRoughness = num(m, "clearcoat_roughness", 0.04);
    out.iridescence = num(m, "iridescence", 0);
    out.iridescenceIor = num(m, "iridescence_ior", 1.6);
    out.iridescenceThicknessNm = vec2(m, "iridescence_thickness_nm", out.iridescenceThicknessNm);
    out.transmission = num(m, "transmission", 0);
    out.ior = num(m, "ior", 1.5);
    out.dispersion = num(m, "dispersion", 0);
    out.glassDepth = num(m, "glass_depth", 0);
    out.flakes = num(m, "flakes", 0);
    out.flakeScale = num(m, "flake_scale", 0.6);
    out.pattern = member(m, "pattern");
    const Json& surface = member(m, "surface");
    const std::string sk = str(surface, "kind", "none");
    out.surface = sk == "crumple" ? SurfaceKind3D::Crumple : sk == "hammered" ? SurfaceKind3D::Hammered : sk == "liquid" ? SurfaceKind3D::Liquid : SurfaceKind3D::None;
    out.surfaceAmount = num(surface, "amount", 0.5);
    out.surfaceScale = num(surface, "scale", 20);
    out.surfaceSeed = (int64_t)num(surface, "seed", 1);
    const Json& base = member(m, "base_texture");
    out.baseTexture = decodeAssetRef(member(base, "texture"));
    out.baseTiling = vec2(base, "tiling", out.baseTiling);
    out.baseOffset = vec2(base, "offset", out.baseOffset);
    out.baseWrap = str(base, "projection", "front") == "wrap";
    out.emission = color(m, "emission", out.emission);
    out.emissionIntensity = num(m, "emission_intensity", 0);
    out.opacity = num(m, "opacity", 1);
    const Json& env = member(m, "environment");
    out.environment = env.is_object() ? env : Json::object();
    out.link = str(m, "link", "");
    return out;
}

Bevel3D titleart::decodeBevel3D(const Json& b) {
    Bevel3D out;
    out.width = num(b, "width", 0);
    out.depth = num(b, "depth", 0);
    three_d::BevelProfileKind kind = three_d::BevelProfileKind::Round;
    three_d::bevelProfileFromName(str(b, "profile", "round"), kind);
    out.shape.profile = kind;
    out.shape.segments = (int)num(b, "segments", 6);
    out.shape.curve = num(b, "curve", 0);
    out.shape.custom = decodeCurve(member(b, "custom"));
    return out;
}

Environment3D titleart::decodeEnvironment3D(const Json& e) {
    Environment3D out;
    out.preset = str(e, "preset", "studio");
    out.hdri = decodeAssetRef(member(e, "hdri"));
    out.matcap = decodeAssetRef(member(e, "matcap"));
    out.intensity = num(e, "intensity", 1);
    out.rotation = num(e, "rotation", 0);
    out.tint = color(e, "tint", out.tint);
    out.contrast = num(e, "contrast", 1);
    out.saturation = num(e, "saturation", 1);
    out.blur = num(e, "blur", 0);
    out.repeatX = num(e, "repeat_x", 1);
    out.repeatY = num(e, "repeat_y", 1);
    return out;
}

Camera3D titleart::decodeCamera3D(const Json& c) {
    Camera3D out;
    out.fov = num(c, "fov", 30);
    out.x = num(c, "x", 0);
    out.y = num(c, "y", 0);
    out.zOffset = num(c, "z_offset", 0);
    out.rotationX = num(c, "rotation_x", 0);
    out.rotationY = num(c, "rotation_y", 0);
    out.rotation = num(c, "rotation", 0);
    return out;
}

std::vector<Light3D> titleart::decodeLights3D(const Json& lights) {
    std::vector<Light3D> out;
    if (!lights.is_array()) return out;
    for (const Json& l : lights) {
        Light3D light;
        const std::string type = str(l, "type", "directional");
        light.type = type == "point" ? LightType3D::Point : type == "ambient" ? LightType3D::Ambient : LightType3D::Directional;
        light.color = color(l, "color", light.color);
        light.intensity = num(l, "intensity", 2);
        const Json& p = member(l, "position");
        if (p.is_array() && p.size() == 3) {
            for (size_t i = 0; i < 3; i++) light.position[i] = p[i].is_number() ? p[i].get<double>() : 0.0;
        }
        out.push_back(light);
    }
    return out;
}

Json titleart::plainStyle3D() {
    return normalized(Json::object(), "style_3d");
}

Json titleart::deriveStyle3D(const Json& layer) {
    const Json& style = member(layer, "style");
    // a 2D style at S80's default gets the plain 3D style
    Json defaultLayer = Json{{"type", member(layer, "type")}};
    if (member(layer, "type").is_string() && member(layer, "type").get<std::string>() == "vector") defaultLayer["mode"] = member(layer, "mode");
    defaultLayer = normalized(defaultLayer, "layer");
    if (member(defaultLayer, "style") == style) return plainStyle3D();

    Json out = Json::object();
    const Json& face2d = member(style, "face");
    Json face = derivedMaterial(face2d);
    out["face"] = face;
    // the side is the face colour at half its value
    TitleColor faceColor{1, 1, 1};
    parseTitleColor(member(member(face2d, "fill"), "color"), faceColor);
    Json side = Json{{"preset", "custom"}, {"color", halfValue(faceColor)}, {"metallic", 0}, {"roughness", 0.35}, {"clearcoat", 0.5}};
    out["side"] = side;
    Json bands = Json::array();
    const Json& rims = member(style, "rims");
    if (rims.is_array()) {
        for (const Json& rim : rims) {
            if (!flag(rim, "enabled", true) || !(num(rim, "width", 0) > 0)) continue;
            Json band = Json::object();
            band["grow"] = num(rim, "width", 3);
            // S80's bevel join has no 3D counterpart: a mitred band
            band["join"] = str(rim, "join", "miter") == "round" ? "round" : "miter";
            band["fill_counters"] = flag(rim, "fill_counters", false);
            band["plate"] = member(rim, "plate");
            band["depth"] = 6;
            band["bevel"] = Json{{"width", 1}, {"depth", 1}, {"profile", "round"}};
            band["material"] = derivedMaterial(member(rim, "material"));
            bands.push_back(band);
            if (bands.size() >= 8) break;
        }
    }
    out["bands"] = bands;
    return normalized(out, "style_3d");
}

Json titleart::effectiveStyle3DJson(const Json& layer) {
    const Json& s = member(layer, "style_3d");
    if (s.is_object()) return s;
    return deriveStyle3D(layer);
}

Title3D titleart::decodeStyle3D(const Json& layer) {
    Title3D out;
    const Json& raw = member(layer, "style_3d");
    out.derived = !raw.is_object();
    const Json s = out.derived ? deriveStyle3D(layer) : raw;
    const std::string mode = str(s, "mode", "extrude");
    out.mode = mode == "inflate" ? Mode3D::Inflate : mode == "orb" ? Mode3D::Orb : Mode3D::Extrude;
    // the orb is a Shape layer's
    if (out.mode == Mode3D::Orb && layerTypeOf(layer) != LayerType::Shape) out.mode = Mode3D::Extrude;
    out.depth = num(s, "depth", 14);
    out.curveSegments = (int)num(s, "curve_segments", 8);
    out.bevel = decodeBevel3D(member(s, "bevel"));
    const std::string position = str(s, "bevel_position", "outside");
    out.bevelPosition = position == "inside" ? BevelPosition3D::Inside : position == "center" ? BevelPosition3D::Center : BevelPosition3D::Outside;
    const auto skew = vec2(s, "skew", {0, 0});
    out.skew = {skew[0], skew[1]};
    const Json& inflate = member(s, "inflate");
    out.inflate.height = num(inflate, "height", 12);
    out.inflate.profile = decodeCurve(member(inflate, "profile"));
    out.inflate.smoothness = num(inflate, "smoothness", 0);
    out.inflate.detail = (int)num(inflate, "detail", 1);
    out.faceImage = num(s, "face_image", 1);
    out.edgeImage = num(s, "edge_image", 0);
    out.face = decodeMaterial3D(member(s, "face"));
    if (member(s, "bevel_material").is_object()) out.bevelMaterial = decodeMaterial3D(member(s, "bevel_material"));
    if (member(s, "side").is_object()) out.side = decodeMaterial3D(member(s, "side"));
    const Json& bands = member(s, "bands");
    if (bands.is_array()) {
        for (const Json& b : bands) {
            Band3D band;
            band.id = str(b, "id", "");
            band.name = str(b, "name", "");
            band.enabled = flag(b, "enabled", true);
            band.grow = num(b, "grow", 3);
            band.roundJoin = str(b, "join", "miter") == "round";
            band.fillCounters = flag(b, "fill_counters", false);
            band.plate = geometry::decodePlate(member(b, "plate"));
            band.depth = num(b, "depth", 6);
            band.offset = num(b, "offset", 0);
            band.bevel = decodeBevel3D(member(b, "bevel"));
            band.material = decodeMaterial3D(member(b, "material"));
            if (member(b, "side").is_object()) band.side = decodeMaterial3D(member(b, "side"));
            out.bands.push_back(std::move(band));
        }
    }
    const Json& glint = member(s, "glint");
    out.glint.enabled = flag(glint, "enabled", false);
    out.glint.threshold = num(glint, "threshold", 1);
    out.glint.intensity = num(glint, "intensity", 1);
    out.glint.radius = num(glint, "radius", 6);
    out.glint.streaks = (int)num(glint, "streaks", 4);
    out.glint.length = num(glint, "length", 40);
    out.glint.angle = num(glint, "angle", 0);
    out.glint.tint = color(glint, "tint", out.glint.tint);
    const Json& ao = member(s, "ao");
    out.ao.enabled = flag(ao, "enabled", false);
    out.ao.radius = num(ao, "radius", 4);
    out.ao.intensity = num(ao, "intensity", 1);
    out.envLock = flag(s, "env_lock", false);
    const Json& lighting = member(s, "lighting");
    if (lighting.is_object()) out.lighting = lighting;
    if (member(s, "camera").is_object()) out.camera = decodeCamera3D(member(s, "camera"));
    return out;
}

Scene3D titleart::decodeScene3D(const Json& root) {
    const Json& raw = member(root, "scene_3d");
    const Json s = raw.is_object() ? raw : normalized(Json::object(), "scene_3d");
    Scene3D out;
    out.camera = decodeCamera3D(member(s, "camera"));
    out.environment = decodeEnvironment3D(member(s, "environment"));
    out.lights = decodeLights3D(member(s, "lights"));
    out.toneMapping = toneMappingFromName(str(s, "tone_mapping", "neutral"));
    out.exposure = num(s, "exposure", 1);
    out.supersample = (int)num(s, "supersample", 3);
    return out;
}

Json titleart::materialPresetValues(const std::string& preset) {
    const auto& table = presetTable();
    auto it = table.find(preset);
    return it != table.end() ? it->second : Json::object();
}

bool titleart::applyMaterialPreset(Json& material, const std::string& preset) {
    if (preset != "custom" && !presetTable().count(preset)) return false;
    if (!material.is_object()) material = Json::object();
    const Json values = materialPresetValues(preset);
    for (const auto& [key, value] : values.items()) material[key] = value;
    material["preset"] = preset;
    return true;
}

Json titleart::materialPreset(const std::string& preset) {
    Json m = Json::object();
    applyMaterialPreset(m, preset);
    return normalized(m, "material_3d");
}

const char* titleart::materialPresetSection(const std::string& p) {
    if (p == "gold" || p == "punchy_gold" || p == "pale_gold" || p == "rose_gold" || p == "chrome" || p == "silver" || p == "copper" ||
        p == "red_metal" || p == "blue_metal" || p == "green_metal" || p == "purple_metal") {
        return "Metals";
    }
    if (p == "black_gloss" || p == "white_gloss" || p == "plastic" || p == "candy") return "Gloss and Paint";
    if (p == "glass" || p == "diamond" || p == "ruby" || p == "sapphire" || p == "emerald") return "Glass and Gems";
    return "Special";
}

Lighting3D titleart::resolveLighting(const Scene3D& scene, const Title3D& style, const TitleMaterial3D* part) {
    Lighting3D out;
    for (const char* key : ENV_KEYS) out.sources.push_back({key, LightingLevel::Document});
    const Json& o = style.lighting;
    out.environment = overrideEnvironment(scene.environment, member(o, "environment"), LightingLevel::Layer, &out.sources);
    if (part) out.environment = overrideEnvironment(out.environment, part->environment, LightingLevel::Part, &out.sources);
    const bool tone = member(o, "tone_mapping").is_string();
    const bool exposure = member(o, "exposure").is_number();
    const bool lights = member(o, "lights").is_array();
    out.toneMapping = tone ? toneMappingFromName(member(o, "tone_mapping").get<std::string>()) : scene.toneMapping;
    out.exposure = exposure ? member(o, "exposure").get<double>() : scene.exposure;
    out.lights = lights ? decodeLights3D(member(o, "lights")) : scene.lights;
    out.sources.push_back({"tone_mapping", tone ? LightingLevel::Layer : LightingLevel::Document});
    out.sources.push_back({"exposure", exposure ? LightingLevel::Layer : LightingLevel::Document});
    out.sources.push_back({"lights", lights ? LightingLevel::Layer : LightingLevel::Document});
    return out;
}

int titleart::countNonAmbientLights(const std::vector<Light3D>& lights) {
    int n = 0;
    for (const Light3D& l : lights) n += l.type != LightType3D::Ambient;
    return n;
}

bool titleart::charIsRigged(const Json& adjust, bool animated) {
    if (animated) return true;
    if (!adjust.is_object()) return false;
    return flag(adjust, "rig", false) || num(adjust, "dz", 0) != 0 || num(adjust, "tilt_x", 0) != 0 || num(adjust, "tilt_y", 0) != 0;
}

CharRig3D titleart::decodeCharRig(uint32_t charIndex, const Json& a) {
    CharRig3D r;
    r.charIndex = charIndex;
    const auto offset = vec2(a, "offset", {0, 0});
    r.offset = {offset[0], offset[1]};
    r.dz = num(a, "dz", 0);
    r.tiltX = num(a, "tilt_x", 0);
    r.tiltY = num(a, "tilt_y", 0);
    r.rotation = num(a, "rotation", 0);
    r.scale = num(a, "scale", 1);
    r.hScale = num(a, "h_scale", 1);
    r.vScale = num(a, "v_scale", 1);
    return r;
}

bool titleart::charHasMaterialBlock(const Json& a) {
    if (!a.is_object()) return false;
    if (member(a, "face_3d").is_object() || member(a, "side_3d").is_object()) return true;
    const Json& bands = member(a, "bands_3d");
    if (bands.is_array()) {
        for (const Json& b : bands) {
            if (b.is_object()) return true;
        }
    }
    return false;
}
