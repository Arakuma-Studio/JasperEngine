// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleRows.h"

#include "profiler/Profile.h"
#include "titleart/TitleArtIO.h"
#include "widget/ColorField.h"
#include "widget/CurveField.h"
#include "widget/RampField.h"

#include "math/Curve.h"
#include "math/Vector3.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <set>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    // ---- the row table: editor-only data per member ----

    bool fillGradient(const ta::Json& o) {
        const std::string k = o.value("kind", std::string("solid"));
        return k != "solid" && k != "image" && k != "source";
    }
    bool fillSolid(const ta::Json& o) { return o.value("kind", std::string("solid")) == "solid"; }
    bool fillImage(const ta::Json& o) { return o.value("kind", std::string("solid")) == "image"; }
    bool fillAngled(const ta::Json& o) {
        const std::string k = o.value("kind", std::string("solid"));
        return k == "linear" || k == "reflected" || k == "angle" || k == "diamond";
    }
    bool bevelFixed(const ta::Json& o) { return o.value("size_mode", std::string("fixed")) == "fixed"; }
    bool bevelFit(const ta::Json& o) { return o.value("size_mode", std::string("fixed")) == "fit"; }
    bool bevelCustom(const ta::Json& o) { return o.value("profile", std::string("smooth")) == "custom"; }
    bool glossCustom(const ta::Json& o) { return o.value("gloss", std::string("ring")) == "custom"; }
    bool patternOn(const ta::Json& o) { return o.value("kind", std::string("none")) != "none"; }
    bool patternImage(const ta::Json& o) { return o.value("kind", std::string("none")) == "custom"; }
    bool patternTwoColor(const ta::Json& o) {
        const std::string k = o.value("kind", std::string("none"));
        return k != "none" && k != "custom";
    }
    bool textureOn(const ta::Json& o) { return o.value("kind", std::string("none")) != "none"; }
    bool warpOn(const ta::Json& o) { return o.value("style", std::string("none")) != "none"; }
    bool distortCorners(const ta::Json& o) { return o.value("mode", std::string("none")) == "corners"; }
    bool distortMesh(const ta::Json& o) { return o.value("mode", std::string("none")) == "mesh"; }
    bool shapePoints(const ta::Json& o) {
        const std::string s = o.value("shape", std::string("rect"));
        return s == "polygon" || s == "star" || s == "burst" || s == "shout";
    }
    bool shapeInner(const ta::Json& o) {
        const std::string s = o.value("shape", std::string("rect"));
        return s == "star" || s == "burst" || s == "shout";
    }
    bool shapeJitter(const ta::Json& o) {
        const std::string s = o.value("shape", std::string("rect"));
        return s == "burst" || s == "shout";
    }
    bool shapeCorner(const ta::Json& o) {
        const std::string s = o.value("shape", std::string("rect"));
        return s == "rect" || s == "parallelogram" || s == "polygon" || s == "star" || s == "shield" || s == "speech" || s == "ribbon";
    }
    bool shapeSlant(const ta::Json& o) { return o.value("shape", std::string("rect")) == "parallelogram"; }
    bool shapeTail(const ta::Json& o) { return o.value("shape", std::string("rect")) == "speech"; }
    bool vectorShape(const ta::Json& o) { return o.value("mode", std::string("shape")) == "shape"; }
    bool textVertical(const ta::Json& o) { return o.value("vertical", false); }
    bool canvasOpaque(const ta::Json& o) { return !o.value("transparent", true); }
    bool exposedNumber(const ta::Json& o) { return o.value("type", std::string()) == "number"; }

    const std::map<std::string, RowSpec>& table() {
        static const std::map<std::string, RowSpec> rows = [] {
            std::map<std::string, RowSpec> t;
            using ui::SliderMap;
            auto hide = [&t](const char* key) { t[key].hidden = true; };
            // members drawn elsewhere: the header, the Layers panel, handles, their own sections
            for (const char* key : {"layer.id", "layer.type", "layer.name", "layer.visible", "layer.locked", "layer.guide", "layer.blend",
                                    "layer.opacity", "layer.clip", "layer.transform", "layer.masks", "layer.effects", "layer.look",
                                    "layer.group.children", "layer.group.collapsed", "layer.text.chars", "layer.text.lines",
                                    "layer.text.style", "layer.text.part_effects", "layer.text.dimension", "layer.text.warp",
                                    "layer.text.distort", "layer.shape.style", "layer.shape.part_effects", "layer.shape.dimension",
                                    "layer.shape.warp", "layer.shape.distort", "layer.vector.style", "layer.vector.part_effects",
                                    "layer.vector.dimension", "layer.vector.warp", "layer.vector.distort", "layer.image.style",
                                    "layer.image.part_effects", "layer.image.dimension", "layer.image.trace", "layer.text.font",
                                    "layer.text.face_index", "layer.text.fallback_fonts", "layer.text.variations", "layer.text.features",
                                    "layer.generator.generator", "layer.adjustment.adjustment", "material.link", "rim.id",
                                    "rim.material", "rim.plate", "effect.id", "effect.type", "effect.params", "effect.from_look",
                                    "effect.enabled", "mask.id", "mask.type", "document.format", "document.version", "document.guid",
                                    "document.layers", "document.material_links", "document.exposed", "document.outputs",
                                    "document.canvas", "document.global_light", "document.name", "material.fill", "material.bevel",
                                    "material.reflection", "material.finish", "material.pattern", "material.satin", "material.texture",
                                    "material.inner_glow", "material.gloss", "material.glass", "material.sheen", "style2d.face",
                                    "style2d.rims", "style2d.erode", "fill.gradient", "fill.image", "bevel.profile_curve",
                                    "bevel.gloss_curve", "satin.contour", "output.id"}) {
                hide(key);
            }
            // text
            t["layer.text.text"] = {.label = "Text", .noSlider = true, .aliases = "words|title|文字|テキスト"};
            t["layer.text.size"] = {.label = "Font Size", .uiMin = 8, .uiMax = 1000, .slider = SliderMap::Log, .aliases = "size|文字サイズ"};
            t["layer.text.tracking"] = {.uiMin = -200, .uiMax = 400, .aliases = "letter spacing|字間|トラッキング"};
            t["layer.text.line_height"] = {.uiMin = 0.5, .uiMax = 3, .aliases = "leading|行間"};
            t["layer.text.align"] = {.label = "Alignment"};
            t["layer.text.vertical"] = {.label = "Vertical", .aliases = "縦書き|tategaki"};
            t["layer.text.tate_chu_yoko"] = {.when = textVertical, .aliases = "縦中横"};
            t["layer.text.tate_chu_yoko_max"] = {.when = textVertical};
            t["layer.text.proportional"] = {.uiMin = 0, .uiMax = 1, .aliases = "詰め|palt|proportional spacing"};
            t["layer.text.h_scale"] = {.uiMin = 0.25, .uiMax = 4, .slider = SliderMap::Log, .aliases = "長体|平体|width|horizontal scale"};
            t["layer.text.v_scale"] = {.uiMin = 0.25, .uiMax = 4, .slider = SliderMap::Log, .aliases = "長体|平体|height|vertical scale"};
            t["layer.text.shear"] = {.label = "Slant", .uiMin = -45, .uiMax = 45, .aliases = "italic|oblique|斜体"};
            t["layer.text.language"] = {.aliases = "locale|言語"};
            // shapes (layers and shape masks)
            for (const char* kind : {"layer.shape", "shape_geometry"}) {
                const std::string k = kind;
                t[k + ".size"] = {.uiMin = 8, .uiMax = 2000, .slider = SliderMap::Log};
                t[k + ".aspect"] = {.uiMin = 0.1, .uiMax = 10, .slider = SliderMap::Log};
                t[k + ".corner"] = {.uiMin = 0, .uiMax = 50, .when = shapeCorner};
                t[k + ".slant"] = {.when = shapeSlant};
                t[k + ".points"] = {.uiMin = 3, .uiMax = 24, .when = shapePoints};
                t[k + ".inner"] = {.label = "Inner Radius", .when = shapeInner};
                t[k + ".jitter"] = {.when = shapeJitter};
                t[k + ".seed"] = {.noSlider = true, .when = shapeJitter};
                t[k + ".tail"] = {.hidden = true};
            }
            t["layer.shape.weight"] = {.uiMin = -10, .uiMax = 10};
            t["layer.text.weight"] = {.uiMin = -10, .uiMax = 10, .aliases = "bold|thickness|太さ"};
            t["layer.vector.weight"] = {.uiMin = -10, .uiMax = 10, .when = vectorShape};
            t["layer.vector.size"] = {.uiMin = 8, .uiMax = 2000, .slider = SliderMap::Log};
            t["layer.image.size"] = {.uiMin = 8, .uiMax = 2000, .slider = SliderMap::Log};
            t["shape_tail.x"] = {.uiMin = -150, .uiMax = 150};
            t["shape_tail.y"] = {.uiMin = -150, .uiMax = 150};
            t["shape_tail.width"] = {.uiMin = 0, .uiMax = 100};
            // transform
            t["transform.rotation"] = {.uiMin = -180, .uiMax = 180, .aliases = "angle|回転"};
            t["transform.scale"] = {.uiMin = 0.05, .uiMax = 10, .slider = SliderMap::Log, .aliases = "size|拡大|縮小"};
            t["transform.position"] = {.aliases = "x|y|location|move|位置"};
            // warp, distort
            t["warp.style"] = {.aliases = "arc|bend|curve|ワープ|アーチ"};
            t["warp.bend"] = {.when = warpOn};
            t["warp.horizontal"] = {.label = "Horizontal Distortion", .when = warpOn};
            t["warp.vertical"] = {.label = "Vertical Distortion", .when = warpOn};
            t["distort.corners"] = {.link = true, .when = distortCorners};
            t["distort.mesh"] = {.link = true, .when = distortMesh};
            // style
            t["style2d.knockout"] = {.aliases = "cut out|ノックアウト"};
            t["rim.width"] = {.uiMin = 0, .uiMax = 30, .aliases = "outline|stroke|border|thickness|フチ|縁"};
            t["rim.offset"] = {.aliases = "shift|ずらし"};
            t["plate.close"] = {.uiMin = 0, .uiMax = 30};
            t["plate.corner"] = {.uiMin = 0, .uiMax = 30};
            t["erode.amount"] = {.aliases = "かすれ|dry brush|grunge|wear"};
            t["erode.edge"] = {.uiMin = 0, .uiMax = 30};
            t["erode.scale"] = {.uiMin = 0.5, .uiMax = 200, .slider = SliderMap::Log};
            t["erode.seed"] = {.noSlider = true};
            // material
            t["fill.color"] = {.when = fillSolid, .aliases = "colour|色"};
            t["fill.angle"] = {.when = fillAngled};
            t["fill.scale"] = {.uiMin = 0.1, .uiMax = 10, .slider = SliderMap::Log, .when = fillGradient};
            t["fill.offset"] = {.when = fillGradient};
            t["fill.span"] = {.when = fillGradient};
            t["fill.kind"] = {.label = "Fill", .aliases = "gradient|グラデーション|塗り"};
            t["bevel.size"] = {.uiMin = 0, .uiMax = 30, .when = bevelFixed, .aliases = "ベベル|emboss"};
            t["bevel.fit"] = {.when = bevelFit};
            t["bevel.soften"] = {.uiMin = 0, .uiMax = 30, .aliases = "ぼかし|smooth"};
            t["bevel.gloss"] = {.aliases = "contour|光沢"};
            t["bevel.use_global_light"] = {.hidden = true};
            t["satin.use_global_light"] = {.hidden = true};
            t["satin.distance"] = {.uiMin = 0, .uiMax = 30};
            t["satin.size"] = {.uiMin = 0, .uiMax = 30};
            t["pattern.color"] = {.when = patternTwoColor};
            t["pattern.color2"] = {.when = patternTwoColor};
            t["pattern.opacity"] = {.when = patternOn};
            t["pattern.scale"] = {.uiMin = 1, .uiMax = 200, .slider = SliderMap::Log, .when = patternOn};
            t["pattern.angle"] = {.when = patternOn};
            t["pattern.seed"] = {.noSlider = true, .when = patternOn};
            t["pattern.image"] = {.when = patternImage};
            t["pattern.shade"] = {.when = patternOn};
            t["pattern.relief"] = {.when = patternOn};
            t["texture.amount"] = {.when = textureOn};
            t["texture.scale"] = {.uiMin = 0.5, .uiMax = 200, .slider = SliderMap::Log, .when = textureOn};
            t["texture.blend"] = {.when = textureOn};
            t["texture.seed"] = {.noSlider = true, .when = textureOn};
            t["texture.target"] = {.when = textureOn};
            t["texture.angle"] = {.when = textureOn};
            t["material_inner_glow.size"] = {.uiMin = 0, .uiMax = 30};
            t["material.refraction"] = {.uiMin = -30, .uiMax = 30};
            // document
            t["canvas.width"] = {.uiMin = 64, .uiMax = 4096, .noSlider = true, .aliases = "size|resolution|サイズ"};
            t["canvas.height"] = {.uiMin = 64, .uiMax = 4096, .noSlider = true, .aliases = "size|resolution|サイズ"};
            t["canvas_background.color"] = {.when = canvasOpaque};
            t["global_light.angle"] = {.uiMin = -180, .uiMax = 180, .aliases = "light|光源"};
            t["document.seed"] = {.noSlider = true, .aliases = "random|variation"};
            t["output.scale"] = {.uiMin = 0.25, .uiMax = 4, .slider = SliderMap::Log};
            // an exposed number's range (other types have none)
            t["exposed.min"] = {.noSlider = true, .when = exposedNumber};
            t["exposed.max"] = {.noSlider = true, .when = exposedNumber};
            // generators and adjustments: their own pages
            t["generator_ref.type"] = {.hidden = true};
            t["generator_ref.params"] = {.hidden = true};
            t["adjustment.type"] = {.hidden = true};
            return t;
        }();
        return rows;
    }

    // ---- inheritance registry ----

    std::vector<InheritanceRule>& rules() {
        static std::vector<InheritanceRule> list = [] {
            std::vector<InheritanceRule> out;
            auto globalAngle = [](const ta::TitleArtDoc& doc, const ta::Json&, const std::string&) -> ta::Json {
                return doc.root.value("global_light", ta::Json::object()).value("angle", 120.0);
            };
            auto globalAltitude = [](const ta::TitleArtDoc& doc, const ta::Json&, const std::string&) -> ta::Json {
                return doc.root.value("global_light", ta::Json::object()).value("altitude", 30.0);
            };
            out.push_back({"bevel.light_angle", "use_global_light", "Global Light", globalAngle});
            out.push_back({"bevel.light_altitude", "use_global_light", "Global Light", globalAltitude});
            out.push_back({"satin.angle", "use_global_light", "Global Light", globalAngle});
            // every definition decorated withGlobalLight: its bound angle and altitude
            out.push_back({"fx:*", "use_global_light", "Global Light", nullptr});
            return out;
        }();
        return list;
    }

    // the active row gesture: the start values of its targets (one gesture at a time)
    struct ActiveRow {
        std::string rowId;
        std::vector<ta::Json> start;
        bool inherited = false;
    };
    ActiveRow& activeRow() {
        static ActiveRow row;
        return row;
    }

    std::vector<RowAudit>& audit() {
        static std::vector<RowAudit> list;
        return list;
    }

    std::string str(const ta::Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    // the object holding a field and its member key: "style.rims[1].width" -> ("style.rims[1]", "width")
    void splitLast(const std::string& path, std::string& parent, std::string& key) {
        const size_t dot = path.rfind('.');
        if (dot == std::string::npos) {
            parent.clear();
            key = path;
        } else {
            parent = path.substr(0, dot);
            key = path.substr(dot + 1);
        }
    }

    // the layers of a document by id while a collect pass reads it (it does not change meanwhile)
    struct LayerMemo {
        const ta::TitleArtDoc* doc = nullptr;
        std::unordered_map<std::string, const ta::Json*> layers;
    };
    LayerMemo* activeLayerMemo = nullptr;

    const ta::Json* objectOf(const ta::TitleArtDoc& doc, const std::string& layer) {
        if (layer.empty()) return &doc.root;
        if (activeLayerMemo && activeLayerMemo->doc == &doc) {
            auto it = activeLayerMemo->layers.find(layer);
            return it != activeLayerMemo->layers.end() ? it->second : nullptr;
        }
        return layerById(doc, layer);
    }

    const char* kindOfTarget(const std::string& layer) {
        return layer.empty() ? "document" : "layer";
    }

    // the schema lookup of a target's field (member or param, owner kind)
    ta::FieldLookup lookup(const ta::TitleArtDoc& doc, const FieldRef& target) {
        ta::FieldLookup out;
        const ta::Json* object = objectOf(doc, target.layer);
        if (!object) return out;
        const ta::SchemaKind* kind = ta::TitleArtSchema::get().kind(kindOfTarget(target.layer));
        if (!kind) return out;
        std::vector<ta::FieldSegment> segments;
        if (!ta::parseFieldPath(target.path, segments, nullptr)) return out;
        out = ta::lookupField(const_cast<ta::Json&>(*object), *kind, segments, false);
        if (!out.ok) {
            // a map entry that does not exist yet (a character without adjustments): the member
            // as it would be once `set` creates the entry at its defaults
            // (a copy of the member the path starts at and the type deciding the variant, not of
            // the whole layer: the index asks this for every missing entry)
            ta::Json copy = ta::Json::object();
            for (const char* variantKey : {"type"}) {
                if (object->contains(variantKey)) copy[variantKey] = (*object)[variantKey];
            }
            if (!segments.empty() && object->contains(segments[0].key)) copy[segments[0].key] = (*object)[segments[0].key];
            out = ta::lookupField(copy, *kind, segments, true);
            out.value = nullptr;
        }
        return out;
    }

    // the effect or generator entry a params path points into ("effects[2].params.size" -> effects[2])
    const ta::Json* paramsOwner(const ta::TitleArtDoc& doc, const FieldRef& target, const ta::EffectDef** effect, const ta::GeneratorDef** generator) {
        *effect = nullptr;
        *generator = nullptr;
        const size_t at = target.path.rfind(".params.");
        if (at == std::string::npos) return nullptr;
        const std::string entryPath = target.path.substr(0, at);
        const ta::Json* object = objectOf(doc, target.layer);
        if (!object) return nullptr;
        const ta::Json* entry = ta::getTitleArtField(*object, kindOfTarget(target.layer), entryPath);
        if (!entry || !entry->is_object()) return nullptr;
        const std::string type = str(*entry, "type");
        if (entryPath.size() >= 9 && entryPath.compare(entryPath.size() - 9, 9, "generator") == 0) *generator = ta::findGenerator(type);
        else *effect = ta::findEffect(type);
        if (!*effect && !*generator) {
            *effect = ta::findEffect(type);
            if (!*effect) *generator = ta::findGenerator(type);
        }
        return entry;
    }

    std::string titleCase(const std::string& value) {
        std::string out;
        bool upper = true;
        for (char c : value) {
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

    const char* unitText(ta::ParamUnit unit) {
        switch (unit) {
            case ta::ParamUnit::Px: return "px";
            case ta::ParamUnit::Percent: return "%";
            case ta::ParamUnit::Deg: return "\xC2\xB0";
            case ta::ParamUnit::Factor: return "x";
            default: return "";
        }
    }

    std::string kindText(ta::ValueType type, const ta::ParamDef* param, const ta::SchemaMember* member) {
        if (param) {
            switch (param->kind) {
                case ta::ParamKind::Number: return "number";
                case ta::ParamKind::Integer: return "integer";
                case ta::ParamKind::Bool: return "bool";
                case ta::ParamKind::Enum: return "enum";
                case ta::ParamKind::Color: return "color";
                case ta::ParamKind::Gradient: return "gradient";
                case ta::ParamKind::Curve: return "curve";
                case ta::ParamKind::Vec2: return "vec2";
                case ta::ParamKind::Asset: return "asset";
                case ta::ParamKind::String: return "text";
                default: return "link";
            }
        }
        switch (type) {
            case ta::ValueType::Number: return "number";
            case ta::ValueType::Integer: return "integer";
            case ta::ValueType::Bool: return "bool";
            case ta::ValueType::Enum: return "enum";
            case ta::ValueType::Color: return "color";
            case ta::ValueType::String: return "text";
            case ta::ValueType::Vec2: return "vec2";
            case ta::ValueType::AssetRef: return "asset";
            case ta::ValueType::Object:
                if (member && member->kind == "gradient") return "gradient";
                if (member && member->kind == "curve") return "curve";
                return "link";
            default: return "link";
        }
    }

    ta::Json encodeNumber(double value, bool integer) {
        if (integer) return ta::Json((int64_t)std::llround(value));
        return ta::Json(value);
    }

    std::string rowIdOf(const RowContext& ctx, const FieldRef& target) {
        std::string node;
        switch (ctx.node.kind) {
            case NodeKind::Document: node = "doc"; break;
            case NodeKind::Layer: node = "layer"; break;
            case NodeKind::Part: node = "part:" + ctx.node.part; break;
            case NodeKind::Effect: node = "fx:" + ctx.node.item; break;
            case NodeKind::Mask: node = "mask:" + ctx.node.item; break;
        }
        return ctx.node.layer + "|" + node + "|" + target.path;
    }

    ta::TitleGradient gradientOf(const ta::Json& value) {
        return ta::decodeGradient(value);
    }

    ta::Json encodeGradient(const ta::TitleGradient& gradient) {
        ta::Json stops = ta::Json::array();
        for (const ta::TitleGradientStop& s : gradient.stops) {
            stops.push_back(ta::Json{{"pos", s.pos}, {"color", ta::formatTitleColor(s.color)}, {"alpha", s.alpha}, {"mid", s.mid}});
        }
        return ta::Json{{"stops", stops}, {"interp", gradient.vivid ? "vivid" : "rgb"}};
    }

    Curve curveOf(const ta::Json& value) {
        const ta::TitleCurve curve = ta::decodeCurve(value);
        std::vector<CurveKey> keys;
        for (const ta::TitleCurvePoint& p : curve.points) {
            CurveKey key;
            key.time = (float)p.x;
            key.value = (float)p.y;
            if (p.corner) {
                key.setBothModes(CurveTangentMode::Linear);
                key.setBroken(true);
            } else {
                key.setBothModes(CurveTangentMode::ClampedAuto);
            }
            keys.push_back(key);
        }
        Curve out(std::move(keys));
        out.updateAllTangents();
        return out;
    }

    ta::Json encodeCurve(const Curve& curve) {
        std::vector<std::pair<std::pair<float, float>, bool>> points;
        for (const CurveKey& key : curve.keys) {
            const bool corner = key.broken() || key.inMode() == CurveTangentMode::Linear || key.inMode() == CurveTangentMode::Constant;
            points.push_back({{std::clamp(key.time, 0.0f, 1.0f), std::clamp(key.value, 0.0f, 1.0f)}, corner});
        }
        return curveJsonFromKeys(points);
    }

}

// ---- table and inheritance ----

const RowSpec* jasperengine::editor::titledesigner::rowSpec(const std::string& key) {
    auto it = table().find(key);
    return it == table().end() ? nullptr : &it->second;
}

std::string jasperengine::editor::titledesigner::rowSpecKey(const ta::SchemaKind* kind, const std::string& member) {
    return (kind ? kind->name : std::string()) + "." + member;
}

void jasperengine::editor::titledesigner::registerInheritance(InheritanceRule rule) {
    rules().push_back(std::move(rule));
}

const InheritanceRule* jasperengine::editor::titledesigner::inheritanceOf(const std::string& kindName, const std::string& member,
                                                                                const ta::EffectDef* effect, const ta::GeneratorDef* generator) {
    const ta::GlobalLightBinding* binding = effect ? &effect->globalLight : generator ? &generator->globalLight : nullptr;
    for (const InheritanceRule& rule : rules()) {
        if (rule.pattern == "fx:*") {
            if (!binding || !binding->angleKey) continue;
            if (member == binding->angleKey || (binding->altitudeKey && member == binding->altitudeKey)) return &rule;
            continue;
        }
        if (!binding && rule.pattern == kindName + "." + member) return &rule;
    }
    return nullptr;
}

const std::vector<RowAudit>& jasperengine::editor::titledesigner::lastCollectAudit() {
    return audit();
}

namespace {
    jasperengine::editor::titledesigner::CollectTimes collectTimes;

    double msSince(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    // adds the scope's time to a CollectTimes member while a Collect pass runs
    struct CollectTimer {
        double* into;
        std::chrono::steady_clock::time_point start;
        CollectTimer(bool collecting, double& member) : into(collecting ? &member : nullptr), start(std::chrono::steady_clock::now()) {}
        ~CollectTimer() {
            if (into) *into += msSince(start);
        }
    };
}

namespace {
    // Collect passes again after an edit: an object whose content, place and page did not change
    // gives the same index entries, so they are replayed rather than collected (R69: an edit to
    // one rim re-collects that rim, not the layer's 14 other cards)
    struct RecordedRows {
        jasperengine::editor::titledesigner::PropertyIndex::Recorded entries;
        std::vector<jasperengine::editor::titledesigner::RowAudit> audits;
    };
    std::unordered_map<std::string, RecordedRows>& recordedRows() {
        static std::unordered_map<std::string, RecordedRows> map;
        return map;
    }

    // the replay key of a group of rows: where they are (page, node, sections, prefixes), what
    // they show (the kind, the targets' path) and every target's object as it is now
    std::string rowsKey(const jasperengine::editor::titledesigner::RowContext& ctx, const std::vector<std::string>& layers,
                        const std::string& basePath, const std::string& what) {
        using namespace jasperengine::editor::titledesigner;
        std::string key = what + "\x1f" + basePath + "\x1f" + ctx.nodeLabel + "\x1f" + ctx.nodeKind + "\x1f" + ctx.stepPrefix + "\x1f" +
                          ctx.countNoun + "\x1f" + std::to_string((int)ctx.node.kind) + ctx.node.layer + "/" + ctx.node.part + "/" + ctx.node.item +
                          (ctx.readOnly ? "\x1fro" : "");
        for (const std::string& section : ctx.sectionChain) key += "\x1f" + section;
        const auto& doc = ctx.state->shown();
        for (const std::string& layer : layers) {
            const ta::Json* object = fieldValue(doc, {layer, basePath});
            key += "\x1f" + layer + "=" + std::to_string(object ? std::hash<ta::Json>{}(*object) : 0);
        }
        return key;
    }

    // replays the rows of `key` into the pass; false when they were not recorded
    bool replayRows(jasperengine::editor::titledesigner::RowContext& ctx, const std::string& key) {
        auto it = recordedRows().find(key);
        if (it == recordedRows().end()) return false;
        ctx.sink->replay(it->second.entries);
        for (const auto& a : it->second.audits) audit().push_back(a);
        return true;
    }

    void recordRows(const jasperengine::editor::titledesigner::RowContext& ctx, const std::string& key, size_t entriesBefore, size_t auditsBefore) {
        if (recordedRows().size() > 8192) recordedRows().clear();
        RecordedRows recorded;
        recorded.entries = ctx.sink->record(entriesBefore, ctx.sink->size());
        recorded.audits.assign(audit().begin() + (long)auditsBefore, audit().end());
        recordedRows()[key] = std::move(recorded);
    }
}

void jasperengine::editor::titledesigner::clearRecordedRows() {
    recordedRows().clear();
}

const jasperengine::editor::titledesigner::CollectTimes& jasperengine::editor::titledesigner::lastCollectTimes() {
    return collectTimes;
}

void jasperengine::editor::titledesigner::clearCollectAudit() {
    collectTimes = CollectTimes();
    audit().clear();
}

// ---- formatting ----

std::string jasperengine::editor::titledesigner::enumLabel(const std::string& value) {
    return titleCase(value);
}

std::vector<std::string> jasperengine::editor::titledesigner::enumValuesOf(const ta::SchemaMember* member, const ta::ParamDef* param) {
    std::vector<std::string> out;
    if (param) {
        for (const ta::EnumOption& option : param->options) out.push_back(option.value);
    } else if (member) {
        out = member->options;
    }
    return out;
}

std::vector<std::string> jasperengine::editor::titledesigner::enumLabelsOf(const ta::SchemaMember* member, const ta::ParamDef* param) {
    std::vector<std::string> out;
    if (param) {
        for (const ta::EnumOption& option : param->options) out.push_back(option.label && option.label[0] ? option.label : titleCase(option.value));
    } else if (member) {
        for (const std::string& option : member->options) {
            if (member->key == "blend") {
                ta::TitleBlend blend;
                out.push_back(ta::titleBlendFromName(option, blend) ? ta::titleBlendLabel(blend) : titleCase(option));
            } else {
                out.push_back(titleCase(option));
            }
        }
    }
    return out;
}

std::string jasperengine::editor::titledesigner::formatValue(const ta::Json& value, ta::ParamUnit unit) {
    if (!value.is_number()) return value.is_string() ? titleCase(value.get<std::string>()) : value.dump();
    ui::DocRowOptions options;
    options.unit = unitText(unit);
    options.decimals = value.is_number_integer() ? 0 : 2;
    return ui::formatDocNumber(value.get<double>(), options);
}

std::string jasperengine::editor::titledesigner::stepName(const char* verb, const std::string& label, size_t targets, const std::string& countNoun) {
    std::string name = std::string(verb) + " " + label;
    if (targets > 1) name += " on " + std::to_string(targets) + " " + (countNoun.empty() ? std::string("Layers") : countNoun);
    return name;
}

std::string jasperengine::editor::titledesigner::objectSummary(const std::string& kindName, const ta::Json& object) {
    const ta::TitleArtSchema& schema = ta::TitleArtSchema::get();
    const ta::SchemaKind* kind = schema.kind(kindName);
    if (!kind || !object.is_object()) return {};
    if (object.contains("enabled") && object["enabled"].is_boolean() && !object["enabled"].get<bool>()) return "Off";
    std::vector<std::string> parts;
    for (const ta::SchemaMember* member : schema.membersOf(*kind)) {
        if (parts.size() >= 3) break;
        if (member->readOnly || member->key == "enabled" || member->key == "type" || member->key == "link") continue;
        auto it = object.find(member->key);
        if (it == object.end()) continue;
        const RowSpec* spec = rowSpec(kind->name + "." + member->key);
        if (spec && (spec->hidden || (spec->when && !spec->when(object)))) continue;
        if (member->type == ta::ValueType::Enum && it->is_string()) {
            const std::string v = it->get<std::string>();
            if (v != "none") parts.push_back(titleCase(v));
        } else if ((member->type == ta::ValueType::Number || member->type == ta::ValueType::Integer) && it->is_number() &&
                   member->defaultValue.is_number() && std::abs(it->get<double>() - member->defaultValue.get<double>()) > 1e-9) {
            parts.push_back(formatValue(*it, member->unit));
        } else if (member->type == ta::ValueType::Color && it->is_string() && kind->name == "fill" && object.value("kind", "solid") == std::string("solid")) {
            parts.push_back(it->get<std::string>());
        }
    }
    std::string out;
    for (const std::string& p : parts) {
        if (!out.empty()) out += " \xC2\xB7 ";
        out += p;
    }
    return out;
}

ta::Json jasperengine::editor::titledesigner::curveJsonFromKeys(const std::vector<std::pair<std::pair<float, float>, bool>>& points) {
    ta::Json list = ta::Json::array();
    for (const auto& p : points) list.push_back(ta::Json{{"x", p.first.first}, {"y", p.first.second}, {"corner", p.second}});
    return ta::Json{{"points", list}};
}

// ---- sections ----

bool jasperengine::editor::titledesigner::beginSection(RowContext& ctx, const std::string& id, const std::string& title, const std::string& summary,
                                                            bool defaultOpen) {
    JE_PROFILE_SCOPE("TD beginSection");
    if (ctx.collecting()) {
        ctx.sectionChain.push_back(title);
        return true;
    }
    const std::string key = ctx.nodeKind + "/" + id;
    bool open = ctx.host ? ctx.host->sectionOpen(key, defaultOpen) : defaultOpen;
    if (!open && std::find(ctx.forceOpen.begin(), ctx.forceOpen.end(), title) != ctx.forceOpen.end()) {
        open = true;
        if (ctx.host) ctx.host->setSectionOpen(key, true);
    }
    const std::string label = (open || summary.empty() ? title : title + "  \xC2\xB7  " + summary) + "###sec_" + id;
    ImGui::SetNextItemOpen(open, ImGuiCond_Always);
    const bool nowOpen = ImGui::CollapsingHeader(label.c_str());
    if (nowOpen != open && ctx.host) ctx.host->setSectionOpen(key, nowOpen);
    if (nowOpen) {
        ctx.sectionChain.push_back(title);
        ImGui::PushID(id.c_str());
        ImGui::Indent(4.0f);
    }
    return nowOpen;
}

void jasperengine::editor::titledesigner::endSection(RowContext& ctx) {
    if (!ctx.sectionChain.empty()) ctx.sectionChain.pop_back();
    if (ctx.collecting()) return;
    ImGui::Unindent(4.0f);
    ImGui::PopID();
}

const ta::Json* jasperengine::editor::titledesigner::fieldValue(const ta::TitleArtDoc& doc, const FieldRef& target) {
    const ta::Json* object = objectOf(doc, target.layer);
    if (!object) return nullptr;
    return ta::getTitleArtField(*object, kindOfTarget(target.layer), target.path);
}

jasperengine::editor::titledesigner::CollectLayerLookup::CollectLayerLookup(const ta::TitleArtDoc& doc) {
    auto* memo = new LayerMemo();
    memo->doc = &doc;
    ta::forEachLayer(doc.root, [&](const ta::Json& l, const ta::Json*, int) {
        auto it = l.find("id");
        if (it != l.end() && it->is_string()) memo->layers.emplace(it->get<std::string>(), &l);
        return true;
    });
    previous = activeLayerMemo;
    activeLayerMemo = memo;
}

jasperengine::editor::titledesigner::CollectLayerLookup::~CollectLayerLookup() {
    delete activeLayerMemo;
    activeLayerMemo = static_cast<LayerMemo*>(previous);
}

// ---- rows ----

void jasperengine::editor::titledesigner::fieldRow(RowContext& ctx, const std::vector<FieldRef>& targets, const RowExtras& extras) {
    JE_PROFILE_SCOPE("TD fieldRow");
    const CollectTimer rowTimer(ctx.collecting(), collectTimes.rows);
    if (ctx.collecting()) collectTimes.rowCount++;
    if (targets.empty() || !ctx.state) return;
    const ta::TitleArtDoc& doc = ctx.state->shown();
    ta::FieldLookup found;
    {
        JE_PROFILE_SCOPE("TD row lookup");
        const CollectTimer timer(ctx.collecting(), collectTimes.lookup);
        found = lookup(doc, targets[0]);
    }
    if (!found.ok || (!found.member && !found.param)) return;
    const ta::SchemaMember* member = found.member;
    const ta::ParamDef* param = found.param;

    std::string parentPath, key;
    splitLast(targets[0].path, parentPath, key);
    const ta::Json* rootObject = objectOf(doc, targets[0].layer);
    static const ta::Json emptyObject = ta::Json::object();
    const ta::Json* parent = nullptr;
    {
        JE_PROFILE_SCOPE("TD row parent");
        parent = parentPath.empty() ? rootObject : ta::getTitleArtField(*rootObject, kindOfTarget(targets[0].layer), parentPath);
    }
    if (!parent) parent = &emptyObject;

    const ta::EffectDef* effect = nullptr;
    const ta::GeneratorDef* generator = nullptr;
    if (param) paramsOwner(doc, targets[0], &effect, &generator);
    const std::string ownerKind = found.owner ? found.owner->name : std::string();
    const std::string specKey = param ? std::string("fx:") + (effect ? effect->type : generator ? generator->type : "") + "." + key
                                      : ownerKind + "." + key;
    const RowSpec* spec = rowSpec(specKey);
    if (!spec && member && found.owner && !found.owner->base.empty()) spec = rowSpec(found.owner->base + "." + key);
    if (spec && spec->hidden && !extras.label) return;
    if (spec && spec->when && !spec->when(*parent)) return;
    if (param && param->when.active() && parent->is_object()) {
        auto it = parent->find(param->when.param);
        const std::string current = it != parent->end() && it->is_string() ? it->get<std::string>()
                                    : it != parent->end() && it->is_boolean() ? (it->get<bool>() ? "true" : "false")
                                                                              : std::string();
        bool shown = false;
        for (const char* v : param->when.values) shown = shown || current == v;
        if (!shown) return;
    }
    if (member && member->readOnly) return;

    std::string label = extras.label ? extras.label : spec && spec->label ? spec->label : param ? param->label : member->label;
    const std::string plainLabel = label;
    // R73: the Japanese typesetting term where English has no exact one, only when the editor
    // font can draw it (the search always has the term: its aliases)
    if (!ctx.collecting() && !extras.label) {
        static const std::map<std::string, std::pair<const char*, ImWchar>> terms = {
            {"layer.text.h_scale", {" (\xE9\x95\xB7\xE4\xBD\x93/\xE5\xB9\xB3\xE4\xBD\x93)", 0x9577}},
            {"layer.text.v_scale", {" (\xE9\x95\xB7\xE4\xBD\x93/\xE5\xB9\xB3\xE4\xBD\x93)", 0x9577}},
            {"layer.text.proportional", {" (\xE8\xA9\xB0\xE3\x82\x81)", 0x8A70}},
            {"layer.text.vertical", {" (\xE7\xB8\xA6\xE6\x9B\xB8\xE3\x81\x8D)", 0x7E26}},
            {"layer.text.tate_chu_yoko", {" (\xE7\xB8\xA6\xE4\xB8\xAD\xE6\xA8\xAA)", 0x7E26}},
            {"erode.amount", {" (\xE3\x81\x8B\xE3\x81\x99\xE3\x82\x8C)", 0x304B}}};
        auto term = terms.find(specKey);
        if (term != terms.end() && ImGui::GetCurrentContext() && ImGui::GetFont() && ImGui::GetFont()->IsGlyphInFont(term->second.second)) {
            label += term->second.first;
        }
    }
    const std::string stepLabel = extras.stepLabel ? extras.stepLabel : ctx.stepPrefix + plainLabel;
    const ta::ValueType type = member ? member->type : ta::ValueType::Json;
    const std::string kind = kindText(type, param, member);
    const bool link = extras.link || (spec && spec->link) || kind == "link";
    const std::string rowId = rowIdOf(ctx, targets[0]);

    // values over the targets
    JE_PROFILE_SCOPE("TD row values");
    std::optional<CollectTimer> valuesTimer(std::in_place, ctx.collecting(), collectTimes.values);
    std::vector<ta::Json> values;
    values.reserve(targets.size());
    for (const FieldRef& target : targets) {
        const ta::Json* v = fieldValue(doc, target);
        values.push_back(v ? *v : (param ? param->defaultValue : member->defaultValue));
    }
    bool mixed = false;
    for (size_t i = 1; i < values.size(); i++) mixed = mixed || values[i] != values[0];

    // inheritance (R23)
    const InheritanceRule* rule = inheritanceOf(ownerKind, key, effect, generator);
    bool inherited = false;
    ta::Json shownValue = values[0];
    if (rule && parent->is_object()) {
        inherited = parent->value(rule->flagMember, false);
        if (inherited) {
            if (rule->value) {
                shownValue = rule->value(doc, *parent, key);
            } else {
                const ta::GlobalLightBinding& binding = effect ? effect->globalLight : generator->globalLight;
                const ta::Json light = doc.root.value("global_light", ta::Json::object());
                if (binding.angleKey && key == binding.angleKey) shownValue = ta::angleFromGlobalLight(binding, light.value("angle", 120.0));
                else shownValue = light.value("altitude", 30.0);
            }
        }
    }

    // numeric options from the schema / definition
    ui::DocRowOptions options;
    options.label = label.c_str();
    options.fieldPath = targets[0].path;
    options.tooltip = param ? std::string(param->description) : std::string();
    options.mixed = mixed;
    options.readOnly = ctx.readOnly;
    options.readOnlyReason = ctx.readOnlyReason;
    options.source = rule ? (inherited ? ui::SourceDot::Inherited : ui::SourceDot::SetHere) : ui::SourceDot::None;
    options.canReset = !rule;
    options.flash = ctx.host && !ctx.host->flashingRow().empty() && ctx.host->flashingRow() == rowId;
    ta::Json defaultValue = param ? param->defaultValue : member->defaultValue;
    const bool numeric = kind == "number" || kind == "integer";
    if (numeric) {
        options.integer = kind == "integer";
        double lo = param ? param->min : member->min;
        double hi = param ? param->max : member->max;
        if (param && param->min == param->max) {
            lo = -std::numeric_limits<double>::infinity();
            hi = std::numeric_limits<double>::infinity();
        }
        options.hardMin = lo;
        options.hardMax = hi;
        options.uiMin = spec && spec->uiMin != spec->uiMax ? spec->uiMin : lo;
        options.uiMax = spec && spec->uiMin != spec->uiMax ? spec->uiMax : hi;
        if (param && param->step > 0) options.step = param->step;
        options.unit = unitText(param ? param->unit : member->unit);
        options.defaultValue = defaultValue.is_number() ? defaultValue.get<double>() : 0.0;
        const bool finite = std::isfinite(options.uiMin) && std::isfinite(options.uiMax) && options.uiMax > options.uiMin;
        if (finite && !(spec && spec->noSlider) && options.uiMax - options.uiMin < 1e7) {
            if (spec && spec->slider != ui::SliderMap::None) options.slider = spec->slider;
            else {
                const double floor = std::max(std::abs(options.uiMin), ui::docRowStep(options));
                options.slider = (options.uiMin >= 0.0 && options.uiMax / std::max(1e-9, floor) >= 50.0 && options.uiMax - options.uiMin >= 50.0)
                                     ? ui::SliderMap::Log
                                     : ui::SliderMap::Linear;
            }
        }
        options.isDefault = !mixed && shownValue.is_number() && std::abs(shownValue.get<double>() - options.defaultValue) < 1e-9;
    } else {
        options.isDefault = !mixed && shownValue == defaultValue;
    }
    if (inherited) options.isDefault = true;

    valuesTimer.reset();
    if (ctx.collecting()) {
        JE_PROFILE_SCOPE("TD row index");
        const CollectTimer timer(true, collectTimes.index);
        if (ctx.sink) {
            IndexEntry entry;
            entry.rowId = rowId;
            entry.label = label;
            if (spec && spec->aliases) {
                std::string word;
                for (const char* c = spec->aliases;; c++) {
                    if (*c == '|' || *c == 0) {
                        if (!word.empty()) entry.aliases.push_back(word);
                        word.clear();
                        if (*c == 0) break;
                    } else {
                        word += *c;
                    }
                }
            }
            if (effect) {
                for (const char* alias : effect->aliases) entry.aliases.push_back(alias);
            }
            entry.location = ctx.sectionChain;
            entry.node = ctx.node;
            entry.nodeLabel = ctx.nodeLabel;
            entry.targets = targets;
            entry.fieldPath = targets[0].path;
            entry.kind = kind;
            if (kind == "enum") entry.enumLabels = enumLabelsOf(member, param);
            entry.link = link;
            ctx.sink->add(std::move(entry));
        }
        if (numeric) {
            RowAudit a;
            a.rowId = rowId;
            a.label = label;
            a.fieldPath = targets[0].path;
            a.numeric = true;
            a.hasDefault = defaultValue.is_number();
            a.defaultValue = options.defaultValue;
            a.hardMin = options.hardMin;
            a.hardMax = options.hardMax;
            audit().push_back(a);
        }
        return;
    }

    if (ctx.host && ctx.host->scrollToRow() == rowId) {
        ImGui::SetScrollHereY(0.5f);
        ctx.host->rowScrolled();
    }

    if (rule && !ctx.legendShown) {
        ui::docRowSourceLegend(rule->sourceName.c_str());
        ctx.legendShown = true;
    }
    if (link) {
        if (ui::docRowLink(rowId.c_str(), options, "Edit on its page" "##link") && ctx.host) ctx.host->selectNode(ctx.node);
        return;
    }

    // ---- draw ----
    ui::DocRowResult result;
    switch (type == ta::ValueType::Json ? (param->kind == ta::ParamKind::Number ? ta::ValueType::Number
                                           : param->kind == ta::ParamKind::Integer ? ta::ValueType::Integer
                                           : param->kind == ta::ParamKind::Bool    ? ta::ValueType::Bool
                                           : param->kind == ta::ParamKind::Enum    ? ta::ValueType::Enum
                                           : param->kind == ta::ParamKind::Color   ? ta::ValueType::Color
                                           : param->kind == ta::ParamKind::Vec2    ? ta::ValueType::Vec2
                                           : param->kind == ta::ParamKind::Asset   ? ta::ValueType::AssetRef
                                           : param->kind == ta::ParamKind::String  ? ta::ValueType::String
                                                                                   : ta::ValueType::Object)
                                        : type) {
        case ta::ValueType::Number:
        case ta::ValueType::Integer:
            result = ui::docRowNumber(rowId.c_str(), shownValue.is_number() ? shownValue.get<double>() : 0.0, options);
            break;
        case ta::ValueType::Bool:
            result = ui::docRowBool(rowId.c_str(), shownValue.is_boolean() && shownValue.get<bool>(), options);
            break;
        case ta::ValueType::Enum: {
            const std::vector<std::string> valuesList = enumValuesOf(member, param);
            const std::vector<std::string> labels = enumLabelsOf(member, param);
            int index = -1;
            if (shownValue.is_string()) {
                auto it = std::find(valuesList.begin(), valuesList.end(), shownValue.get<std::string>());
                if (it != valuesList.end()) index = (int)(it - valuesList.begin());
            }
            result = ui::docRowEnum(rowId.c_str(), index, labels, options);
            if (result.kind == ui::DocRowResult::Kind::Set && result.value >= 0 && (size_t)result.value < valuesList.size()) {
                result.text = valuesList[(size_t)result.value];
            }
            break;
        }
        case ta::ValueType::String:
            result = ui::docRowText(rowId.c_str(), shownValue.is_string() ? shownValue.get<std::string>() : std::string(), options,
                                    key == "text");
            break;
        case ta::ValueType::Color: {
            ta::TitleColor color;
            ta::parseTitleColor(shownValue, color);
            result = ui::docRowCustom(rowId.c_str(), options, [&](float width) {
                ui::DocRowResult r;
                Vector3 rgb(color.r, color.g, color.b);
                ui::ColorFieldOptions colorOptions;
                colorOptions.alpha = false;
                colorOptions.linearStorage = false;
                colorOptions.label = label.c_str();
                colorOptions.mixed = mixed;
                colorOptions.width = width;
                const ui::EditPhase phase = ui::colorField("##color", rgb, colorOptions);
                if (phase != ui::EditPhase::None) {
                    r.phase = phase;
                    r.kind = ui::DocRowResult::Kind::Set;
                    ta::TitleColor out{rgb.x, rgb.y, rgb.z};
                    r.text = ta::formatTitleColor(out);
                }
                return r;
            });
            if (result.pasted) {
                ta::TitleColor pasted;
                if (ta::parseTitleColor(ta::Json(result.text), pasted)) {
                    result.phase = ui::EditPhase::Applied;
                    result.kind = ui::DocRowResult::Kind::Set;
                    result.text = ta::formatTitleColor(pasted);
                }
            }
            break;
        }
        case ta::ValueType::Vec2: {
            const ta::TitleVec2 v = ta::decodeVec2(shownValue);
            result = ui::docRowCustom(rowId.c_str(), options, [&](float width) {
                ui::DocRowResult r;
                const float half = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                double xy[2] = {v.x, v.y};
                for (int axis = 0; axis < 2; axis++) {
                    if (axis) ImGui::SameLine();
                    ImGui::PushID(axis);
                    ui::DocRowOptions sub = options;
                    sub.label = axis ? "Y" : "X";
                    sub.labelWidth = ImGui::CalcTextSize("X").x + ImGui::GetFontSize() * 1.6f;
                    sub.slider = ui::SliderMap::None;
                    sub.canReset = false;
                    sub.source = ui::SourceDot::None;
                    sub.canExpose = false;
                    ImGui::BeginGroup();
                    ImGui::PushItemWidth(half);
                    ImGui::BeginChild("##axis", ImVec2(half, ImGui::GetFrameHeight()), false, ImGuiWindowFlags_NoScrollbar);
                    ui::DocRowResult axisResult = ui::docRowNumber("##v", xy[axis], sub);
                    ImGui::EndChild();
                    ImGui::PopItemWidth();
                    ImGui::EndGroup();
                    if (axisResult.phase != ui::EditPhase::None) {
                        r = axisResult;
                        r.text = axis ? "y" : "x";
                    }
                    ImGui::PopID();
                }
                return r;
            });
            break;
        }
        case ta::ValueType::AssetRef: {
            const ta::TitleAssetRef ref = ta::decodeAssetRef(shownValue);
            result = ui::docRowCustom(rowId.c_str(), options, [&](float width) {
                ui::DocRowResult r;
                const std::string name = mixed ? std::string("\xE2\x80\x94") : ref.empty() ? std::string("None") : ref.path;
                ImGui::Button((name + "##asset").c_str(), ImVec2(width, 0.0f));
                if (ImGui::IsItemHovered() && !ref.path.empty()) ImGui::SetTooltip("%s", ref.path.c_str());
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                        // the Project window's payload: newline-separated project paths; the first one
                        const std::string text((const char*)payload->Data, (size_t)payload->DataSize);
                        std::string first = text.substr(0, text.find('\n'));
                        while (!first.empty() && first.back() == '\0') first.pop_back();
                        r.phase = ui::EditPhase::Applied;
                        r.kind = ui::DocRowResult::Kind::Set;
                        r.text = first;
                    }
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::BeginPopupContextItem("##assetmenu")) {
                    if (ImGui::MenuItem("Clear", nullptr, false, !ref.empty())) {
                        r.phase = ui::EditPhase::Applied;
                        r.kind = ui::DocRowResult::Kind::Set;
                        r.text.clear();
                    }
                    ImGui::EndPopup();
                }
                return r;
            });
            break;
        }
        default: {
            // gradients and curves
            if (kind == "gradient") {
                ta::TitleGradient gradient = gradientOf(shownValue);
                result = ui::docRowCustom(rowId.c_str(), options, [&](float width) {
                    ui::DocRowResult r;
                    ui::RampFieldOptions rampOptions;
                    rampOptions.label = label.c_str();
                    rampOptions.mixed = mixed;
                    ImGui::PushItemWidth(width);
                    const ui::EditPhase phase = ui::rampField("##ramp", gradient, rampOptions);
                    ImGui::PopItemWidth();
                    if (phase != ui::EditPhase::None) {
                        r.phase = phase;
                        r.kind = ui::DocRowResult::Kind::Set;
                        r.text = encodeGradient(gradient).dump();
                    }
                    return r;
                });
            } else if (kind == "curve") {
                Curve curve = curveOf(shownValue);
                result = ui::docRowCustom(rowId.c_str(), options, [&](float width) {
                    ui::DocRowResult r;
                    ui::CurveFieldOptions curveOptions;
                    curveOptions.range = {0.0f, 0.0f, 1.0f, 1.0f, true};
                    curveOptions.label = label.c_str();
                    curveOptions.mixed = mixed;
                    curveOptions.height = ImGui::GetFrameHeight() * 1.6f;
                    ImGui::SetNextItemWidth(width);
                    const ui::EditPhase phase = ui::curveField("##curve", curve, curveOptions);
                    if (phase != ui::EditPhase::None) {
                        r.phase = phase;
                        r.kind = ui::DocRowResult::Kind::Set;
                        r.text = encodeCurve(curve).dump();
                    }
                    return r;
                });
            }
            break;
        }
    }

    // ---- results -> staged / committed ops ----
    if (result.expose && ctx.host) ctx.host->exposeField(targets[0], label);
    std::string error;
    auto valueFor = [&](size_t i, const ui::DocRowResult& r) -> ta::Json {
        if (r.kind == ui::DocRowResult::Kind::Shift) {
            const ta::Json& start = i < activeRow().start.size() && activeRow().rowId == rowId ? activeRow().start[i] : values[i];
            const double from = start.is_number() ? start.get<double>() : 0.0;
            return encodeNumber(ui::shiftEach(from, r.delta, options), options.integer);
        }
        if (numeric) return encodeNumber(r.value, options.integer);
        if (type == ta::ValueType::Bool || (param && param->kind == ta::ParamKind::Bool)) return ta::Json(r.value != 0.0);
        if (kind == "gradient" || kind == "curve") return ta::Json::parse(r.text);
        if (kind == "vec2") {
            ta::TitleVec2 v = ta::decodeVec2(values[i]);
            const double axisValue = r.kind == ui::DocRowResult::Kind::Set ? r.value : 0.0;
            if (r.text == "x") v.x = axisValue;
            else v.y = axisValue;
            return ta::Json::array({v.x, v.y});
        }
        if (kind == "asset") return r.text.empty() ? ta::Json() : ta::Json{{"path", r.text}, {"guid", ""}};
        return ta::Json(r.text);
    };
    // the vec2 axes shift from their own start, per axis
    auto vecShift = [&](size_t i, const ui::DocRowResult& r) -> ta::Json {
        const ta::Json& start = i < activeRow().start.size() && activeRow().rowId == rowId ? activeRow().start[i] : values[i];
        ta::TitleVec2 v = ta::decodeVec2(start);
        if (r.text == "x") v.x += r.delta;
        else v.y += r.delta;
        return ta::Json::array({v.x, v.y});
    };
    auto editsOf = [&](const ui::DocRowResult& r, bool override) {
        std::vector<FieldEdit> edits;
        for (size_t i = 0; i < targets.size(); i++) {
            ta::Json v = (kind == "vec2" && r.kind == ui::DocRowResult::Kind::Shift) ? vecShift(i, r) : valueFor(i, r);
            if (inherited && r.kind == ui::DocRowResult::Kind::Shift && numeric && i < activeRow().start.size()) {
                // shift from the inherited value the row showed
                const double from = shownValue.is_number() ? shownValue.get<double>() : 0.0;
                v = encodeNumber(ui::shiftEach(from, r.delta, options), options.integer);
            }
            edits.push_back({targets[i].layer, targets[i].path, v});
            if (override && rule) {
                const std::string flagPath = parentPath.empty() ? rule->flagMember : parentPath + "." + rule->flagMember;
                edits.push_back({targets[i].layer, flagPath, false});
            }
        }
        return edits;
    };
    const char* verb = inherited ? "Override" : "Change";
    const std::string name = stepName(verb, stepLabel, targets.size(), ctx.countNoun);
    switch (result.phase) {
        case ui::EditPhase::Begin:
            activeRow() = {rowId, values, inherited};
            ctx.state->beginGesture(name, std::string());
            if (result.kind != ui::DocRowResult::Kind::None) ctx.state->stage(editsToOps(editsOf(result, inherited)), &error);
            break;
        case ui::EditPhase::Changing:
            if (!ctx.state->gestureActive()) {
                activeRow() = {rowId, values, inherited};
                ctx.state->beginGesture(name, std::string());
            }
            ctx.state->stage(editsToOps(editsOf(result, activeRow().inherited)), &error);
            break;
        case ui::EditPhase::Committed:
            if (result.kind != ui::DocRowResult::Kind::None) ctx.state->stage(editsToOps(editsOf(result, activeRow().inherited)), &error);
            ctx.state->endGesture(&error);
            activeRow() = {};
            break;
        case ui::EditPhase::Cancelled:
            ctx.state->cancelGesture();
            activeRow() = {};
            break;
        case ui::EditPhase::Applied:
            activeRow() = {};
            ctx.state->commit(editsToOps(editsOf(result, inherited)), name, &error);
            break;
        case ui::EditPhase::None: break;
    }
    if (result.reset && !rule) {
        std::vector<FieldEdit> edits;
        for (const FieldRef& target : targets) edits.push_back({target.layer, target.path, defaultValue});
        ctx.state->commit(editsToOps(edits), stepName("Reset", stepLabel, targets.size(), ctx.countNoun), &error);
    }
    if ((result.toggleSource || result.useSource) && rule) {
        std::vector<FieldEdit> edits;
        const bool toInherited = result.useSource ? true : !inherited;
        for (const FieldRef& target : targets) {
            const std::string flagPath = parentPath.empty() ? rule->flagMember : parentPath + "." + rule->flagMember;
            edits.push_back({target.layer, flagPath, toInherited});
            // set here: the row keeps the value it showed
            if (!toInherited) edits.push_back({target.layer, target.path, shownValue});
        }
        ctx.state->commit(editsToOps(edits), toInherited ? "Use Global Light" : stepName("Override", stepLabel, targets.size(), ctx.countNoun), &error);
    }
    if (!error.empty() && ctx.host) ctx.host->reportError(error);
}

void jasperengine::editor::titledesigner::objectRows(RowContext& ctx, const std::vector<std::string>& layers, const std::string& basePath,
                                                          const std::string& kindName, const std::vector<std::string>& skip) {
    JE_PROFILE_SCOPE("TD objectRows");
    const ta::TitleArtSchema& schema = ta::TitleArtSchema::get();
    const ta::SchemaKind* kind = schema.kind(kindName);
    if (!kind) return;
    // collecting a nested object: replayed when it did not change (the layer's own members, at
    // an empty base, always run: their key would hash the whole layer)
    std::string replayKey;
    size_t entriesBefore = 0, auditsBefore = 0;
    if (ctx.collecting() && ctx.sink && !basePath.empty()) {
        std::string skipped;
        for (const std::string& k : skip) skipped += k + ",";
        replayKey = rowsKey(ctx, layers, basePath, "object:" + kindName + ":" + skipped);
        if (replayRows(ctx, replayKey)) return;
        entriesBefore = ctx.sink->size();
        auditsBefore = audit().size();
    }
    for (const ta::SchemaMember* member : schema.membersOf(*kind)) {
        if (member->readOnly || member->type == ta::ValueType::Id) continue;
        if (std::find(skip.begin(), skip.end(), member->key) != skip.end()) continue;
        const bool rowObject = member->type == ta::ValueType::Object && (member->kind == "gradient" || member->kind == "curve");
        if (!rowObject && (member->type == ta::ValueType::Object || member->type == ta::ValueType::ObjectArray || member->type == ta::ValueType::ObjectMap ||
                           member->type == ta::ValueType::ObjectArrayMap || member->type == ta::ValueType::EffectParams ||
                           member->type == ta::ValueType::GeneratorParams || member->type == ta::ValueType::Json ||
                           member->type == ta::ValueType::NumberMap || member->type == ta::ValueType::IntegerMap ||
                           member->type == ta::ValueType::AssetRefList || member->type == ta::ValueType::StringList ||
                           member->type == ta::ValueType::Vec2List || member->type == ta::ValueType::LayerRef)) {
            // Vec2Lists (distort corners) are handles; a link row says so in search
            if (member->type != ta::ValueType::Vec2List) continue;
        }
        std::vector<FieldRef> targets;
        for (const std::string& layer : layers) targets.push_back({layer, basePath.empty() ? member->key : basePath + "." + member->key});
        fieldRow(ctx, targets);
    }
    if (!replayKey.empty()) recordRows(ctx, replayKey, entriesBefore, auditsBefore);
}

void jasperengine::editor::titledesigner::paramRows(RowContext& ctx, const std::vector<std::string>& layers, const std::string& basePath,
                                                         const std::vector<ta::ParamDef>& params, const ta::EffectDef* effect,
                                                         const ta::GeneratorDef* generator) {
    JE_PROFILE_SCOPE("TD paramRows");
    std::string replayKey;
    size_t entriesBefore = 0, auditsBefore = 0;
    if (ctx.collecting() && ctx.sink && !basePath.empty()) {
        replayKey = rowsKey(ctx, layers, basePath, std::string("params:") + (effect ? effect->type : "") + ":" + (generator ? generator->type : ""));
        if (replayRows(ctx, replayKey)) return;
        entriesBefore = ctx.sink->size();
        auditsBefore = audit().size();
    }
    for (const ta::ParamDef& param : params) {
        if (std::string(param.key) == "use_global_light") continue;      // the source dot of its angle row
        std::vector<FieldRef> targets;
        for (const std::string& layer : layers) targets.push_back({layer, basePath + "." + param.key});
        if (param.kind == ta::ParamKind::Material || param.kind == ta::ParamKind::Adjustment || param.kind == ta::ParamKind::RowArray ||
            param.kind == ta::ParamKind::Layer) {
            // drawn as their own sub-rows
            if (param.kind == ta::ParamKind::Material) {
                if (beginSection(ctx, std::string("p_") + param.key, param.label, std::string(), false)) {
                    objectRows(ctx, layers, basePath + "." + param.key, "material");
                    endSection(ctx);
                }
            } else if (param.kind == ta::ParamKind::Adjustment) {
                const ta::Json* value = fieldValue(ctx.state->shown(), targets[0]);
                const std::string type = value ? str(*value, "type") : std::string();
                const ta::SchemaKind* base = ta::TitleArtSchema::get().kind("adjustment");
                const ta::SchemaKind* variant = base && value ? ta::TitleArtSchema::get().variantOf(*base, *value) : nullptr;
                fieldRow(ctx, {{layers[0], basePath + "." + param.key + ".type"}}, {.label = "Adjustment"});
                if (variant) objectRows(ctx, layers, basePath + "." + param.key, variant->name);
            } else if (param.kind == ta::ParamKind::RowArray) {
                const ta::Json* rowsValue = fieldValue(ctx.state->shown(), targets[0]);
                const size_t count = rowsValue && rowsValue->is_array() ? rowsValue->size() : 0;
                for (size_t i = 0; i < count; i++) {
                    if (beginSection(ctx, std::string("r_") + param.key + std::to_string(i), std::string(param.label) + " " + std::to_string(i + 1),
                                     std::string(), true)) {
                        std::vector<std::string> one;
                        for (const ta::ParamDef& sub : param.rows) {
                            std::vector<FieldRef> subTargets;
                            for (const std::string& layer : layers) {
                                subTargets.push_back({layer, basePath + "." + param.key + "[" + std::to_string(i) + "]." + sub.key});
                            }
                            fieldRow(ctx, subTargets);
                        }
                        endSection(ctx);
                    }
                }
            } else {
                // a layer reference: a combo of the document's layers
                fieldRow(ctx, targets);
            }
            continue;
        }
        fieldRow(ctx, targets);
    }
    if (!replayKey.empty()) recordRows(ctx, replayKey, entriesBefore, auditsBefore);
}
