// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The 3D members' defaults are Pachislop's (src/model/types.ts, defaults.ts, lighting.ts) by
// SafEight, used with the author's permission, renamed snake_case (docs/specs/S86-title-3d.md §6.1).

#include "Title3DSchema.h"

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/three_d/BevelProfile.h"

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    const std::vector<std::string> MATERIAL_PRESETS = {
        "custom",     "gold",        "punchy_gold", "pale_gold",   "rose_gold",    "chrome",      "silver",  "copper",
        "red_metal",  "blue_metal",  "green_metal", "purple_metal", "black_gloss", "white_gloss", "rainbow", "glass",
        "plastic",    "emissive",    "candy",       "diamond",     "ruby",         "sapphire",    "emerald"};

    const std::vector<std::string> ENVIRONMENTS = {
        "studio", "strips", "backlit", "under", "sunset", "neon", "gold", "night", "softbox",       // procedural (softbox: HDRI + key)
        "highway", "city", "sky",                                                                  // photo (Poly Haven)
        "night_city", "night_dark", "sunset_harbour", "warehouse",                                 // S84's photo environments
        "custom", "matcap"};

    const std::vector<std::string> TONE_MAPPINGS = {"none", "aces_filmic", "agx", "neutral", "punch"};

    // a member with its type's animation class, the S86 owner and a hash scope
    class Kind {
    public:
        // a new kind (an existing one, S80's transform and char_adjust, only gains members)
        Kind(TitleArtSchema& schema, const char* name, const char* label, HashScope hash, bool sparse = false,
             void (*prepare)(Json&, NormalizeContext&) = nullptr, std::vector<std::string> alwaysWritten = {})
            : schema(schema), name(name) {
            SchemaKind k;
            k.name = name;
            k.label = label;
            k.hash = hash;
            k.sparse = sparse;
            k.prepare = prepare;
            k.alwaysWritten = std::move(alwaysWritten);
            schema.addKind(std::move(k));
            scope = hash;
        }

        SchemaMember& add(const std::string& key, ValueType type, Json def) {
            members.emplace_back();
            SchemaMember& m = members.back();
            m.key = key;
            m.type = type;
            m.defaultValue = std::move(def);
            m.owner = "S86";
            m.hash = scope;
            m.label = labelOf(key);
            const bool scalar = type == ValueType::Number || type == ValueType::Color || type == ValueType::Vec2;
            m.anim = scalar ? FxAnim::Rerender : FxAnim::Step;
            m.interp = scalar ? ParamInterp::Smooth : ParamInterp::Stepped;
            if (type == ValueType::Object || type == ValueType::ObjectArray || type == ValueType::Id) m.anim = FxAnim::None;
            return m;
        }
        SchemaMember& number(const std::string& key, double def, double min, double max, ParamUnit unit = ParamUnit::None) {
            SchemaMember& m = add(key, ValueType::Number, def);
            m.min = min;
            m.max = max;
            m.unit = unit;
            return m;
        }
        SchemaMember& integer(const std::string& key, int64_t def, double min, double max) {
            SchemaMember& m = add(key, ValueType::Integer, def);
            m.min = min;
            m.max = max;
            return m;
        }
        SchemaMember& boolean(const std::string& key, bool def) { return add(key, ValueType::Bool, def); }
        SchemaMember& enumeration(const std::string& key, const char* def, std::vector<std::string> options) {
            SchemaMember& m = add(key, ValueType::Enum, def);
            m.options = std::move(options);
            return m;
        }
        SchemaMember& color(const std::string& key, const char* def) { return add(key, ValueType::Color, def); }
        SchemaMember& vec2(const std::string& key, double x, double y, ParamUnit unit = ParamUnit::None) {
            SchemaMember& m = add(key, ValueType::Vec2, Json::array({x, y}));
            m.unit = unit;
            return m;
        }
        SchemaMember& object(const std::string& key, const char* kind, Json def = nullptr, bool nullable = false) {
            SchemaMember& m = add(key, ValueType::Object, std::move(def));
            m.kind = kind;
            m.nullable = nullable;
            return m;
        }
        SchemaMember& array(const std::string& key, const char* kind, size_t maxItems, bool nullable = false) {
            SchemaMember& m = add(key, ValueType::ObjectArray, Json::array());
            m.kind = kind;
            m.maxItems = maxItems;
            m.nullable = nullable;
            return m;
        }
        SchemaMember& asset(const std::string& key) { return add(key, ValueType::AssetRef, nullptr); }
        SchemaMember& id() {
            SchemaMember& m = add("id", ValueType::Id, "");
            m.readOnly = true;
            m.hash = HashScope::Identity;
            m.label = "Id";
            return m;
        }

        // the members registered on the kind (call last)
        void commit() {
            for (SchemaMember& m : members) schema.addMember(name, std::move(m));
            members.clear();
        }

        static std::string labelOf(const std::string& key) {
            std::string out;
            bool upper = true;
            for (char c : key) {
                if (c == '_') {
                    out += ' ';
                    upper = true;
                    continue;
                }
                out += upper ? (char)std::toupper((unsigned char)c) : c;
                upper = false;
            }
            return out;
        }

    private:
        TitleArtSchema& schema;
        std::string name;
        HashScope scope;
        std::vector<SchemaMember> members;
    };

    Json curveOf(std::initializer_list<std::pair<double, double>> points) {
        Json list = Json::array();
        for (const auto& p : points) list.push_back(Json{{"x", p.first}, {"y", p.second}, {"corner", false}});
        return Json{{"points", list}};
    }

    // an unknown id of `key` loads as `fallback` with one warning naming it (R64)
    void knownOr(Json& object, const char* key, const std::vector<std::string>& valid, const char* fallback, const char* what,
                 NormalizeContext& context) {
        auto it = object.find(key);
        if (it == object.end() || !it->is_string()) return;
        const std::string id = it->get<std::string>();
        if (std::find(valid.begin(), valid.end(), id) != valid.end()) return;
        context.warn(key, std::string("unknown ") + what + " '" + id + "': loaded as " + fallback);
        *it = fallback;
    }

    void prepareMaterial(Json& object, NormalizeContext& context) {
        if (object.is_object()) knownOr(object, "preset", MATERIAL_PRESETS, "custom", "material preset", context);
    }
    void prepareBevel(Json& object, NormalizeContext& context) {
        if (object.is_object()) knownOr(object, "profile", three_d::bevelProfileNames(), "round", "bevel profile", context);
    }
    void prepareEnvironment(Json& object, NormalizeContext& context) {
        if (object.is_object()) knownOr(object, "preset", ENVIRONMENTS, "studio", "environment", context);
    }

    // a light's position: three finite numbers
    void normalizePosition(Json& value, const std::string& path, NormalizeContext& context) {
        const Json fallback = Json::array({-600, 800, 1000});
        if (!value.is_array() || value.size() != 3) {
            context.warn(path, "expected [x, y, z]: the default is used");
            value = fallback;
            return;
        }
        for (size_t i = 0; i < 3; i++) {
            if (!value[i].is_number() || !std::isfinite(value[i].get<double>())) {
                context.warn(path, "expected [x, y, z]: the default is used");
                value = fallback;
                return;
            }
        }
    }

    void addEnvironmentMembers(Kind& k) {
        k.enumeration("preset", "studio", ENVIRONMENTS).label = "Environment";
        k.asset("hdri").label = "HDRI";
        k.asset("matcap").label = "Matcap";
        k.number("intensity", 1, 0, 16);
        k.number("rotation", 0, -36000, 36000, ParamUnit::Deg);
        k.color("tint", "#ffffff");
        k.number("contrast", 1, 0, 4);
        k.number("saturation", 1, 0, 4);
        k.number("blur", 0, 0, 1);
        k.number("repeat_x", 1, 0.25, 16).label = "Repeat X";
        k.number("repeat_y", 1, 0.25, 16).label = "Repeat Y";
    }

}

const std::vector<std::string>& titleart::title3DMaterialPresetNames() {
    return MATERIAL_PRESETS;
}

const std::vector<std::string>& titleart::title3DEnvironmentNames() {
    return ENVIRONMENTS;
}

const std::vector<std::string>& titleart::title3DToneMappingNames() {
    return TONE_MAPPINGS;
}

void titleart::registerTitle3DMembers(TitleArtSchema& schema) {
    // ---- materials ----
    {
        Kind k(schema, "material_3d", "3D Material", HashScope::Stack, false, prepareMaterial);
        k.enumeration("preset", "custom", MATERIAL_PRESETS).label = "Preset";
        k.color("color", "#ffffff");
        k.object("gradient", "gradient", nullptr, true);
        k.enumeration("gradient_kind", "linear", {"linear", "radial", "reflected"});
        k.number("gradient_angle", 90, -360, 360, ParamUnit::Deg);
        k.enumeration("gradient_span", "layer", {"layer", "line", "char", "part"});
        k.number("gradient_scale", 1, 0.01, 100);
        k.number("gradient_offset", 0, -1, 1);
        k.enumeration("gradient_axis", "xy", {"xy", "depth"});
        k.number("gradient_bend", 0, -1, 1);
        k.number("metallic", 0, 0, 1);
        k.number("roughness", 0.3, 0, 1);
        k.number("env_multiplier", 1, 0, 4).label = "Environment Multiplier";
        k.number("specular_multiplier", 1, 0, 4);
        k.number("clearcoat", 0, 0, 1);
        k.number("clearcoat_roughness", 0.04, 0, 1);
        k.number("iridescence", 0, 0, 1);
        k.number("iridescence_ior", 1.6, 1, 2.5).label = "Iridescence IOR";
        k.vec2("iridescence_thickness_nm", 180, 520).label = "Iridescence Thickness (nm)";
        k.number("transmission", 0, 0, 1);
        k.number("ior", 1.5, 1, 2.5).label = "IOR";
        k.number("dispersion", 0, 0, 1);
        k.number("glass_depth", 0, 0, 1);
        k.number("flakes", 0, 0, 1);
        k.number("flake_scale", 0.6, 0.05, 100, ParamUnit::Percent);
        k.object("pattern", "pattern");
        k.object("surface", "surface_3d");
        k.object("base_texture", "base_texture_3d");
        k.color("emission", "#000000");
        k.number("emission_intensity", 0, 0, 100);
        k.number("opacity", 1, 0, 1);
        k.object("environment", "environment_3d_overrides");
        SchemaMember& link = k.add("link", ValueType::String, "");
        link.hash = HashScope::Identity;
        link.anim = FxAnim::None;
        k.commit();
    }
    {
        Kind k(schema, "surface_3d", "Surface", HashScope::Stack);
        k.enumeration("kind", "none", {"none", "crumple", "hammered", "liquid"});
        k.number("amount", 0.5, 0, 1);
        k.number("scale", 20, 0.1, 1000, ParamUnit::Percent);
        k.integer("seed", 1, 0, 2147483647);
        k.commit();
    }
    {
        Kind k(schema, "base_texture_3d", "Base Texture", HashScope::Stack);
        k.asset("texture");
        k.vec2("tiling", 1, 1);
        k.vec2("offset", 0, 0);
        k.enumeration("projection", "front", {"front", "wrap"}).hash = HashScope::Geometry;
        k.commit();
    }
    // ---- environments and lighting ----
    {
        Kind k(schema, "environment_3d", "Environment", HashScope::Stack, false, prepareEnvironment);
        addEnvironmentMembers(k);
        k.commit();
    }
    {
        Kind k(schema, "environment_3d_overrides", "Environment Overrides", HashScope::Stack, true, prepareEnvironment);
        addEnvironmentMembers(k);
        k.commit();
    }
    {
        Kind k(schema, "light_3d", "Light", HashScope::Stack);
        k.id();
        k.enumeration("type", "directional", {"directional", "point", "ambient"});
        k.color("color", "#ffffff");
        k.number("intensity", 2, 0, 100);
        SchemaMember& position = k.add("position", ValueType::Json, Json::array({-600, 800, 1000}));
        position.normalize = normalizePosition;
        position.anim = FxAnim::Rerender;
        position.interp = ParamInterp::Smooth;
        position.unit = ParamUnit::Px;
        k.commit();
    }
    {
        Kind k(schema, "lighting_3d", "Lighting", HashScope::Stack, true, nullptr, {"environment"});
        k.object("environment", "environment_3d_overrides");
        k.enumeration("tone_mapping", "neutral", TONE_MAPPINGS);
        k.number("exposure", 1, 0, 16);
        k.array("lights", "light_3d", 8);
        k.commit();
    }
    {
        Kind k(schema, "camera_3d", "Camera", HashScope::Stack);
        k.number("fov", 30, 1, 120, ParamUnit::Deg).label = "FOV";
        k.number("x", 0, -100000, 100000, ParamUnit::Px);
        k.number("y", 0, -100000, 100000, ParamUnit::Px);
        k.number("z_offset", 0, -100000, 100000, ParamUnit::Px);
        k.number("rotation_x", 0, -36000, 36000, ParamUnit::Deg);
        k.number("rotation_y", 0, -36000, 36000, ParamUnit::Deg);
        k.number("rotation", 0, -36000, 36000, ParamUnit::Deg);
        k.commit();
    }
    {
        Kind k(schema, "scene_3d", "3D Scene", HashScope::Stack);
        k.object("camera", "camera_3d");
        k.object("environment", "environment_3d");
        SchemaMember& lights = k.array("lights", "light_3d", 8);
        lights.defaultValue = Json::array({Json{{"type", "directional"}, {"color", "#ffffff"}, {"intensity", 2}, {"position", Json::array({-600, 800, 1000})}},
                                           Json{{"type", "ambient"}, {"color", "#ffffff"}, {"intensity", 0.3}}});
        k.enumeration("tone_mapping", "neutral", TONE_MAPPINGS);
        k.number("exposure", 1, 0, 16);
        SchemaMember& supersample = k.integer("supersample", 3, 1, 3);
        supersample.anim = FxAnim::None;
        k.commit();
    }
    // ---- geometry ----
    {
        Kind k(schema, "bevel_3d", "Bevel", HashScope::Geometry, false, prepareBevel);
        k.number("width", 0, 0, 100, ParamUnit::Percent);
        k.number("depth", 0, 0, 100, ParamUnit::Percent);
        k.enumeration("profile", "round", three_d::bevelProfileNames());
        k.integer("segments", 6, 1, 24);
        k.number("curve", 0, -1, 1);
        k.object("custom", "curve", curveOf({{0, 0}, {1, 1}}));
        k.commit();
    }
    {
        Kind k(schema, "inflate_3d", "Inflate", HashScope::Geometry);
        k.number("height", 12, 0, 200, ParamUnit::Percent);
        k.object("profile", "curve", curveOf({{0, 0}, {0.35, 0.8}, {1, 1}}));
        k.number("smoothness", 0, 0, 100);
        k.integer("detail", 1, 1, 4);
        k.commit();
    }
    {
        Kind k(schema, "band_3d", "Band", HashScope::Geometry);
        k.id();
        k.add("name", ValueType::String, "").hash = HashScope::Identity;
        k.boolean("enabled", true);
        k.number("grow", 3, 0, 100, ParamUnit::Percent);
        k.enumeration("join", "miter", {"miter", "round"});
        k.boolean("fill_counters", false);
        k.object("plate", "plate");
        k.number("depth", 6, 0, 200, ParamUnit::Percent);
        k.number("offset", 0, -200, 200, ParamUnit::Percent).label = "Z Offset";
        k.object("bevel", "bevel_3d", Json{{"width", 1}, {"depth", 1}});
        k.object("material", "material_3d").hash = HashScope::Stack;
        k.object("side", "material_3d", nullptr, true).hash = HashScope::Stack;
        k.commit();
    }
    {
        Kind k(schema, "glint_3d", "Glint", HashScope::Stack);
        k.boolean("enabled", false);
        k.number("threshold", 1, 0, 64);
        k.number("intensity", 1, 0, 4);
        k.number("radius", 6, 0, 100, ParamUnit::Percent);
        SchemaMember& streaks = k.integer("streaks", 4, 0, 8);
        streaks.allowed = {0, 2, 4, 6, 8};
        k.number("length", 40, 0, 400, ParamUnit::Percent);
        k.number("angle", 0, -360, 360, ParamUnit::Deg);
        k.color("tint", "#fff4dc");
        k.commit();
    }
    {
        Kind k(schema, "ao_3d", "Ambient Occlusion", HashScope::Stack);
        k.boolean("enabled", false);
        k.number("radius", 4, 0, 100, ParamUnit::Percent);
        k.number("intensity", 1, 0, 2);
        k.commit();
    }
    {
        Kind k(schema, "style_3d", "3D Style", HashScope::Geometry);
        k.enumeration("mode", "extrude", {"extrude", "inflate", "orb"});
        k.number("depth", 14, 0, 200, ParamUnit::Percent);
        k.integer("curve_segments", 8, 2, 32);
        k.object("bevel", "bevel_3d");
        k.enumeration("bevel_position", "outside", {"outside", "center", "inside"});
        SchemaMember& skew = k.vec2("skew", 0, 0);
        skew.min = -2;
        skew.max = 2;
        skew.hash = HashScope::Stack;
        k.object("inflate", "inflate_3d");
        k.number("face_image", 1, 0, 1).hash = HashScope::Stack;
        k.number("edge_image", 0, 0, 1).hash = HashScope::Stack;
        k.object("face", "material_3d", Json{{"color", "#c0c0c0"}, {"roughness", 0.4}, {"env_multiplier", 0.5}}).hash = HashScope::Stack;
        k.object("bevel_material", "material_3d", nullptr, true).hash = HashScope::Stack;
        k.object("side", "material_3d", nullptr, true).hash = HashScope::Stack;
        k.array("bands", "band_3d", 8);
        k.object("glint", "glint_3d").hash = HashScope::Stack;
        k.object("ao", "ao_3d").hash = HashScope::Stack;
        SchemaMember& lock = k.boolean("env_lock", false);
        lock.hash = HashScope::Stack;
        lock.label = "Lock Environment To Layer";
        k.object("lighting", "lighting_3d").hash = HashScope::Stack;
        k.object("camera", "camera_3d", nullptr, true).hash = HashScope::Stack;
        k.commit();
    }

    // ---- members of S80's kinds ----
    auto layerStyle = [&](const char* kind) {
        SchemaMember m;
        m.key = "style_3d";
        m.type = ValueType::Object;
        m.kind = "style_3d";
        m.nullable = true;
        m.defaultValue = nullptr;
        m.owner = "S86";
        m.label = "3D Style";
        m.hash = HashScope::Geometry;
        m.anim = FxAnim::None;
        schema.addMember(kind, std::move(m));
    };
    for (const char* kind : {"layer.text", "layer.shape", "layer.vector", "layer.image"}) layerStyle(kind);
    {
        SchemaMember m;
        m.key = "scene_3d";
        m.type = ValueType::Object;
        m.kind = "scene_3d";
        m.nullable = true;
        m.defaultValue = nullptr;
        m.owner = "S86";
        m.label = "3D Scene";
        m.hash = HashScope::Stack;
        m.anim = FxAnim::None;
        schema.addMember("document", std::move(m));
    }
    {
        Kind k(schema, "transform", "Transform", HashScope::Stack);
        // transform exists: Kind's constructor only adds new kinds, so these land on S80's
        SchemaMember& z = k.number("z", 0, -100000, 100000, ParamUnit::Px);
        z.anim = FxAnim::Live;
        SchemaMember& rx = k.number("rotation_x", 0, -36000, 36000, ParamUnit::Deg);
        rx.anim = FxAnim::Live;
        rx.label = "Rotation X";
        SchemaMember& ry = k.number("rotation_y", 0, -36000, 36000, ParamUnit::Deg);
        ry.anim = FxAnim::Live;
        ry.label = "Rotation Y";
        k.commit();
    }
    {
        Kind k(schema, "char_adjust", "Character", HashScope::Geometry);
        k.number("dz", 0, -1000, 1000, ParamUnit::Percent).hash = HashScope::Stack;
        SchemaMember& tx = k.number("tilt_x", 0, -36000, 36000, ParamUnit::Deg);
        tx.hash = HashScope::Stack;
        tx.label = "Tilt X";
        SchemaMember& ty = k.number("tilt_y", 0, -36000, 36000, ParamUnit::Deg);
        ty.hash = HashScope::Stack;
        ty.label = "Tilt Y";
        k.object("face_3d", "material_3d", nullptr, true).label = "Face 3D";
        k.object("side_3d", "material_3d", nullptr, true).label = "Side 3D";
        k.array("bands_3d", "material_3d", 8, true).label = "Bands 3D";
        k.boolean("rig", false);
        k.commit();
    }
}
