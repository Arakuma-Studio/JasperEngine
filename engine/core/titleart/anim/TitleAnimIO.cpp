// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleAnimIO.h"

#include "titleanim/TitleAnimDataIO.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"
#include "util/Fnv.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;
namespace ta = jasperengine::titleanim;

namespace {

    SchemaMember s85(const std::string& key, ValueType type, Json def, const char* label){
        SchemaMember m;
        m.key = key;
        m.type = type;
        m.defaultValue = std::move(def);
        m.label = label;
        m.owner = "S85";
        m.omitDefault = true;
        return m;
    }

    // the member's normalize: the lenient codec, unique ids and names
    void normalizeAnimationMember(Json& value, const std::string& path, NormalizeContext& context){
        if (value.is_null()) return;
        std::vector<ta::TitleAnimWarning> warnings;
        ta::TitleAnimationSet set;
        ta::decodeAnimationSet(value, path, set, &warnings);
        for (const ta::TitleAnimWarning& w : warnings) context.warn(w.path, w.message);
        TitleIdGenerator local;
        TitleIdGenerator& ids = context.ids ? *context.ids : local;
        std::set<std::string> taken;
        for (size_t i = 0; i < set.animations.size(); i++){
            ta::TitleAnimation& a = set.animations[i];
            if (!isTitleArtId(a.id) || !taken.insert(a.id).second){
                const std::string id = ids.next();
                if (!a.id.empty())
                    context.warn(jsonPathMember(jsonPathIndex(jsonPathMember(path, "animations"), i), "id"), "\"" + a.id + "\" is not a unique id: " + id);
                a.id = id;
                taken.insert(id);
            }else{
                ids.reserve(a.id);
            }
            // a `next` that names nothing is kept (it may be renamed back) but plays nothing
        }
        value = ta::encodeAnimationSet(set);
    }

    // the document's finish: tracks in layer order, then by field (§6.1 Order)
    void finishDocumentAnimation(Json& document, const std::string&, NormalizeContext&){
        auto it = document.find("animation");
        if (it == document.end()) return;
        if (it->is_null()){
            document.erase(it);
            return;
        }
        if (!it->is_object()) return;
        std::map<std::string, int> order;
        int n = 0;
        forEachLayer(document, [&](const Json& layer, const Json*, int){
            order[layer.value("id", std::string())] = n++;
            return true;
        });
        auto rank = [&](const Json& track) -> int{
            const std::string layer = track.value("layer", std::string());
            if (layer.empty()) return -1;
            auto o = order.find(layer);
            return o != order.end() ? o->second : n;
        };
        auto list = it->find("animations");
        if (list == it->end() || !list->is_array()) return;
        for (Json& animation : *list){
            auto tracks = animation.find("tracks");
            if (tracks == animation.end() || !tracks->is_array()) continue;
            // fields are stored long form (ops and hand edits may use the shorthands)
            for (Json& track : *tracks){
                const std::string layerId = track.value("layer", std::string());
                std::string field = track.value("field", std::string());
                if (layerId.empty() || field.rfind("glyph.", 0) == 0) continue;
                const Json* layer = findLayer(document, layerId);
                if (!layer) continue;
                std::string prefix;
                if (field.rfind("child.", 0) == 0){
                    const Json& children = layerChildren(*layer);
                    if (children.empty()) continue;
                    layer = &children[0];
                    prefix = "child.";
                    field = field.substr(6);
                }
                const ExpandedFieldPath expanded = expandFieldShorthand(layer, field);
                if (expanded.ok && expanded.path != field) track["field"] = prefix + expanded.path;
            }
            std::vector<Json> sorted(tracks->begin(), tracks->end());
            std::stable_sort(sorted.begin(), sorted.end(), [&](const Json& a, const Json& b){
                const int ra = rank(a), rb = rank(b);
                if (ra != rb) return ra < rb;
                if (ra == n){
                    // layers that are gone keep a stable order by id
                    const std::string la = a.value("layer", std::string()), lb = b.value("layer", std::string());
                    if (la != lb) return la < lb;
                }
                return a.value("field", std::string()) < b.value("field", std::string());
            });
            *tracks = Json(sorted);
        }
    }

    const char* const TITLE_FIELDS_LIST[] = {"transform.position", "transform.rotation", "transform.scale", "transform.stretch",
                                             "transform.pivot", "opacity", "flash.color", "flash.amount", "live.sweep_position",
                                             "live.sweep_angle", "live.sweep_width", "live.glow_pulse", "live.tint_color",
                                             "live.tint_amount", "live.glint_amount"};
    const char* const GLYPH_FIELDS_LIST[] = {"glyph.offset", "glyph.rotation", "glyph.scale", "glyph.stretch", "glyph.opacity",
                                             "glyph.tint", "glyph.flash.color", "glyph.flash.amount"};

    ta::TrackValue valueOf(ValueType t){
        switch (t){
            case ValueType::Number: return ta::TrackValue::Number;
            case ValueType::Vec2: return ta::TrackValue::Vec2;
            case ValueType::Color: return ta::TrackValue::Color;
            default: return ta::TrackValue::Stepped;
        }
    }

    ta::TrackValue valueOfParam(ParamKind k){
        switch (k){
            case ParamKind::Number: return ta::TrackValue::Number;
            case ParamKind::Vec2: return ta::TrackValue::Vec2;
            case ParamKind::Color: return ta::TrackValue::Color;
            default: return ta::TrackValue::Stepped;
        }
    }

    int widthOf(ta::TrackValue v){
        return v == ta::TrackValue::Vec2 ? 2 : (v == ta::TrackValue::Color ? 3 : 1);
    }

    // "style.rims[1].width" -> "Rim 2 Width", "effects[0].params.intensity" -> "Outer Glow Intensity"
    std::string labelFor(const Json& layer, const std::string& longPath, const FieldLookup& look){
        std::string label = look.param ? std::string(look.param->label) : (look.member ? look.member->label : longPath);
        std::vector<FieldSegment> segs;
        if (!parseFieldPath(longPath, segs, nullptr)) return label;
        std::string prefix;
        for (size_t i = 0; i + 1 < segs.size(); i++){
            if (segs[i].key == "rims" && i + 1 < segs.size() && segs[i + 1].isIndex) prefix = "Rim " + std::to_string(segs[i + 1].index + 1) + " ";
            if (segs[i].key == "effects" && i + 1 < segs.size() && segs[i + 1].isIndex){
                const Json& effects = layer.value("effects", Json::array());
                if (segs[i + 1].index < effects.size())
                    if (const EffectDef* def = findEffect(effects[segs[i + 1].index].value("type", std::string()))) prefix = std::string(def->label) + " ";
            }
            if (segs[i].key == "generator")
                if (const GeneratorDef* def = findGenerator(layer.value("generator", Json::object()).value("type", std::string())))
                    prefix = std::string(def->label) + " ";
            if (segs[i].key == "face") prefix = "Face ";
        }
        return prefix + label;
    }

    void fillFromMember(TrackFieldInfo& out, const FieldLookup& look){
        if (look.param){
            const ParamDef& p = *look.param;
            out.value = valueOfParam(p.kind);
            out.anim = p.anim;
            out.combine = p.combine;
            out.stepped = out.value == ta::TrackValue::Stepped || p.interp == ParamInterp::Stepped;
            out.integer = p.kind == ParamKind::Integer;
            out.seed = std::string(p.key) == "seed";
            if (p.min != p.max){
                out.min = p.min;
                out.max = p.max;
            }
        }else if (look.member){
            const SchemaMember& m = *look.member;
            out.value = valueOf(m.type);
            out.anim = m.anim;
            out.combine = m.combine;
            out.stepped = out.value == ta::TrackValue::Stepped || m.interp == ParamInterp::Stepped;
            out.integer = m.type == ValueType::Integer;
            out.seed = m.key == "seed";
            out.min = m.min;
            out.max = m.max;
        }
        if (out.stepped) out.value = ta::TrackValue::Stepped;
        out.width = widthOf(out.value);
    }

}

void jasperengine::titleart::registerTitleAnimMembers(TitleArtSchema& schema){
    {
        SchemaMember m = s85("animation", ValueType::Json, nullptr, "Animation");
        m.normalize = &normalizeAnimationMember;
        m.anim = FxAnim::None;
        m.interp = ParamInterp::Stepped;
        m.hash = HashScope::Identity;
        schema.addMember("document", std::move(m));
    }
    {
        SchemaMember m = s85("stretch", ValueType::Vec2, Json::array({1, 1}), "Stretch");
        m.min = 0.01;
        m.max = 100;
        m.anim = FxAnim::Live;
        m.combine = ParamCombine::Multiply;
        m.hash = HashScope::Geometry;
        schema.addMember("transform", std::move(m));
    }
    {
        SchemaMember m = s85("tint", ValueType::Color, "#ffffff", "Tint");
        m.anim = FxAnim::Live;
        m.combine = ParamCombine::Multiply;
        m.hash = HashScope::Node;
        schema.addMember("layer", std::move(m));
    }
    {
        SchemaKind flash;
        flash.name = "layer_flash";
        flash.label = "Flash";
        flash.hash = HashScope::Node;
        SchemaMember color;
        color.key = "color";
        color.type = ValueType::Color;
        color.defaultValue = "#ffffff";
        color.label = "Flash Color";
        color.anim = FxAnim::Live;
        color.hash = HashScope::Node;
        color.owner = "S85";
        SchemaMember amount;
        amount.key = "amount";
        amount.type = ValueType::Number;
        amount.defaultValue = 0;
        amount.min = -4;
        amount.max = 4;
        amount.label = "Flash Amount";
        amount.anim = FxAnim::Live;
        amount.hash = HashScope::Node;
        amount.owner = "S85";
        flash.members = {color, amount};
        schema.addKind(std::move(flash));
        SchemaMember m = s85("flash", ValueType::Object, nullptr, "Flash");
        m.kind = "layer_flash";
        m.anim = FxAnim::None;
        m.interp = ParamInterp::Stepped;
        m.hash = HashScope::Node;
        schema.addMember("layer", std::move(m));
    }
    {
        SchemaMember m = s85("glyph_mode", ValueType::Enum, "cells", "Glyph Mode");
        m.options = {"cells", "merged"};
        m.anim = FxAnim::None;
        m.interp = ParamInterp::Stepped;
        m.hash = HashScope::Identity;
        schema.addMember("layer.text", std::move(m));
    }
    schema.setFinish("document", &finishDocumentAnimation);
}

ta::TitleAnimationSet jasperengine::titleart::readAnimationSet(const TitleArtDoc& doc, std::vector<TitleWarning>* warnings){
    ta::TitleAnimationSet set;
    auto it = doc.root.find("animation");
    if (it == doc.root.end() || it->is_null()) return set;
    std::vector<ta::TitleAnimWarning> w;
    ta::decodeAnimationSet(*it, "animation", set, &w);
    if (warnings) for (const ta::TitleAnimWarning& x : w) warnings->push_back({x.path, x.message});
    for (ta::TitleAnimation& a : set.animations){
        for (ta::TitleTrack& t : a.tracks){
            const TrackFieldInfo info = resolveTrackField(doc, t.layer, t.field);
            t.orphan = !info.ok && (info.code == "layer_missing" || info.code == "bad_path");
        }
    }
    return set;
}

void jasperengine::titleart::writeAnimationSet(TitleArtDoc& doc, const ta::TitleAnimationSet& set){
    if (set.animations.empty() && set.defaultName.empty() && set.fps == ta::TITLE_ANIM_DEFAULT_FPS && set.unknown.empty()){
        doc.root.erase("animation");
        return;
    }
    Json value = ta::encodeAnimationSet(set);
    NormalizeContext context;
    TitleIdGenerator ids;
    ids.reserveFrom(doc.root);
    context.ids = &ids;
    normalizeAnimationMember(value, "animation", context);
    doc.root["animation"] = std::move(value);
    finishDocumentAnimation(doc.root, "", context);
}

bool jasperengine::titleart::hasAnimations(const TitleArtDoc& doc){
    auto it = doc.root.find("animation");
    if (it == doc.root.end() || !it->is_object()) return false;
    auto list = it->find("animations");
    return list != it->end() && list->is_array() && !list->empty();
}

TrackFieldInfo jasperengine::titleart::resolveTrackField(const TitleArtDoc& doc, const std::string& layerId, const std::string& field){
    TrackFieldInfo out;
    auto fail = [&](const char* code, const std::string& error){
        out.ok = false;
        out.code = code;
        out.error = error;
        return out;
    };
    std::string base = field;
    int component = -1;
    if (layerId.empty()){
        ta::TitleField tf = ta::titleFieldFromPath(field);
        if (tf == ta::TitleField::None && ta::splitComponent(field, base, component)) tf = ta::titleFieldFromPath(base);
        if (tf != ta::TitleField::None){
            const ta::VirtualFieldInfo info = ta::titleFieldInfo(tf);
            if (component >= 0 && !((info.value == ta::TrackValue::Vec2 && component < 2) || (info.value == ta::TrackValue::Color && component < 3)))
                return fail("bad_path", "'" + field + "' names no component of " + base);
            out.ok = true;
            out.longPath = field;
            out.target = ta::TrackTarget::Title;
            out.titleField = tf;
            out.component = component;
            out.value = component >= 0 ? ta::TrackValue::Number : info.value;
            out.width = component >= 0 ? 1 : info.width;
            out.anim = FxAnim::Live;
            out.combine = info.relativeCombine == ta::TrackCombine::Multiply ? ParamCombine::Multiply : ParamCombine::Replace;
            out.min = info.min;
            out.max = info.max;
            out.label = std::string("Title ") + info.label;
            Json rest = Json::array();
            for (int i = 0; i < info.width; i++) rest.push_back(info.rest[i]);
            out.rest = info.width == 1 ? rest[0] : rest;
            return out;
        }
        // a document member (the document `seed`, global_light.*)
        if (field.rfind("glyph.", 0) == 0 || field.rfind("child.", 0) == 0)
            return fail("not_animatable", "'" + field + "' needs a layer: set `layer` to a text layer (glyph.) or a group (child.)");
        std::vector<FieldSegment> segs;
        std::string error;
        if (!parseFieldPath(field, segs, &error)) return fail("bad_path", error);
        Json root = doc.root;
        const SchemaKind* document = TitleArtSchema::get().kind("document");
        FieldLookup look = lookupField(root, *document, segs, true);
        if (!look.ok || !look.member || look.member->anim == FxAnim::None){
            out.validHere = animatableFields(doc, "");
            out.nearest = nearestName(field, out.validHere);
            return fail(look.ok ? "not_animatable" : "bad_path", "'" + field + "' is not an animatable title or document field");
        }
        out.ok = true;
        out.longPath = look.longPath;
        out.target = ta::TrackTarget::Field;
        fillFromMember(out, look);
        out.label = look.member->label;
        out.rest = *look.value;
        return out;
    }

    const Json* layer = findLayer(doc.root, layerId);
    if (!layer) return fail("layer_missing", "no layer with id '" + layerId + "'");
    out.layer = layer;
    const LayerType type = layerTypeOf(*layer);
    if (field.rfind("glyph.", 0) == 0){
        if (type != LayerType::Text) return fail("not_animatable", "glyph fields animate text layers; '" + layer->value("name", layerId) + "' is not one");
        ta::GlyphField gf = ta::glyphFieldFromPath(field);
        if (gf == ta::GlyphField::None && ta::splitComponent(field, base, component)) gf = ta::glyphFieldFromPath(base);
        if (gf == ta::GlyphField::None){
            for (const char* f : GLYPH_FIELDS_LIST) out.validHere.push_back(f);
            out.nearest = nearestName(field, out.validHere);
            return fail("bad_path", "'" + field + "' is not a glyph field");
        }
        const ta::VirtualFieldInfo info = ta::glyphFieldInfo(gf);
        out.ok = true;
        out.longPath = field;
        out.target = ta::TrackTarget::Glyph;
        out.glyphField = gf;
        out.component = component;
        out.value = component >= 0 ? ta::TrackValue::Number : info.value;
        out.width = component >= 0 ? 1 : info.width;
        out.anim = FxAnim::Live;
        out.combine = info.relativeCombine == ta::TrackCombine::Multiply ? ParamCombine::Multiply : ParamCombine::Replace;
        out.min = info.min;
        out.max = info.max;
        out.label = info.label;
        Json rest = Json::array();
        for (int i = 0; i < info.width; i++) rest.push_back(info.rest[i]);
        out.rest = info.width == 1 ? rest[0] : rest;
        return out;
    }
    const Json* target = layer;
    std::string path = field;
    if (field.rfind("child.", 0) == 0){
        if (type != LayerType::Group) return fail("not_animatable", "child fields animate the children of a group; '" + layer->value("name", layerId) + "' is not one");
        const Json& children = layerChildren(*layer);
        if (children.empty()) return fail("bad_path", "the group has no children");
        target = &children[0];
        path = field.substr(6);
        out.target = ta::TrackTarget::Child;
    }
    // a component track ("tint.r", "transform.position.x") resolves its whole value
    std::string resolved = path;
    {
        std::string whole;
        int c = -1;
        const ExpandedFieldPath direct = expandFieldShorthand(target, path);
        std::vector<FieldSegment> probe;
        bool directOk = direct.ok && parseFieldPath(direct.path, probe, nullptr);
        if (directOk){
            Json copy = *target;
            directOk = lookupField(copy, *TitleArtSchema::get().kind("layer"), probe, true).ok;
        }
        if (!directOk && ta::splitComponent(path, whole, c)){
            resolved = whole;
            component = c;
        }
    }
    const ExpandedFieldPath expanded = expandFieldShorthand(target, resolved);
    if (!expanded.ok){
        out.validHere = animatableFields(doc, layerId);
        out.nearest = expanded.nearest.empty() ? nearestName(path, out.validHere) : expanded.nearest;
        return fail(expanded.code.empty() ? "bad_path" : expanded.code.c_str(), expanded.error);
    }
    std::vector<FieldSegment> segs;
    std::string error;
    if (!parseFieldPath(expanded.path, segs, &error)) return fail("bad_path", error);
    Json copy = *target;
    const SchemaKind* kind = TitleArtSchema::get().kind("layer");
    FieldLookup look = lookupField(copy, *kind, segs, true);
    if (!look.ok){
        out.validHere = animatableFields(doc, layerId);
        out.nearest = look.nearest.empty() ? nearestName(path, out.validHere) : look.nearest;
        return fail("bad_path", look.error.empty() ? "'" + field + "' does not resolve" : look.error);
    }
    if ((look.member && look.member->anim == FxAnim::None) || (look.param && look.param->anim == FxAnim::None) || (!look.member && !look.param) ||
            (look.member && (look.member->type == ValueType::Object || look.member->type == ValueType::ObjectArray ||
                             look.member->type == ValueType::ObjectMap || look.member->type == ValueType::Json))){
        out.validHere = animatableFields(doc, layerId);
        out.nearest = nearestName(path, out.validHere);
        return fail("not_animatable", "'" + field + "' is not animatable (identity or structure)");
    }
    if (component >= 0){
        const bool vec2 = look.member ? look.member->type == ValueType::Vec2 : (look.param && look.param->kind == ParamKind::Vec2);
        const bool color = look.member ? look.member->type == ValueType::Color : (look.param && look.param->kind == ParamKind::Color);
        if (!(vec2 && component < 2) && !(color && component < 3)) return fail("bad_path", "'" + field + "' names no component of " + resolved);
    }
    out.ok = true;
    const std::string longPath = component >= 0 ? look.longPath + "." + path.substr(path.size() - 1) : look.longPath;
    out.longPath = out.target == ta::TrackTarget::Child ? "child." + longPath : longPath;
    if (out.target != ta::TrackTarget::Child) out.target = ta::TrackTarget::Field;
    fillFromMember(out, look);
    if (component >= 0){
        out.component = component;
        out.value = ta::TrackValue::Number;
        out.width = 1;
    }
    out.label = labelFor(*target, look.longPath, look);
    out.rest = look.value ? *look.value : Json();
    return out;
}

std::vector<std::string> jasperengine::titleart::animatableFields(const TitleArtDoc& doc, const std::string& layerId){
    std::vector<std::string> out;
    if (layerId.empty()){
        for (const char* f : TITLE_FIELDS_LIST) out.push_back(f);
        out.push_back("seed");
        out.push_back("global_light.angle");
        out.push_back("global_light.altitude");
        return out;
    }
    const Json* layer = findLayer(doc.root, layerId);
    if (!layer) return out;
    if (layerTypeOf(*layer) == LayerType::Text) for (const char* f : GLYPH_FIELDS_LIST) out.push_back(f);
    Json copy = *layer;
    const SchemaKind* kind = TitleArtSchema::get().kind("layer");
    forEachMember(copy, *kind, [&](const SchemaMember* m, const ParamDef* p, Json& value, const std::string& path){
        (void)value;
        if (m && (m->type == ValueType::Object || m->type == ValueType::ObjectArray || m->type == ValueType::ObjectMap ||
                  m->type == ValueType::ObjectArrayMap || m->type == ValueType::EffectParams || m->type == ValueType::GeneratorParams)){
            return path != "children";
        }
        const FxAnim anim = p ? p->anim : (m ? m->anim : FxAnim::None);
        if (anim != FxAnim::None && !path.empty()) out.push_back(path);
        return true;
    });
    if (layerTypeOf(*layer) == LayerType::Group){
        const Json& children = layerChildren(*layer);
        if (!children.empty()){
            out.push_back("child.transform.position");
            out.push_back("child.transform.rotation");
            out.push_back("child.transform.scale");
            out.push_back("child.opacity");
        }
    }
    return out;
}

uint64_t jasperengine::titleart::animationHash(const TitleArtDoc& doc, const ta::TitleAnimation& animation){
    (void)doc;
    const std::string text = writeTitleArtJson(ta::encodeAnimation(animation));
    return fnv1a64(text.data(), text.size());
}
