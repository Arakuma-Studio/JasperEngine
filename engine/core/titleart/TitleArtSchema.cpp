// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The defaults of the 2D materials, rims and text members are Pachislop's (src/model/defaults.ts,
// types.ts) by SafEight, used with the author's permission, renamed snake_case (§6.1).

#include "TitleArtSchema.h"
#include "TitleArtIO.h"
#include "TitleEffectRegistry.h"
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
#include "three_d/Title3DSchema.h"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace jasperengine::titleart {

    // the table's builder (TitleArtSchema's private members)
    struct SchemaBuilderAccess {
        static SchemaKind* mutableKind(TitleArtSchema& schema, std::string_view name) { return schema.mutableKind(name); }
    };

    // the program's registration functions: one line per spec that adds members (S81 `runtime`,
    // S85 `transform.stretch`, S86 `style_3d`, S87 `translations`...), called after S80's table
    void registerProgramMembers(TitleArtSchema& schema);

}

namespace {

    // "line_height" -> "Line Height", "flip_x" -> "Flip X"
    std::string labelOf(const std::string& key) {
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

    // Builds one kind: members take the type's default animation class and interpolation, then the
    // kind's defaults set with anim() / hash(), then what the member call sets
    class KindBuilder {
    public:
        KindBuilder(TitleArtSchema& schema, const char* name, const char* label, HashScope rootHash = HashScope::Node)
            : schema(schema) {
            SchemaKind kind;
            kind.name = name;
            kind.label = label;
            kind.hash = rootHash;
            schema.addKind(std::move(kind));
            target = SchemaBuilderAccess::mutableKind(schema, name);
        }

        KindBuilder& anim(FxAnim a) { kindAnim = a; hasKindAnim = true; return *this; }
        KindBuilder& hash(HashScope h) { kindHash = h; return *this; }
        SchemaKind& kind() { return *target; }

        SchemaMember& add(const std::string& key, ValueType type, Json def) {
            SchemaMember m;
            m.key = key;
            m.type = type;
            m.defaultValue = std::move(def);
            m.label = labelOf(key);
            switch (type) {
                case ValueType::Number: case ValueType::Color: case ValueType::Vec2: case ValueType::Vec2List:
                    m.anim = FxAnim::Rerender;
                    m.interp = ParamInterp::Smooth;
                    break;
                case ValueType::Integer: case ValueType::Bool: case ValueType::String: case ValueType::Enum:
                case ValueType::AssetRef: case ValueType::AssetRefList: case ValueType::LayerRef: case ValueType::StringList:
                    m.anim = FxAnim::Step;
                    m.interp = ParamInterp::Stepped;
                    break;
                default:
                    m.anim = FxAnim::None;
                    m.interp = ParamInterp::Stepped;
                    break;
            }
            const bool leaf = type != ValueType::Object && type != ValueType::ObjectArray && type != ValueType::ObjectMap &&
                              type != ValueType::ObjectArrayMap && type != ValueType::EffectParams &&
                              type != ValueType::GeneratorParams && type != ValueType::Id;
            if (hasKindAnim && leaf) m.anim = kindAnim;
            m.hash = kindHash;
            target->members.push_back(std::move(m));
            return target->members.back();
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
        SchemaMember& string(const std::string& key, const char* def) { return add(key, ValueType::String, def); }
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
        SchemaMember& asset(const std::string& key) { return add(key, ValueType::AssetRef, nullptr); }
        SchemaMember& id() {
            SchemaMember& m = add("id", ValueType::Id, "");
            m.readOnly = true;
            m.hash = HashScope::Identity;
            m.anim = FxAnim::None;
            m.label = "Id";
            return m;
        }
        SchemaMember& object(const std::string& key, const char* kind, Json def = nullptr) {
            SchemaMember& m = add(key, ValueType::Object, std::move(def));
            m.kind = kind;
            m.hash = HashScope::Inherit;
            return m;
        }
        SchemaMember& array(const std::string& key, const char* kind, size_t maxItems, Json def = Json::array()) {
            SchemaMember& m = add(key, ValueType::ObjectArray, std::move(def));
            m.kind = kind;
            m.maxItems = maxItems;
            return m;
        }
        SchemaMember& map(const std::string& key, const char* kind, MapKeys keys) {
            SchemaMember& m = add(key, ValueType::ObjectMap, Json::object());
            m.kind = kind;
            m.mapKeys = keys;
            return m;
        }

    private:
        TitleArtSchema& schema;
        SchemaKind* target = nullptr;
        FxAnim kindAnim = FxAnim::Rerender;
        bool hasKindAnim = false;
        HashScope kindHash = HashScope::Inherit;
    };

    const std::vector<std::string> BLEND_NAMES = {
        "normal", "add", "screen", "multiply", "overlay", "soft_light", "hard_light", "darken", "lighten",
        "color_dodge", "color_burn", "linear_burn", "difference", "exclusion", "hue", "saturation", "color", "luminosity"};

    const std::vector<std::string> LAYER_TYPES = {"text", "shape", "image", "vector", "group", "generator", "adjustment"};

    const std::vector<std::string> SHAPES = {"rect", "parallelogram", "ellipse", "orb", "polygon", "star", "burst",
                                             "shield", "speech", "shout", "ribbon"};

    const std::vector<std::string> PATTERN_KINDS = {"none", "zebra", "tiger", "giraffe", "leopard", "checker", "hazard",
                                                    "stripes", "dots", "sakura", "seigaiha", "asanoha", "shippo", "kikko",
                                                    "uroko", "custom"};

    // the default object of a gradient / curve member
    Json gradientDefault() {
        return Json{{"stops", Json::array({Json{{"pos", 0}, {"color", "#000000"}, {"alpha", 1}, {"mid", 0.5}},
                                           Json{{"pos", 1}, {"color", "#ffffff"}, {"alpha", 1}, {"mid", 0.5}}})},
                    {"interp", "rgb"}};
    }

    Json curveOf(std::initializer_list<std::pair<double, double>> points) {
        Json list = Json::array();
        for (const auto& p : points) list.push_back(Json{{"x", p.first}, {"y", p.second}, {"corner", false}});
        return Json{{"points", list}};
    }

    // shape geometry, on shape layers (Relayout) and shape masks (Rerender)
    void addShapeGeometry(KindBuilder& k) {
        k.enumeration("shape", "rect", SHAPES);
        k.number("size", 200, 1, 16384, ParamUnit::Px);
        k.number("aspect", 1, 0.05, 20);
        k.number("corner", 0, 0, 100, ParamUnit::Percent);
        k.number("slant", 0, -80, 80, ParamUnit::Deg);
        k.integer("points", 5, 3, 64);
        k.number("inner", 0.5, 0, 1);
        k.number("jitter", 0, 0, 1);
        k.integer("seed", 1, 0, 2147483647);
        k.object("tail", "shape_tail");
    }

    // warp, distort, weight, dimension, style, part_effects: the outline and style members of
    // Text and Shape layers (Vector layers' in `shape` mode)
    void addOutlineMembers(KindBuilder& k) {
        k.object("warp", "warp");
        k.object("distort", "distort");
        k.number("weight", 0, -20, 20, ParamUnit::Percent).anim = FxAnim::Relayout;
    }

    void addStyleMembers(KindBuilder& k) {
        SchemaMember& dimension = k.enumeration("dimension", "2d", {"2d", "3d"});
        dimension.hash = HashScope::Geometry;
        SchemaMember& style = k.object("style", "style2d");
        style.hash = HashScope::Stack;
        SchemaMember& parts = k.add("part_effects", ValueType::ObjectArrayMap, Json::object());
        parts.kind = "effect";
        parts.mapKeys = MapKeys::PartKey;
        parts.maxItems = TITLE_MAX_EFFECTS;
        parts.hash = HashScope::Effects;
        parts.label = "Part Effects";
    }

    // the layer's name default is its type's ("Text", "Shape"...); an image layer's face shows its
    // own pixels (`source`), as does a vector layer in `artwork` mode
    void prepareLayer(Json& layer, NormalizeContext&) {
        const LayerType type = layerTypeOf(layer);
        if (!layer.contains("name") || !layer["name"].is_string()) {
            layer["name"] = type == LayerType::Vector ? "Vector" : layerTypeLabel(type);
        }
        const bool ownPixels = type == LayerType::Image ||
                               (type == LayerType::Vector && layer.value("mode", std::string("shape")) == "artwork");
        if (!ownPixels) return;
        Json& style = layer["style"];
        if (!style.is_object()) style = Json::object();
        Json& face = style["face"];
        if (!face.is_object()) face = Json::object();
        Json& fill = face["fill"];
        if (!fill.is_object()) fill = Json::object();
        if (!fill.contains("kind")) fill["kind"] = "source";
    }

    // gradient stops sorted by position, 2..32 of them
    void finishGradient(Json& gradient, const std::string& path, NormalizeContext& context);
    // curve points sorted by x, the ends present, 2..32 of them
    void finishCurve(Json& curve, const std::string& path, NormalizeContext& context);

    void buildS80(TitleArtSchema& schema) {
        // ---- document ----
        {
            KindBuilder k(schema, "document", "Title Art");
            k.anim(FxAnim::None);
            SchemaMember& format = k.string("format", TITLE_ART_FORMAT);
            format.readOnly = true;
            format.hash = HashScope::Identity;
            SchemaMember& version = k.integer("version", TITLE_ART_VERSION, 1, 1000000);
            version.readOnly = true;
            version.hash = HashScope::Identity;
            SchemaMember& guid = k.string("guid", "");
            guid.readOnly = true;
            guid.hash = HashScope::Identity;
            guid.label = "GUID";
            k.string("name", "").hash = HashScope::Identity;
            k.object("canvas", "canvas").hash = HashScope::Node;
            SchemaMember& light = k.object("global_light", "global_light");
            light.hash = HashScope::Stack;
            SchemaMember& seed = k.integer("seed", 1, 0, 2147483647);
            seed.anim = FxAnim::Rerender;
            seed.hash = HashScope::Geometry;
            k.map("material_links", "material_link", MapKeys::Any).hash = HashScope::Identity;
            k.array("layers", "layer", TITLE_MAX_LAYERS).hash = HashScope::Node;
            k.array("exposed", "exposed", TITLE_MAX_EXPOSED).hash = HashScope::Identity;
            k.array("outputs", "output", TITLE_MAX_OUTPUTS, Json::array({Json{{"id", "main"}}})).hash = HashScope::Identity;
        }
        for (const auto& [key, owner] : std::vector<std::pair<const char*, const char*>>{
                 {"runtime", "S81"}, {"scene_3d", "S86"}, {"color_schemes", "S84"}, {"animation", "S85"},
                 {"source_language", "S87"}, {"languages", "S87"}, {"string_namespace", "S87"}, {"dpi", "S87"},
                 {"import_notes", "S87"}}) {
            schema.reserve("document", key, owner);
        }
        {
            KindBuilder k(schema, "canvas", "Canvas");
            k.anim(FxAnim::None).hash(HashScope::Node);
            k.integer("width", 1920, TITLE_MIN_CANVAS_SIDE, TITLE_MAX_CANVAS_SIDE).unit = ParamUnit::Px;
            k.integer("height", 1080, TITLE_MIN_CANVAS_SIDE, TITLE_MAX_CANVAS_SIDE).unit = ParamUnit::Px;
            k.object("background", "canvas_background");
        }
        {
            KindBuilder k(schema, "canvas_background", "Background");
            k.anim(FxAnim::None);
            k.boolean("transparent", true);
            k.color("color", "#000000");
        }
        {
            KindBuilder k(schema, "global_light", "Global Light", HashScope::Stack);
            k.number("angle", 120, -360, 360, ParamUnit::Deg);
            k.number("altitude", 30, 0, 90, ParamUnit::Deg);
        }
        {
            KindBuilder k(schema, "material_link", "Material Link", HashScope::Identity);
            k.anim(FxAnim::None);
            k.string("name", "");
        }

        // ---- layers ----
        {
            KindBuilder k(schema, "layer", "Layer");
            k.kind().discriminator = "type";
            k.kind().prepare = &prepareLayer;
            k.id();
            SchemaMember& type = k.enumeration("type", "text", LAYER_TYPES);
            type.readOnly = true;
            type.anim = FxAnim::None;
            type.hash = HashScope::Geometry;
            SchemaMember& name = k.string("name", "");
            name.anim = FxAnim::None;
            name.hash = HashScope::Identity;
            SchemaMember& visible = k.boolean("visible", true);
            visible.anim = FxAnim::Live;
            visible.hash = HashScope::Node;
            SchemaMember& locked = k.boolean("locked", false);
            locked.anim = FxAnim::None;
            locked.hash = HashScope::Identity;
            SchemaMember& guide = k.boolean("guide", false);
            guide.anim = FxAnim::None;
            guide.hash = HashScope::Node;
            k.enumeration("blend", "normal", BLEND_NAMES).hash = HashScope::Node;
            SchemaMember& opacity = k.number("opacity", 1, 0, 1);
            opacity.anim = FxAnim::Live;
            opacity.combine = ParamCombine::Multiply;
            opacity.hash = HashScope::Node;
            k.boolean("clip", false).hash = HashScope::Node;
            k.object("transform", "transform");
            SchemaMember& masks = k.array("masks", "mask", TITLE_MAX_MASKS);
            masks.hash = HashScope::Masks;
            SchemaMember& effects = k.array("effects", "effect", TITLE_MAX_EFFECTS);
            effects.hash = HashScope::Effects;
            SchemaMember& look = k.object("look", "layer_look");
            look.hash = HashScope::Identity;
            for (const char* variant : {"text", "shape", "image", "vector", "group", "generator", "adjustment"}) {
                k.kind().variants[variant] = std::string("layer.") + variant;
            }
        }
        schema.reserve("layer", "tint", "S85");
        schema.reserve("layer", "flash", "S85");
        schema.reserve("layer", "only_languages", "S87");
        {
            KindBuilder k(schema, "transform", "Transform", HashScope::Stack);
            k.anim(FxAnim::Live);
            k.vec2("position", 0, 0, ParamUnit::Px).hash = HashScope::Placement;
            k.number("rotation", 0, -36000, 36000, ParamUnit::Deg).hash = HashScope::Stack;
            k.number("scale", 1, 0.01, 100).hash = HashScope::Geometry;
            SchemaMember& flipX = k.boolean("flip_x", false);
            flipX.hash = HashScope::Stack;
            flipX.label = "Flip X";
            SchemaMember& flipY = k.boolean("flip_y", false);
            flipY.hash = HashScope::Stack;
            flipY.label = "Flip Y";
        }
        schema.reserve("transform", "z", "S86");
        schema.reserve("transform", "rotation_x", "S86");
        schema.reserve("transform", "rotation_y", "S86");
        schema.reserve("transform", "stretch", "S85");
        {
            KindBuilder k(schema, "layer_look", "Look", HashScope::Identity);
            k.anim(FxAnim::None).hash(HashScope::Identity);
            k.string("id", "").label = "Look";
            k.string("color_scheme", "");
            SchemaMember& owner = k.add("owner", ValueType::LayerRef, "");
            owner.anim = FxAnim::None;
        }
        {
            KindBuilder k(schema, "layer.text", "Text Layer", HashScope::Geometry);
            k.kind().base = "layer";
            k.anim(FxAnim::Relayout).hash(HashScope::Geometry);
            k.string("text", "Title");
            SchemaMember& font = k.asset("font");
            font.anim = FxAnim::Step;
            k.integer("face_index", 0, 0, 65535).anim = FxAnim::Step;
            SchemaMember& fallbacks = k.add("fallback_fonts", ValueType::AssetRefList, Json::array());
            fallbacks.anim = FxAnim::Step;
            fallbacks.maxItems = 16;
            fallbacks.label = "Fallback Fonts";
            SchemaMember& variations = k.add("variations", ValueType::NumberMap, Json::object());
            variations.mapKeys = MapKeys::Tag;
            variations.anim = FxAnim::Relayout;
            variations.interp = ParamInterp::Smooth;
            SchemaMember& features = k.add("features", ValueType::IntegerMap, Json::object());
            features.mapKeys = MapKeys::Tag;
            features.anim = FxAnim::Relayout;
            features.min = 0;
            features.max = 65535;
            k.string("language", "");
            k.number("size", 200, 1, 8192, ParamUnit::Px);
            k.number("tracking", 0, -1000, 1000).label = "Tracking";
            k.number("line_height", 1.1, 0.1, 10);
            k.boolean("kerning", true);
            k.enumeration("align", "center", {"start", "center", "end"});
            k.boolean("vertical", false);
            k.boolean("tate_chu_yoko", true).label = "Tate-chu-yoko";
            k.integer("tate_chu_yoko_max", 3, 1, 4).label = "Tate-chu-yoko Max";
            k.number("proportional", 0, 0, 1);
            k.number("h_scale", 1, 0.1, 10).label = "Horizontal Scale";
            k.number("v_scale", 1, 0.1, 10).label = "Vertical Scale";
            k.number("shear", 0, -60, 60, ParamUnit::Deg);
            k.map("chars", "char_adjust", MapKeys::Index).label = "Characters";
            k.map("lines", "line_adjust", MapKeys::Index);
            addOutlineMembers(k);
            addStyleMembers(k);
        }
        for (const char* key : {"text_key", "translate", "max_chars", "translator_note", "fit", "translations"}) {
            schema.reserve("layer.text", key, "S87");
        }
        schema.reserve("layer.text", "glyph_mode", "S85");
        schema.reserve("layer.text", "style_3d", "S86");
        {
            KindBuilder k(schema, "char_adjust", "Character", HashScope::Geometry);
            k.anim(FxAnim::Relayout).hash(HashScope::Geometry);
            k.vec2("offset", 0, 0, ParamUnit::Percent);
            k.number("scale", 1, 0.01, 100);
            k.number("h_scale", 1, 0.1, 10).label = "Horizontal Scale";
            k.number("v_scale", 1, 0.1, 10).label = "Vertical Scale";
            k.number("rotation", 0, -36000, 36000, ParamUnit::Deg);
            SchemaMember& face = k.object("face", "material");
            face.nullable = true;
            face.hash = HashScope::Geometry;
        }
        for (const char* key : {"dz", "tilt_x", "tilt_y", "face_3d", "side_3d", "bands_3d", "rig"}) {
            schema.reserve("char_adjust", key, "S86");
        }
        {
            KindBuilder k(schema, "line_adjust", "Line", HashScope::Geometry);
            k.anim(FxAnim::Relayout).hash(HashScope::Geometry);
            k.number("scale", 1, 0.01, 100);
            k.vec2("offset", 0, 0, ParamUnit::Percent);
            k.number("h_scale", 1, 0.1, 10).label = "Horizontal Scale";
            k.number("v_scale", 1, 0.1, 10).label = "Vertical Scale";
        }
        {
            KindBuilder k(schema, "warp", "Warp", HashScope::Geometry);
            k.anim(FxAnim::Relayout).hash(HashScope::Geometry);
            k.enumeration("style", "none", {"none", "arc", "arc_lower", "arc_upper", "arch", "bulge", "shell_lower",
                                            "shell_upper", "flag", "wave", "fish", "rise", "fisheye", "inflate",
                                            "squeeze", "twist"});
            k.number("bend", 0, -100, 100);
            k.number("horizontal", 0, -100, 100);
            k.number("vertical", 0, -100, 100);
        }
        {
            KindBuilder k(schema, "distort", "Distort", HashScope::Geometry);
            k.anim(FxAnim::Relayout).hash(HashScope::Geometry);
            k.enumeration("mode", "none", {"none", "corners", "mesh"});
            Json corners = Json::array();
            for (int i = 0; i < 4; i++) corners.push_back(Json::array({0, 0}));
            SchemaMember& c = k.add("corners", ValueType::Vec2List, corners);
            c.maxItems = 4;
            c.anim = FxAnim::Relayout;
            Json mesh = Json::array();
            for (int i = 0; i < 16; i++) mesh.push_back(Json::array({0, 0}));
            SchemaMember& m = k.add("mesh", ValueType::Vec2List, mesh);
            m.maxItems = 16;
            m.anim = FxAnim::Relayout;
        }
        {
            KindBuilder k(schema, "layer.shape", "Shape Layer", HashScope::Geometry);
            k.kind().base = "layer";
            k.anim(FxAnim::Relayout).hash(HashScope::Geometry);
            addShapeGeometry(k);
            addOutlineMembers(k);
            addStyleMembers(k);
        }
        schema.reserve("layer.shape", "style_3d", "S86");
        {
            KindBuilder k(schema, "shape_tail", "Tail", HashScope::Geometry);
            k.anim(FxAnim::Relayout);
            k.number("x", 0, -1000, 1000, ParamUnit::Percent);
            k.number("y", -60, -1000, 1000, ParamUnit::Percent);
            k.number("width", 20, 0, 1000, ParamUnit::Percent);
        }
        {
            KindBuilder k(schema, "layer.vector", "Vector (SVG) Layer", HashScope::Geometry);
            k.kind().base = "layer";
            k.hash(HashScope::Geometry);
            SchemaMember& svg = k.asset("svg");
            svg.label = "SVG";
            k.number("size", 200, 1, 16384, ParamUnit::Px).anim = FxAnim::Relayout;
            k.enumeration("mode", "shape", {"shape", "artwork"});
            addOutlineMembers(k);
            addStyleMembers(k);
        }
        schema.reserve("layer.vector", "translations", "S87");
        schema.reserve("layer.vector", "style_3d", "S86");
        {
            KindBuilder k(schema, "layer.image", "Image Layer", HashScope::Geometry);
            k.kind().base = "layer";
            k.hash(HashScope::Geometry);
            k.asset("image");
            k.number("size", 200, 1, 16384, ParamUnit::Px).anim = FxAnim::Relayout;
            k.enumeration("filter", "linear", {"linear", "nearest"});
            k.object("trace", "trace");
            addStyleMembers(k);
        }
        schema.reserve("layer.image", "translations", "S87");
        schema.reserve("layer.image", "style_3d", "S86");
        {
            KindBuilder k(schema, "trace", "Trace", HashScope::Geometry);
            k.integer("threshold", 128, 1, 255);
            k.number("smooth", 0, 0, 100, ParamUnit::Percent);
            k.number("despeckle", 0, 0, 100, ParamUnit::Percent);
        }
        {
            KindBuilder k(schema, "layer.group", "Group", HashScope::Node);
            k.kind().base = "layer";
            k.array("children", "layer", TITLE_MAX_LAYERS).hash = HashScope::Node;
            SchemaMember& collapsed = k.boolean("collapsed", false);
            collapsed.anim = FxAnim::None;
            collapsed.hash = HashScope::Identity;
        }
        {
            KindBuilder k(schema, "layer.generator", "Generator", HashScope::Geometry);
            k.kind().base = "layer";
            k.hash(HashScope::Geometry);
            k.object("generator", "generator_ref");
            k.add("follow", ValueType::LayerRef, "");
        }
        {
            KindBuilder k(schema, "generator_ref", "Generator", HashScope::Geometry);
            SchemaMember& type = k.string("type", "fill");
            type.readOnly = true;
            type.anim = FxAnim::None;
            k.add("params", ValueType::GeneratorParams, Json::object()).label = "Parameters";
        }
        {
            KindBuilder k(schema, "layer.adjustment", "Adjustment", HashScope::Geometry);
            k.kind().base = "layer";
            k.hash(HashScope::Geometry);
            k.object("adjustment", "adjustment");
        }

        // ---- adjustments (the adjustment layer and the `adjust` effect) ----
        {
            KindBuilder k(schema, "adjustment", "Adjustment", HashScope::Geometry);
            k.kind().discriminator = "type";
            SchemaMember& type = k.enumeration("type", "hue_saturation",
                                               {"brightness_contrast", "levels", "curves", "exposure", "vibrance",
                                                "hue_saturation", "color_balance", "black_white", "photo_filter",
                                                "gradient_map", "invert", "posterize", "threshold", "effects_only"});
            type.anim = FxAnim::Step;
            for (const std::string& variant : type.options) k.kind().variants[variant] = "adjustment." + variant;
        }
        auto adjustment = [&schema](const char* name, const char* label) {
            KindBuilder k(schema, (std::string("adjustment.") + name).c_str(), label, HashScope::Geometry);
            k.kind().base = "adjustment";
            return k;
        };
        {
            KindBuilder k = adjustment("brightness_contrast", "Brightness/Contrast");
            k.number("brightness", 0, -150, 150);
            k.number("contrast", 0, -50, 100);
        }
        {
            KindBuilder k = adjustment("levels", "Levels");
            k.object("channels", "levels_channels");
        }
        {
            KindBuilder k(schema, "levels_channels", "Channels", HashScope::Geometry);
            k.object("rgb", "levels_channel").label = "RGB";
            k.object("r", "levels_channel").label = "Red";
            k.object("g", "levels_channel").label = "Green";
            k.object("b", "levels_channel").label = "Blue";
        }
        {
            KindBuilder k(schema, "levels_channel", "Levels Channel", HashScope::Geometry);
            k.number("in_black", 0, 0, 255);
            k.number("in_white", 255, 0, 255);
            k.number("gamma", 1, 0.1, 9.99);
            k.number("out_black", 0, 0, 255);
            k.number("out_white", 255, 0, 255);
        }
        {
            KindBuilder k = adjustment("curves", "Curves");
            k.object("channels", "curves_channels");
        }
        {
            KindBuilder k(schema, "curves_channels", "Channels", HashScope::Geometry);
            k.object("rgb", "curve").label = "RGB";
            k.object("r", "curve").label = "Red";
            k.object("g", "curve").label = "Green";
            k.object("b", "curve").label = "Blue";
        }
        {
            KindBuilder k = adjustment("exposure", "Exposure");
            k.number("exposure", 0, -20, 20);
            k.number("offset", 0, -0.5, 0.5);
            k.number("gamma", 1, 0.01, 9.99);
        }
        {
            KindBuilder k = adjustment("vibrance", "Vibrance");
            k.number("vibrance", 0, -100, 100);
            k.number("saturation", 0, -100, 100);
        }
        {
            KindBuilder k = adjustment("hue_saturation", "Hue/Saturation");
            k.number("hue", 0, -180, 180, ParamUnit::Deg);
            k.number("saturation", 0, -100, 100);
            k.number("lightness", 0, -100, 100);
            k.boolean("colorize", false);
        }
        {
            KindBuilder k = adjustment("color_balance", "Color Balance");
            k.object("shadows", "color_balance_range");
            k.object("midtones", "color_balance_range");
            k.object("highlights", "color_balance_range");
            k.boolean("preserve_luminosity", true);
        }
        {
            KindBuilder k(schema, "color_balance_range", "Tone Range", HashScope::Geometry);
            k.number("cyan_red", 0, -100, 100).label = "Cyan - Red";
            k.number("magenta_green", 0, -100, 100).label = "Magenta - Green";
            k.number("yellow_blue", 0, -100, 100).label = "Yellow - Blue";
        }
        {
            KindBuilder k = adjustment("black_white", "Black & White");
            k.number("reds", 40, -200, 300);
            k.number("yellows", 60, -200, 300);
            k.number("greens", 40, -200, 300);
            k.number("cyans", 60, -200, 300);
            k.number("blues", 20, -200, 300);
            k.number("magentas", 80, -200, 300);
            k.boolean("tint", false);
            k.color("tint_color", "#e1d3b3").label = "Tint Color";
        }
        {
            KindBuilder k = adjustment("photo_filter", "Photo Filter");
            k.color("color", "#ec8a00");
            k.number("density", 25, 0, 100);
            k.boolean("preserve_luminosity", true);
        }
        {
            KindBuilder k = adjustment("gradient_map", "Gradient Map");
            k.object("gradient", "gradient");
            k.boolean("reverse", false);
        }
        adjustment("invert", "Invert");
        {
            KindBuilder k = adjustment("posterize", "Posterize");
            k.integer("levels", 4, 2, 255);
        }
        {
            KindBuilder k = adjustment("threshold", "Threshold");
            k.integer("level", 128, 1, 255);
        }
        adjustment("effects_only", "Effects Only");

        // ---- style and parts ----
        {
            KindBuilder k(schema, "style2d", "Style", HashScope::Stack);
            k.object("face", "material").hash = HashScope::Stack;
            k.array("rims", "rim", TITLE_MAX_RIMS).hash = HashScope::Mask;
            k.boolean("knockout", false).hash = HashScope::Stack;
            k.object("erode", "erode").hash = HashScope::Mask;
        }
        {
            KindBuilder k(schema, "rim", "Rim", HashScope::Mask);
            k.hash(HashScope::Mask);
            k.id();
            k.boolean("enabled", true);
            k.number("width", 3, 0, 100, ParamUnit::Percent);
            k.enumeration("join", "miter", {"miter", "round", "bevel"});
            k.boolean("fill_counters", false);
            k.object("plate", "plate");
            k.vec2("offset", 0, 0, ParamUnit::Percent);
            k.object("material", "material").hash = HashScope::Stack;
        }
        {
            KindBuilder k(schema, "plate", "Plate", HashScope::Mask);
            k.hash(HashScope::Mask);
            k.enumeration("kind", "contour", {"contour", "hull", "box"});
            k.number("close", 0, 0, 100, ParamUnit::Percent);
            k.number("corner", 4, 0, 100, ParamUnit::Percent);
            k.number("slant", 0, -80, 80, ParamUnit::Deg);
        }
        {
            KindBuilder k(schema, "erode", "Erode", HashScope::Mask);
            k.hash(HashScope::Mask);
            k.boolean("enabled", false);
            k.number("amount", 0.5, 0, 1);
            k.number("edge", 6, 0, 100, ParamUnit::Percent);
            k.number("scale", 12, 0.1, 1000, ParamUnit::Percent);
            k.number("angle", 0, -360, 360, ParamUnit::Deg);
            k.integer("seed", 1, 0, 2147483647);
        }

        // ---- material (2D) ----
        {
            KindBuilder k(schema, "material", "Material", HashScope::Stack);
            k.hash(HashScope::Stack);
            k.object("fill", "fill");
            k.number("opacity", 1, 0, 1).label = "Fill Opacity";
            k.object("bevel", "bevel");
            k.object("reflection", "reflection");
            k.object("finish", "finish");
            k.object("pattern", "pattern");
            k.object("satin", "satin");
            k.object("texture", "texture");
            k.object("inner_glow", "material_inner_glow");
            k.object("gloss", "gloss");
            k.number("refraction", 0, -100, 100, ParamUnit::Percent);
            k.object("glass", "glass");
            k.object("sheen", "sheen");
            SchemaMember& link = k.string("link", "");
            link.anim = FxAnim::None;
            link.hash = HashScope::Identity;
        }
        {
            KindBuilder k(schema, "fill", "Fill", HashScope::Stack);
            k.hash(HashScope::Stack);
            k.enumeration("kind", "solid", {"solid", "linear", "radial", "angle", "reflected", "diamond", "shape_burst", "image", "source"});
            k.color("color", "#ffffff");
            k.object("gradient", "gradient");
            k.number("angle", 90, -360, 360, ParamUnit::Deg);
            k.number("scale", 1, 0.01, 100);
            k.number("offset", 0, -1, 1);
            k.enumeration("span", "layer", {"layer", "line", "char"});
            k.object("image", "fill_image");
        }
        {
            KindBuilder k(schema, "fill_image", "Fill Image", HashScope::Stack);
            k.hash(HashScope::Stack);
            k.asset("image");
            k.number("scale", 1, 0.01, 100);
            k.vec2("offset", 0, 0);
            k.number("rotation", 0, -360, 360, ParamUnit::Deg);
            k.boolean("tile", false);
        }
        {
            KindBuilder k(schema, "bevel", "Bevel", HashScope::Stack);
            k.hash(HashScope::Stack);
            k.boolean("enabled", false).hash = HashScope::Field;
            k.enumeration("profile", "smooth", {"smooth", "button", "mesa", "chamfer", "ridge", "cove", "stepped", "custom"}).hash = HashScope::Field;
            k.object("profile_curve", "curve", curveOf({{0, 0}, {0.5, 0.75}, {1, 1}})).hash = HashScope::Field;
            k.number("size", 3, 0, 100, ParamUnit::Percent).hash = HashScope::Field;
            k.enumeration("size_mode", "fixed", {"fixed", "fit"}).hash = HashScope::Field;
            k.number("fit", 70, 0, 100, ParamUnit::Percent).hash = HashScope::Field;
            k.number("depth", 3, 0, 10);
            k.enumeration("direction", "up", {"up", "down"});
            k.number("soften", 0, 0, 100, ParamUnit::Percent).hash = HashScope::Field;
            k.number("light_angle", 120, -360, 360, ParamUnit::Deg);
            k.number("light_altitude", 30, 0, 90, ParamUnit::Deg);
            k.boolean("use_global_light", true);
            k.enumeration("gloss", "ring", {"linear", "ring", "double_ring", "cove", "rounded_steps", "sawtooth", "custom"}).label = "Gloss Contour";
            k.object("gloss_curve", "curve");
            k.color("highlight_color", "#ffffff");
            k.number("highlight_opacity", 0.85, 0, 1);
            k.color("shadow_color", "#000000");
            k.number("shadow_opacity", 0.6, 0, 1);
        }
        {
            KindBuilder k(schema, "reflection", "Reflection", HashScope::Stack);
            k.hash(HashScope::Stack);
            k.boolean("enabled", false);
            k.asset("map");
            k.number("amount", 1, 0, 1);
            k.number("blur", 75, 0, 100);
            k.number("rotation", 0, -360, 360, ParamUnit::Deg);
            k.number("colorize", 0, 0, 1);
        }
        {
            KindBuilder k(schema, "finish", "Finish", HashScope::Stack);
            k.hash(HashScope::Stack);
            k.boolean("enabled", false);
            k.object("gradient", "gradient");
            k.number("amount", 1, 0, 1);
        }
        {
            KindBuilder k(schema, "pattern", "Pattern", HashScope::Stack);
            k.hash(HashScope::Stack);
            k.enumeration("kind", "none", PATTERN_KINDS);
            k.color("color", "#000000");
            k.color("color2", "#ffffff").label = "Color 2";
            k.number("opacity", 1, 0, 1);
            k.number("scale", 12, 0.1, 1000, ParamUnit::Percent);
            k.number("angle", 0, -360, 360, ParamUnit::Deg);
            k.integer("seed", 1, 0, 2147483647);
            k.asset("image");
            k.number("shade", 0.6, 0, 1);
            k.number("relief", 0, 0, 1);
        }
        {
            KindBuilder k(schema, "satin", "Satin", HashScope::Stack);
            k.hash(HashScope::Stack);
            k.boolean("enabled", false).hash = HashScope::Field;
            k.color("color", "#000000");
            k.enumeration("blend", "multiply", {"normal", "multiply", "screen", "overlay"});
            k.number("opacity", 0.5, 0, 1);
            k.number("angle", 19, -360, 360, ParamUnit::Deg);
            k.boolean("use_global_light", false);
            k.number("distance", 5, 0, 100, ParamUnit::Percent).hash = HashScope::Field;
            k.number("size", 7, 0, 100, ParamUnit::Percent).hash = HashScope::Field;
            k.object("contour", "curve", curveOf({{0, 0}, {0.5, 1}, {1, 0}}));
            k.boolean("invert", true);
        }
        {
            KindBuilder k(schema, "texture", "Texture", HashScope::Stack);
            k.hash(HashScope::Stack);
            k.enumeration("kind", "none", {"none", "noise", "sparkle", "brushed", "hairline", "spun", "flakes", "ripple",
                                           "grunge", "clouds", "hammered", "crumple", "liquid"});
            k.number("amount", 0.3, 0, 1);
            k.number("scale", 4, 0.1, 1000, ParamUnit::Percent);
            k.enumeration("blend", "overlay", {"overlay", "soft_light", "screen", "multiply"});
            k.integer("seed", 1, 0, 2147483647);
            k.enumeration("target", "color", {"color", "surface"});
            k.number("angle", 0, -360, 360, ParamUnit::Deg);
        }
        {
            KindBuilder k(schema, "material_inner_glow", "Inner Glow", HashScope::Stack);
            k.hash(HashScope::Stack);
            k.boolean("enabled", false).hash = HashScope::Field;
            k.color("color", "#ffffff");
            k.number("size", 2, 0, 100, ParamUnit::Percent).hash = HashScope::Field;
            k.number("opacity", 0.6, 0, 1);
            k.number("noise", 0, 0, 1);
        }
        {
            KindBuilder k(schema, "gloss", "Gloss", HashScope::Stack);
            k.hash(HashScope::Stack);
            k.boolean("enabled", false);
            k.number("opacity", 0.55, 0, 1);
            k.number("position", 0.45, 0, 1);
            k.number("softness", 0.15, 0, 1);
            k.number("curve", 0.25, -1, 1);
            k.number("angle", 0, -360, 360, ParamUnit::Deg);
        }
        {
            KindBuilder k(schema, "glass", "Glass", HashScope::Stack);
            k.hash(HashScope::Stack);
            k.boolean("enabled", false);
            k.color("color", "#3cc8ff");
            k.number("tint", 1, 0, 1);
            k.number("opacity", 0.7, 0, 1);
            k.number("edge", 0.7, 0, 1);
            k.number("caustic", 0.7, 0, 2);
        }
        {
            KindBuilder k(schema, "sheen", "Sheen", HashScope::Stack);
            k.hash(HashScope::Stack);
            k.boolean("enabled", false);
            k.color("color", "#ffffff");
            k.number("amount", 0.7, 0, 3);
            k.number("position", -0.25, -1, 1);
            k.number("width", 0.35, 0.05, 2);
        }
        {
            KindBuilder k(schema, "gradient", "Gradient", HashScope::Stack);
            k.kind().finish = &finishGradient;
            k.array("stops", "gradient_stop", 32, gradientDefault()["stops"]).anim = FxAnim::None;
            k.enumeration("interp", "rgb", {"rgb", "vivid"}).label = "Interpolation";
        }
        {
            KindBuilder k(schema, "gradient_stop", "Gradient Stop", HashScope::Stack);
            k.number("pos", 0, 0, 1).label = "Position";
            k.color("color", "#000000");
            k.number("alpha", 1, 0, 1);
            k.number("mid", 0.5, 0.05, 0.95).label = "Midpoint";
        }
        {
            KindBuilder k(schema, "curve", "Curve", HashScope::Stack);
            k.kind().finish = &finishCurve;
            k.array("points", "curve_point", 32, curveOf({{0, 0}, {1, 1}})["points"]).anim = FxAnim::None;
        }
        {
            KindBuilder k(schema, "curve_point", "Curve Point", HashScope::Stack);
            k.number("x", 0, 0, 1);
            k.number("y", 0, 0, 1);
            k.boolean("corner", false);
        }

        // ---- masks ----
        {
            KindBuilder k(schema, "mask", "Mask", HashScope::Masks);
            k.kind().discriminator = "type";
            k.hash(HashScope::Masks);
            k.id();
            SchemaMember& type = k.enumeration("type", "shape", {"shape", "gradient", "layer", "image"});
            type.readOnly = true;
            type.anim = FxAnim::None;
            k.boolean("enabled", true);
            k.boolean("invert", false);
            k.number("feather", 0, 0, 1000, ParamUnit::Px);
            k.number("density", 1, 0, 1);
            k.boolean("linked", true);
            for (const char* variant : {"shape", "gradient", "layer", "image"}) {
                k.kind().variants[variant] = std::string("mask.") + variant;
            }
        }
        {
            KindBuilder k(schema, "mask.shape", "Shape Mask", HashScope::Masks);
            k.kind().base = "mask";
            k.hash(HashScope::Masks);
            k.object("shape", "shape_geometry");
            k.vec2("position", 0, 0, ParamUnit::Px);
            k.number("rotation", 0, -36000, 36000, ParamUnit::Deg);
        }
        {
            KindBuilder k(schema, "shape_geometry", "Shape", HashScope::Masks);
            addShapeGeometry(k);
        }
        {
            KindBuilder k(schema, "mask.gradient", "Gradient Mask", HashScope::Masks);
            k.kind().base = "mask";
            k.hash(HashScope::Masks);
            k.enumeration("shape", "linear", {"linear", "radial"});
            k.vec2("position", 0, 0, ParamUnit::Px);
            k.number("angle", 0, -360, 360, ParamUnit::Deg);
            k.number("length", 200, 1, 16384, ParamUnit::Px);
            k.number("inner", 0, 0, 1);
            k.number("mid", 0.5, 0.01, 0.99).label = "Midpoint";
        }
        {
            KindBuilder k(schema, "mask.layer", "Layer Mask", HashScope::Masks);
            k.kind().base = "mask";
            k.hash(HashScope::Masks);
            k.add("layer", ValueType::LayerRef, "");
            k.enumeration("channel", "alpha", {"alpha", "luminance"});
        }
        {
            KindBuilder k(schema, "mask.image", "Image Mask", HashScope::Masks);
            k.kind().base = "mask";
            k.hash(HashScope::Masks);
            k.asset("image");
            k.enumeration("channel", "alpha", {"alpha", "luminance"});
            k.vec2("position", 0, 0, ParamUnit::Px);
            k.number("size", 200, 1, 16384, ParamUnit::Px);
            k.number("rotation", 0, -36000, 36000, ParamUnit::Deg);
        }

        // ---- effect entries ----
        {
            KindBuilder k(schema, "effect", "Effect", HashScope::Effects);
            k.hash(HashScope::Effects);
            k.id();
            SchemaMember& type = k.string("type", "");
            type.readOnly = true;
            type.anim = FxAnim::None;
            k.boolean("enabled", true);
            k.add("params", ValueType::EffectParams, Json::object()).label = "Parameters";
            SchemaMember& fromLook = k.boolean("from_look", false);
            fromLook.anim = FxAnim::None;
            fromLook.hash = HashScope::Identity;
        }

        // ---- exposed parameters and outputs ----
        {
            KindBuilder k(schema, "exposed", "Exposed Parameter", HashScope::Identity);
            k.anim(FxAnim::None).hash(HashScope::Identity);
            k.string("name", "param").pattern = "[a-z][a-z0-9_]{0,31}";
            k.string("label", "");
            k.enumeration("type", "text", {"text", "color", "number", "bool", "font", "image", "color_scheme"});
            k.array("targets", "exposed_target", 0);
            SchemaMember& min = k.number("min", 0, -1e30, 1e30);
            min.nullable = true;
            min.defaultValue = nullptr;
            min.anim = FxAnim::None;
            SchemaMember& max = k.number("max", 0, -1e30, 1e30);
            max.nullable = true;
            max.defaultValue = nullptr;
            max.anim = FxAnim::None;
            k.add("options", ValueType::StringList, Json::array());
        }
        {
            KindBuilder k(schema, "exposed_target", "Target", HashScope::Identity);
            k.anim(FxAnim::None).hash(HashScope::Identity);
            k.add("layer", ValueType::LayerRef, "");
            k.string("field", "");
        }
        {
            KindBuilder k(schema, "output", "Output", HashScope::Identity);
            k.anim(FxAnim::None).hash(HashScope::Identity);
            k.string("id", "main").pattern = "[a-z0-9_]{1,32}";
            k.enumeration("content", "all", {"all", "layers", "ink", "glow", "mask"});
            k.add("layers", ValueType::StringList, Json::array());
            k.string("file", "");
            k.number("scale", 1, 0.1, 8);
            k.boolean("trim", true);
            k.integer("padding", 4, 0, 1024).unit = ParamUnit::Px;
            SchemaMember& roundTo = k.integer("round_to", 4, 1, 8);
            roundTo.allowed = {1, 2, 4, 8};
            SchemaMember& bitDepth = k.integer("bit_depth", 8, 8, 16);
            bitDepth.allowed = {8, 16};
            k.boolean("alpha_bleed", true);
            k.integer("max_size", 4096, 16, 8192).unit = ParamUnit::Px;
        }
    }

    double numberAt(const Json& object, const char* key, double fallback) {
        auto it = object.find(key);
        return it != object.end() && it->is_number() ? it->get<double>() : fallback;
    }

    void finishGradient(Json& gradient, const std::string& path, NormalizeContext& context) {
        Json& stops = gradient["stops"];
        if (!stops.is_array() || stops.size() < 2) {
            context.warn(jsonPathMember(path, "stops"), "a gradient needs 2 stops: the default black-to-white gradient is used");
            stops = gradientDefault()["stops"];
            return;
        }
        std::stable_sort(stops.begin(), stops.end(), [](const Json& a, const Json& b) {
            return numberAt(a, "pos", 0) < numberAt(b, "pos", 0);
        });
    }

    void finishCurve(Json& curve, const std::string& path, NormalizeContext& context) {
        Json& points = curve["points"];
        if (!points.is_array() || points.size() < 2) {
            context.warn(jsonPathMember(path, "points"), "a curve needs 2 points: the identity curve is used");
            points = curveOf({{0, 0}, {1, 1}})["points"];
            return;
        }
        std::stable_sort(points.begin(), points.end(), [](const Json& a, const Json& b) {
            return numberAt(a, "x", 0) < numberAt(b, "x", 0);
        });
        // the ends are always present: the outermost points move onto x = 0 and x = 1
        if (numberAt(points.front(), "x", 0) > 0) {
            context.warn(jsonPathMember(path, "points"), "the first point moved to x = 0");
            points.front()["x"] = 0;
        }
        if (numberAt(points.back(), "x", 1) < 1) {
            context.warn(jsonPathMember(path, "points"), "the last point moved to x = 1");
            points.back()["x"] = 1;
        }
    }

    size_t editDistance(std::string_view a, std::string_view b) {
        std::vector<size_t> row(b.size() + 1);
        for (size_t j = 0; j <= b.size(); j++) row[j] = j;
        for (size_t i = 1; i <= a.size(); i++) {
            size_t diagonal = row[0];
            row[0] = i;
            for (size_t j = 1; j <= b.size(); j++) {
                const size_t above = row[j];
                const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
                row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + cost});
                diagonal = above;
            }
        }
        return row[b.size()];
    }

}

// ---- the program's registrations ----

void jasperengine::titleart::registerProgramMembers([[maybe_unused]] TitleArtSchema& schema) {
    // each later title spec adds one call here (S81, S84-S87), after S80's table
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
    registerTitle3DMembers(schema);
#endif
}

// ---- TitleArtSchema ----

const char* titleart::valueTypeName(ValueType type) {
    switch (type) {
        case ValueType::Number: return "number";
        case ValueType::Integer: return "integer";
        case ValueType::Bool: return "bool";
        case ValueType::String: return "string";
        case ValueType::Enum: return "enum";
        case ValueType::Color: return "color";
        case ValueType::Vec2: return "vec2";
        case ValueType::Vec2List: return "vec2 list";
        case ValueType::AssetRef: return "asset";
        case ValueType::AssetRefList: return "asset list";
        case ValueType::Id: return "id";
        case ValueType::LayerRef: return "layer";
        case ValueType::StringList: return "string list";
        case ValueType::Object: return "object";
        case ValueType::ObjectArray: return "list";
        case ValueType::ObjectMap: return "map";
        case ValueType::ObjectArrayMap: return "map of lists";
        case ValueType::NumberMap: return "number map";
        case ValueType::IntegerMap: return "integer map";
        case ValueType::EffectParams: return "effect params";
        case ValueType::GeneratorParams: return "generator params";
        case ValueType::Json: return "json";
    }
    return "json";
}

TitleArtSchema& TitleArtSchema::get() {
    static TitleArtSchema* schema = [] {
        auto* s = new TitleArtSchema();
        buildS80(*s);
        registerProgramMembers(*s);
        s->reindex();
        return s;
    }();
    return *schema;
}

const SchemaKind* TitleArtSchema::kind(std::string_view name) const {
    auto it = byName.find(name);
    return it != byName.end() ? it->second : nullptr;
}

SchemaKind* TitleArtSchema::mutableKind(std::string_view name) {
    auto it = byName.find(name);
    return it != byName.end() ? it->second : nullptr;
}

void TitleArtSchema::reindex() {
    byName.clear();
    for (auto& k : kinds) byName[k->name] = k.get();
    resolved.clear();
    for (auto& k : kinds) {
        std::vector<const SchemaMember*>& out = resolved[k.get()];
        if (!k->base.empty()) {
            if (const SchemaKind* base = kind(k->base)) {
                for (const SchemaMember& m : base->members) out.push_back(&m);
            }
        }
        for (const SchemaMember& m : k->members) out.push_back(&m);
    }
}

const SchemaKind* TitleArtSchema::variantOf(const SchemaKind& base, const Json& object) const {
    if (base.discriminator.empty()) return &base;
    if (!object.is_object()) return nullptr;
    auto it = object.find(base.discriminator);
    if (it == object.end() || !it->is_string()) return nullptr;
    auto variant = base.variants.find(it->get<std::string>());
    return variant != base.variants.end() ? kind(variant->second) : nullptr;
}

const std::vector<const SchemaMember*>& TitleArtSchema::membersOf(const SchemaKind& k) const {
    static const std::vector<const SchemaMember*> none;
    auto it = resolved.find(&k);
    return it != resolved.end() ? it->second : none;
}

const SchemaMember* TitleArtSchema::member(const SchemaKind& k, std::string_view key) const {
    for (const SchemaMember& m : k.members) {
        if (m.key == key) return &m;
    }
    if (!k.base.empty()) {
        if (const SchemaKind* base = kind(k.base)) return member(*base, key);
    }
    return nullptr;
}

Json TitleArtSchema::defaultObject(std::string_view kindName, std::string_view variant) const {
    Json object = Json::object();
    const SchemaKind* k = kind(kindName);
    if (k && !k->discriminator.empty() && !variant.empty()) object[k->discriminator] = std::string(variant);
    NormalizeContext context;
    normalizeTitleArtObject(object, std::string(kindName), "", context);
    return object;
}

bool TitleArtSchema::addKind(SchemaKind k) {
    if (k.name.empty() || kind(k.name)) return false;
    kinds.push_back(std::make_unique<SchemaKind>(std::move(k)));
    reindex();
    return true;
}

bool TitleArtSchema::addMember(std::string_view kindName, SchemaMember m) {
    SchemaKind* k = mutableKind(kindName);
    if (!k || m.key.empty() || member(*k, m.key)) return false;
    reserved.erase(std::remove_if(reserved.begin(), reserved.end(), [&](const SchemaReservation& r) {
        return r.kind == kindName && r.key == m.key;
    }), reserved.end());
    if (m.label.empty()) m.label = labelOf(m.key);
    k->members.push_back(std::move(m));
    reindex();
    return true;
}

bool TitleArtSchema::addVariant(std::string_view baseKind, std::string_view value, std::string_view variantKind) {
    SchemaKind* k = mutableKind(baseKind);
    if (!k || k->discriminator.empty() || !kind(variantKind)) return false;
    if (!k->variants.emplace(std::string(value), std::string(variantKind)).second) return false;
    // the discriminator's enum takes the value
    for (SchemaMember& m : k->members) {
        if (m.key == k->discriminator && m.type == ValueType::Enum) m.options.push_back(std::string(value));
    }
    return true;
}

bool TitleArtSchema::removeMember(std::string_view kindName, std::string_view key) {
    SchemaKind* k = mutableKind(kindName);
    if (!k) return false;
    auto it = std::find_if(k->members.begin(), k->members.end(), [&](const SchemaMember& m) { return m.key == key; });
    if (it == k->members.end()) return false;
    k->members.erase(it);
    reindex();
    return true;
}

bool TitleArtSchema::removeKind(std::string_view name) {
    auto it = std::find_if(kinds.begin(), kinds.end(), [&](const auto& k) { return k->name == name; });
    if (it == kinds.end()) return false;
    kinds.erase(it);
    reindex();
    return true;
}

void TitleArtSchema::reserve(std::string_view kindName, std::string_view key, std::string_view owner) {
    if (!reservedOwner(kindName, key).empty()) return;
    reserved.push_back({std::string(kindName), std::string(key), std::string(owner)});
}

std::string TitleArtSchema::reservedOwner(std::string_view kindName, std::string_view key) const {
    for (const SchemaReservation& r : reserved) {
        if (r.kind == kindName && r.key == key) return r.owner;
    }
    // a variant's base reservations apply to it too
    if (const SchemaKind* k = kind(kindName)) {
        if (!k->base.empty()) return reservedOwner(k->base, key);
    }
    return {};
}

std::vector<std::string> TitleArtSchema::kindNames() const {
    std::vector<std::string> out;
    for (const auto& k : kinds) out.push_back(k->name);
    return out;
}

namespace {

    void collectPatterns(const TitleArtSchema& schema, const SchemaKind& kind, const std::string& prefix, int depth,
                         std::vector<std::string>& out) {
        if (depth > 24) return;
        for (const SchemaMember* m : schema.membersOf(kind)) {
            const std::string path = prefix.empty() ? m->key : prefix + "." + m->key;
            const SchemaKind* child = m->kind.empty() ? nullptr : schema.kind(m->kind);
            switch (m->type) {
                case ValueType::Object:
                    if (child && child->discriminator.empty()) collectPatterns(schema, *child, path, depth + 1, out);
                    else out.push_back(path);
                    break;
                case ValueType::ObjectArray:
                    if (child && child->discriminator.empty()) collectPatterns(schema, *child, path + "[*]", depth + 1, out);
                    else out.push_back(path + "[*]");
                    break;
                case ValueType::ObjectMap:
                    if (child && child->discriminator.empty()) collectPatterns(schema, *child, path + ".*", depth + 1, out);
                    else out.push_back(path + ".*");
                    break;
                case ValueType::ObjectArrayMap:
                    out.push_back(path + ".*[*]");
                    break;
                case ValueType::NumberMap:
                case ValueType::IntegerMap:
                    out.push_back(path + ".*");
                    break;
                default:
                    out.push_back(path);
                    break;
            }
        }
    }

}

std::vector<std::string> TitleArtSchema::patterns(std::string_view kindName) const {
    std::vector<std::string> out;
    if (const SchemaKind* k = kind(kindName)) collectPatterns(*this, *k, "", 0, out);
    return out;
}

const SchemaMember* TitleArtSchema::resolvePattern(std::string_view kindName, std::string_view pattern) const {
    std::vector<FieldSegment> segments;
    std::string text(pattern);
    // [*] reads as an index segment, * as a map key
    for (size_t at = text.find("[*]"); at != std::string::npos; at = text.find("[*]")) text.replace(at, 3, "[0]");
    if (!parseFieldPath(text, segments, nullptr)) return nullptr;
    const SchemaKind* k = kind(kindName);
    const SchemaMember* found = nullptr;
    for (size_t i = 0; i < segments.size() && k; i++) {
        const FieldSegment& s = segments[i];
        if (s.isIndex) return nullptr;
        found = member(*k, s.key);
        if (!found) return nullptr;
        if (i + 1 == segments.size()) return found;
        const SchemaKind* child = found->kind.empty() ? nullptr : kind(found->kind);
        switch (found->type) {
            case ValueType::Object:
                k = child;
                break;
            case ValueType::ObjectArray:
                if (!segments[i + 1].isIndex) return nullptr;
                i++;
                k = child;
                break;
            case ValueType::ObjectMap:
                i++;    // the key
                k = child;
                break;
            case ValueType::ObjectArrayMap:
                i += 2; // the key and the index
                if (i >= segments.size() || !segments[i].isIndex) return nullptr;
                k = child;
                break;
            case ValueType::NumberMap:
            case ValueType::IntegerMap:
                // the key is the last segment: the entries are the member's values
                return i + 2 == segments.size() && !segments[i + 1].isIndex ? found : nullptr;
            default:
                return nullptr;
        }
        if (i + 1 >= segments.size()) return found;
    }
    return nullptr;
}

// ---- field paths ----

bool titleart::parseFieldPath(std::string_view path, std::vector<FieldSegment>& out, std::string* error) {
    out.clear();
    auto fail = [error](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (path.empty()) return fail("the path is empty");
    size_t i = 0;
    while (i < path.size()) {
        if (path[i] == '[') {
            const size_t close = path.find(']', i);
            if (close == std::string_view::npos) return fail("'[' without ']' in '" + std::string(path) + "'");
            const std::string_view digits = path.substr(i + 1, close - i - 1);
            if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                return fail("'[" + std::string(digits) + "]' is not an index in '" + std::string(path) + "'");
            }
            FieldSegment s;
            s.isIndex = true;
            s.index = (size_t)std::stoull(std::string(digits));
            out.push_back(s);
            i = close + 1;
            if (i < path.size() && path[i] == '.') {
                i++;
                if (i == path.size()) return fail("'" + std::string(path) + "' ends with '.'");
            }
            continue;
        }
        size_t end = i;
        while (end < path.size() && path[end] != '.' && path[end] != '[') end++;
        if (end == i) return fail("an empty segment in '" + std::string(path) + "'");
        FieldSegment s;
        s.key = std::string(path.substr(i, end - i));
        out.push_back(s);
        i = end;
        if (i < path.size() && path[i] == '.') {
            i++;
            if (i == path.size()) return fail("'" + std::string(path) + "' ends with '.'");
        }
    }
    return true;
}

std::string titleart::formatFieldPath(const std::vector<FieldSegment>& path) {
    std::string out;
    for (const FieldSegment& s : path) {
        if (s.isIndex) {
            out += "[" + std::to_string(s.index) + "]";
        } else {
            if (!out.empty()) out += ".";
            out += s.key;
        }
    }
    return out;
}

std::string titleart::nearestName(std::string_view word, const std::vector<std::string>& candidates) {
    std::string best;
    // at most a third of the word (at least one) plus one edit: a transposition is two
    size_t bestDistance = std::max<size_t>(1, word.size() / 3) + 2;
    for (const std::string& c : candidates) {
        size_t d = editDistance(word, c);
        // a candidate that contains the word (or the other way round) is close
        if (c.find(word) != std::string::npos || (!c.empty() && word.find(c) != std::string_view::npos)) d = std::min<size_t>(d, 1);
        if (d < bestDistance) {
            best = c;
            bestDistance = d;
        }
    }
    return best;
}

namespace {

    bool validMapKey(MapKeys keys, const std::string& key) {
        if (key.empty()) return false;
        switch (keys) {
            case MapKeys::Any: return true;
            case MapKeys::Index:
                return key.size() <= 9 && std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
                       (key.size() == 1 || key[0] != '0');
            case MapKeys::Tag:
                return key.size() == 4 && std::all_of(key.begin(), key.end(), [](char c) { return c >= 32 && c < 127; });
            case MapKeys::PartKey:
                return key == "face" || key == "bevel" || key == "side" || key.rfind("rim:", 0) == 0 || key.rfind("band:", 0) == 0;
            case MapKeys::LanguageCode:
                return std::all_of(key.begin(), key.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '-'; });
        }
        return false;
    }

    std::vector<std::string> memberKeys(const TitleArtSchema& schema, const SchemaKind& kind) {
        std::vector<std::string> keys;
        for (const SchemaMember* m : schema.membersOf(kind)) keys.push_back(m->key);
        return keys;
    }

    std::vector<std::string> objectKeys(const Json& object) {
        std::vector<std::string> keys;
        if (object.is_object()) {
            for (auto it = object.begin(); it != object.end(); ++it) keys.push_back(it.key());
        }
        return keys;
    }

    const SchemaKind* paramKindOf(const TitleArtSchema& schema, ParamKind kind) {
        switch (kind) {
            case ParamKind::Gradient: return schema.kind("gradient");
            case ParamKind::Curve: return schema.kind("curve");
            case ParamKind::Adjustment: return schema.kind("adjustment");
            case ParamKind::Material: return schema.kind("material");
            default: return nullptr;
        }
    }

}

FieldLookup titleart::lookupField(Json& object, const SchemaKind& rootKind, const std::vector<FieldSegment>& path, bool createEntries) {
    const TitleArtSchema& schema = TitleArtSchema::get();
    FieldLookup result;
    std::string longPath, pattern;
    auto appendKey = [&](const std::string& key, bool wildcard) {
        if (!longPath.empty()) longPath += ".";
        longPath += key;
        if (!pattern.empty()) pattern += ".";
        pattern += wildcard ? "*" : key;
    };
    auto appendIndex = [&](size_t index) {
        longPath += "[" + std::to_string(index) + "]";
        pattern += "[*]";
    };
    auto fail = [&](size_t segment, const std::string& code, const std::string& message, std::vector<std::string> valid) {
        result.ok = false;
        result.failedSegment = segment;
        result.code = code;
        result.error = message;
        if (segment < path.size() && !path[segment].isIndex) result.nearest = nearestName(path[segment].key, valid);
        result.validHere = std::move(valid);
        result.longPath = longPath;
        return result;
    };
    if (path.empty()) return fail(0, "bad_path", "the path is empty", {});

    Json* current = &object;
    const SchemaKind* kind = schema.variantOf(rootKind, object);
    if (!kind) return fail(0, "unknown_type", "the object's type is not one this build knows", {});
    // inside effect / generator params: the definition's params
    const std::vector<ParamDef>* params = nullptr;
    for (size_t i = 0; i < path.size(); i++) {
        const FieldSegment& s = path[i];
        const bool last = i + 1 == path.size();
        if (params) {
            if (s.isIndex) return fail(i, "bad_path", "parameters are named, not indexed", {});
            const ParamDef* param = findParam(*params, s.key);
            std::vector<std::string> keys;
            for (const ParamDef& p : *params) keys.push_back(p.key);
            if (!param) return fail(i, "bad_path", "'" + s.key + "' is not a parameter here", keys);
            appendKey(s.key, false);
            Json& slot = (*current)[s.key];
            params = nullptr;
            if (last) {
                result.ok = true;
                result.value = &slot;
                result.param = param;
                result.owner = kind;
                result.longPath = longPath;
                result.pattern = pattern;
                return result;
            }
            const SchemaKind* child = paramKindOf(schema, param->kind);
            if (!child || !slot.is_object()) return fail(i + 1, "bad_path", "'" + s.key + "' is a " + paramKindName(param->kind) + ", not an object", {});
            current = &slot;
            kind = schema.variantOf(*child, slot);
            if (!kind) return fail(i + 1, "unknown_type", "'" + s.key + "' has a type this build does not know", {});
            continue;
        }
        if (s.isIndex) return fail(i, "bad_path", "'" + longPath + "' is not a list", memberKeys(schema, *kind));
        const SchemaMember* member = schema.member(*kind, s.key);
        if (!member) {
            const std::string owner = schema.reservedOwner(kind->name, s.key);
            if (!owner.empty()) return fail(i, "not_available", "'" + s.key + "' arrives with " + owner, memberKeys(schema, *kind));
            return fail(i, "bad_path", "'" + s.key + "' is not a member of " + kind->label, memberKeys(schema, *kind));
        }
        appendKey(s.key, false);
        Json& slot = (*current)[s.key];
        if (last) {
            result.ok = true;
            result.value = &slot;
            result.member = member;
            result.owner = kind;
            result.longPath = longPath;
            result.pattern = pattern;
            return result;
        }
        const SchemaKind* child = member->kind.empty() ? nullptr : schema.kind(member->kind);
        switch (member->type) {
            case ValueType::Object: {
                if (!child) return fail(i + 1, "bad_path", "'" + s.key + "' has no members", {});
                if (slot.is_null()) {
                    if (!createEntries) return fail(i + 1, "bad_path", "'" + longPath + "' is null: set it to an object first", {});
                    slot = schema.defaultObject(child->name);
                }
                current = &slot;
                kind = schema.variantOf(*child, slot);
                if (!kind) return fail(i + 1, "unknown_type", "'" + longPath + "' has a type this build does not know", {});
                break;
            }
            case ValueType::ObjectArray: {
                const FieldSegment& index = path[i + 1];
                const size_t size = slot.is_array() ? slot.size() : 0;
                if (!index.isIndex) {
                    return fail(i + 1, "bad_path", "'" + longPath + "' is a list: index it ([0]" + (size > 1 ? "..[" + std::to_string(size - 1) + "]" : "") + ")", {});
                }
                if (index.index >= size) {
                    return fail(i + 1, "out_of_range", "'" + longPath + "' has " + std::to_string(size) + " item" + (size == 1 ? "" : "s") +
                                (size ? ": valid indexes are 0.." + std::to_string(size - 1) : ""), {});
                }
                appendIndex(index.index);
                i++;
                current = &slot[index.index];
                if (!child) return fail(i, "bad_path", "'" + longPath + "' has no members", {});
                kind = schema.variantOf(*child, *current);
                if (!kind) return fail(i + 1, "unknown_type", "'" + longPath + "' has a type this build does not know", {});
                if (i + 1 == path.size()) {
                    result.ok = true;
                    result.value = current;
                    result.member = member;
                    result.owner = kind;
                    result.longPath = longPath;
                    result.pattern = pattern;
                    return result;
                }
                break;
            }
            case ValueType::ObjectMap:
            case ValueType::ObjectArrayMap:
            case ValueType::NumberMap:
            case ValueType::IntegerMap: {
                const FieldSegment& key = path[i + 1];
                if (key.isIndex || !validMapKey(member->mapKeys, key.key)) {
                    return fail(i + 1, "bad_path", "'" + (key.isIndex ? "[" + std::to_string(key.index) + "]" : key.key) + "' is not a key of '" + longPath + "'", objectKeys(slot));
                }
                if (!slot.is_object()) slot = Json::object();
                const bool exists = slot.contains(key.key);
                if (!exists && !createEntries) return fail(i + 1, "bad_path", "'" + longPath + "' has no '" + key.key + "'", objectKeys(slot));
                appendKey(key.key, true);
                i++;
                Json& entry = slot[key.key];
                if (member->type == ValueType::NumberMap || member->type == ValueType::IntegerMap) {
                    if (i + 1 != path.size()) return fail(i + 1, "bad_path", "'" + longPath + "' is a number", {});
                    if (!exists) entry = 0;
                    result.ok = true;
                    result.value = &entry;
                    result.member = member;
                    result.owner = kind;
                    result.longPath = longPath;
                    result.pattern = pattern;
                    return result;
                }
                if (member->type == ValueType::ObjectArrayMap) {
                    if (!exists) entry = Json::array();
                    if (i + 1 == path.size()) {
                        result.ok = true;
                        result.value = &entry;
                        result.member = member;
                        result.owner = kind;
                        result.longPath = longPath;
                        result.pattern = pattern;
                        return result;
                    }
                    const FieldSegment& index = path[i + 1];
                    if (!index.isIndex || index.index >= entry.size()) {
                        return fail(i + 1, "out_of_range", "'" + longPath + "' has " + std::to_string(entry.size()) + " effects", {});
                    }
                    appendIndex(index.index);
                    i++;
                    current = &entry[index.index];
                } else {
                    if (!exists) entry = schema.defaultObject(member->kind);
                    current = &entry;
                }
                if (!child) return fail(i, "bad_path", "'" + longPath + "' has no members", {});
                kind = schema.variantOf(*child, *current);
                if (!kind) return fail(i + 1, "unknown_type", "'" + longPath + "' has a type this build does not know", {});
                if (i + 1 == path.size()) {
                    result.ok = true;
                    result.value = current;
                    result.member = member;
                    result.owner = kind;
                    result.longPath = longPath;
                    result.pattern = pattern;
                    return result;
                }
                break;
            }
            case ValueType::EffectParams:
            case ValueType::GeneratorParams: {
                const std::string type = current->value("type", std::string());
                if (member->type == ValueType::EffectParams) {
                    const EffectDef* def = findEffect(type);
                    if (!def) return fail(i + 1, "unknown_type", "effect type '" + type + "' is not one this build knows", {});
                    params = &def->params;
                } else {
                    const GeneratorDef* def = findGenerator(type);
                    if (!def) return fail(i + 1, "unknown_type", "generator type '" + type + "' is not one this build knows", {});
                    params = &def->params;
                }
                if (!slot.is_object()) slot = Json::object();
                current = &slot;
                break;
            }
            default:
                return fail(i + 1, "bad_path", "'" + longPath + "' is a " + valueTypeName(member->type) + ", not an object", {});
        }
    }
    return fail(path.size(), "bad_path", "the path ends inside an object", {});
}

// ---- walking members ----

void titleart::forEachMember(Json& object, const SchemaKind& base, const MemberVisitor& visit, const std::string& path) {
    const TitleArtSchema& schema = TitleArtSchema::get();
    const SchemaKind* kind = schema.variantOf(base, object);
    if (!kind || !object.is_object()) return;
    for (const SchemaMember* m : schema.membersOf(*kind)) {
        auto it = object.find(m->key);
        if (it == object.end()) continue;
        Json& value = *it;
        const std::string at = path.empty() ? m->key : path + "." + m->key;
        if (!visit(m, nullptr, value, at)) continue;
        const SchemaKind* child = m->kind.empty() ? nullptr : schema.kind(m->kind);
        switch (m->type) {
            case ValueType::Object:
                if (child && value.is_object()) forEachMember(value, *child, visit, at);
                break;
            case ValueType::ObjectArray:
                if (!child || !value.is_array()) break;
                for (size_t i = 0; i < value.size(); i++) forEachMember(value[i], *child, visit, at + "[" + std::to_string(i) + "]");
                break;
            case ValueType::ObjectMap:
                if (!child || !value.is_object()) break;
                for (auto e = value.begin(); e != value.end(); ++e) forEachMember(e.value(), *child, visit, at + "." + e.key());
                break;
            case ValueType::ObjectArrayMap:
                if (!child || !value.is_object()) break;
                for (auto e = value.begin(); e != value.end(); ++e) {
                    if (!e.value().is_array()) continue;
                    for (size_t i = 0; i < e.value().size(); i++) {
                        forEachMember(e.value()[i], *child, visit, at + "." + e.key() + "[" + std::to_string(i) + "]");
                    }
                }
                break;
            case ValueType::EffectParams:
            case ValueType::GeneratorParams: {
                if (!value.is_object()) break;
                const std::string type = object.value("type", std::string());
                const std::vector<ParamDef>* params = nullptr;
                if (m->type == ValueType::EffectParams) {
                    if (const EffectDef* def = findEffect(type)) params = &def->params;
                } else {
                    if (const GeneratorDef* def = findGenerator(type)) params = &def->params;
                }
                if (!params) break;
                for (const ParamDef& p : *params) {
                    auto v = value.find(p.key);
                    if (v == value.end()) continue;
                    const std::string paramPath = at + "." + p.key;
                    if (!visit(nullptr, &p, *v, paramPath)) continue;
                    if (const SchemaKind* objectKind = paramKindOf(schema, p.kind)) {
                        if (v->is_object()) forEachMember(*v, *objectKind, visit, paramPath);
                    }
                }
                break;
            }
            default:
                break;
        }
    }
}
