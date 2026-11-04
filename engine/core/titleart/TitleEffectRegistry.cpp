// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// angleFromGlobalLight is ported from Pachislop src/effects/globalLight.ts (angleFromGlobal) by
// SafEight, used with the author's permission.

#include "TitleEffectRegistry.h"
#include "TitleArtIO.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;

const char* titleart::paramKindName(ParamKind kind) {
    switch (kind) {
        case ParamKind::Number: return "number";
        case ParamKind::Integer: return "integer";
        case ParamKind::Bool: return "bool";
        case ParamKind::Enum: return "enum";
        case ParamKind::Color: return "color";
        case ParamKind::Gradient: return "gradient";
        case ParamKind::Curve: return "curve";
        case ParamKind::Vec2: return "vec2";
        case ParamKind::Asset: return "asset";
        case ParamKind::Layer: return "layer";
        case ParamKind::Adjustment: return "adjustment";
        case ParamKind::String: return "string";
        case ParamKind::Material: return "material";
        case ParamKind::RowArray: return "rows";
    }
    return "number";
}

const char* titleart::costClassName(CostClass cost) {
    switch (cost) {
        case CostClass::C0: return "C0";
        case CostClass::C1: return "C1";
        case CostClass::C2: return "C2";
        case CostClass::C3: return "C3";
        case CostClass::C4: return "C4";
    }
    return "C1";
}

const char* titleart::fallbackClassName(FallbackClass fallback) {
    switch (fallback) {
        case FallbackClass::F0: return "F0";
        case FallbackClass::F1: return "F1";
        case FallbackClass::F2: return "F2";
    }
    return "F0";
}

const char* titleart::liveKindName(LiveKind kind) {
    switch (kind) {
        case LiveKind::None: return "none";
        case LiveKind::ScaleSplit: return "scale_split";
        case LiveKind::Kernel: return "kernel";
    }
    return "none";
}

const char* titleart::followModeName(FollowMode mode) {
    switch (mode) {
        case FollowMode::None: return "none";
        case FollowMode::Optional: return "optional";
        case FollowMode::Required: return "required";
    }
    return "none";
}

// ---- ParamValues ----

ParamValues::ParamValues(const Json& values, const GlobalLightBinding* binding, double angle, double altitude)
    : params(&values), light(binding), lightAngle(angle), lightAltitude(altitude) {
}

const Json& ParamValues::json() const {
    static const Json empty = Json::object();
    return params && params->is_object() ? *params : empty;
}

bool ParamValues::has(const char* key) const {
    return json().contains(key);
}

const Json& ParamValues::raw(const char* key) const {
    static const Json none;
    const Json& values = json();
    auto it = values.find(key);
    return it != values.end() ? *it : none;
}

double ParamValues::number(const char* key, double fallback) const {
    if (light && light->angleKey && flag("use_global_light", false)) {
        if (std::string_view(key) == light->angleKey) return angleFromGlobalLight(*light, lightAngle);
        if (light->altitudeKey && std::string_view(key) == light->altitudeKey) return lightAltitude;
    }
    const Json& v = raw(key);
    return v.is_number() ? v.get<double>() : fallback;
}

int ParamValues::integer(const char* key, int fallback) const {
    const Json& v = raw(key);
    return v.is_number() ? (int)std::lround(v.get<double>()) : fallback;
}

bool ParamValues::flag(const char* key, bool fallback) const {
    const Json& v = raw(key);
    return v.is_boolean() ? v.get<bool>() : fallback;
}

std::string ParamValues::string(const char* key) const {
    const Json& v = raw(key);
    return v.is_string() ? v.get<std::string>() : std::string();
}

TitleColor ParamValues::color(const char* key) const {
    TitleColor c;
    parseTitleColor(raw(key), c);
    return c;
}

TitleVec2 ParamValues::vec2(const char* key) const {
    return decodeVec2(raw(key));
}

TitleGradient ParamValues::gradient(const char* key) const {
    return decodeGradient(raw(key));
}

TitleCurve ParamValues::curve(const char* key) const {
    return decodeCurve(raw(key));
}

TitleAssetRef ParamValues::asset(const char* key) const {
    return decodeAssetRef(raw(key));
}

double titleart::angleFromGlobalLight(const GlobalLightBinding& binding, double g) {
    const double ccw = binding.relation == LightRelation::From ? g : binding.relation == LightRelation::Away ? g + 180.0 : -(g + 180.0);
    const double value = binding.convention == AngleConvention::ClockwiseFromRight ? -ccw : ccw;
    double r = std::fmod(std::fmod(value, 360.0) + 360.0, 360.0);
    return r > 180.0 ? r - 360.0 : r;
}

// ---- the registry ----

TitleEffectRegistry& TitleEffectRegistry::get() {
    static TitleEffectRegistry* registry = [] {
        auto* r = new TitleEffectRegistry();
        registerBuiltinTitleEffects(*r);
        registerBuiltinTitleGenerators(*r);
        return r;
    }();
    return *registry;
}

bool TitleEffectRegistry::add(EffectDef def) {
    if (!def.type || !*def.type || effect(def.type)) return false;
    effectDefs.push_back(std::make_unique<EffectDef>(std::move(def)));
    changes++;
    return true;
}

bool TitleEffectRegistry::add(GeneratorDef def) {
    if (!def.type || !*def.type || generator(def.type)) return false;
    generatorDefs.push_back(std::make_unique<GeneratorDef>(std::move(def)));
    changes++;
    return true;
}

bool TitleEffectRegistry::removeEffect(std::string_view type) {
    auto it = std::find_if(effectDefs.begin(), effectDefs.end(), [&](const auto& d) { return type == d->type; });
    if (it == effectDefs.end()) return false;
    effectDefs.erase(it);
    changes++;
    return true;
}

bool TitleEffectRegistry::removeGenerator(std::string_view type) {
    auto it = std::find_if(generatorDefs.begin(), generatorDefs.end(), [&](const auto& d) { return type == d->type; });
    if (it == generatorDefs.end()) return false;
    generatorDefs.erase(it);
    changes++;
    return true;
}

const EffectDef* TitleEffectRegistry::effect(std::string_view type) const {
    for (const auto& d : effectDefs) {
        if (type == d->type) return d.get();
    }
    return nullptr;
}

const GeneratorDef* TitleEffectRegistry::generator(std::string_view type) const {
    for (const auto& d : generatorDefs) {
        if (type == d->type) return d.get();
    }
    return nullptr;
}

std::vector<const EffectDef*> TitleEffectRegistry::effects() const {
    std::vector<const EffectDef*> out;
    for (const auto& d : effectDefs) out.push_back(d.get());
    return out;
}

std::vector<const GeneratorDef*> TitleEffectRegistry::generators() const {
    std::vector<const GeneratorDef*> out;
    for (const auto& d : generatorDefs) out.push_back(d.get());
    return out;
}

const EffectDef* titleart::findEffect(std::string_view type) {
    return TitleEffectRegistry::get().effect(type);
}

const GeneratorDef* titleart::findGenerator(std::string_view type) {
    return TitleEffectRegistry::get().generator(type);
}

namespace {

    template<class Def>
    Def bindGlobalLight(Def def, const char* angleKey, LightRelation relation, const char* altitudeKey) {
        def.globalLight.angleKey = angleKey;
        def.globalLight.altitudeKey = altitudeKey;
        def.globalLight.relation = relation;
        def.globalLight.convention = relation == LightRelation::From ? AngleConvention::CounterClockwiseFromRight
                                                                     : AngleConvention::ClockwiseFromRight;
        if (findParam(def.params, "use_global_light")) return def;
        ParamDef use;
        use.key = "use_global_light";
        use.label = "Use Global Light";
        use.kind = ParamKind::Bool;
        use.defaultValue = true;
        use.description = "Take the angle from the document's global light, so one change relights every effect";
        use.anim = FxAnim::Step;
        use.interp = ParamInterp::Stepped;
        auto at = std::find_if(def.params.begin(), def.params.end(), [&](const ParamDef& p) { return std::string_view(p.key) == angleKey; });
        def.params.insert(at == def.params.end() ? def.params.end() : at + 1, use);
        return def;
    }

}

EffectDef titleart::withGlobalLight(EffectDef def, const char* angleKey, LightRelation relation, const char* altitudeKey) {
    return bindGlobalLight(std::move(def), angleKey, relation, altitudeKey);
}

GeneratorDef titleart::withGlobalLight(GeneratorDef def, const char* angleKey, LightRelation relation, const char* altitudeKey) {
    return bindGlobalLight(std::move(def), angleKey, relation, altitudeKey);
}

// ---- params in documents ----

const ParamDef* titleart::findParam(const std::vector<ParamDef>& params, std::string_view key) {
    for (const ParamDef& p : params) {
        if (key == p.key) return &p;
    }
    return nullptr;
}

namespace {

    std::string shortNumber(double v) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.7g", v);
        return text;
    }

    const char* schemaKindOf(ParamKind kind) {
        switch (kind) {
            case ParamKind::Gradient: return "gradient";
            case ParamKind::Curve: return "curve";
            case ParamKind::Adjustment: return "adjustment";
            case ParamKind::Material: return "material";
            default: return nullptr;
        }
    }

    // a param's default as stored (objects through their schema kind)
    Json storedDefault(const ParamDef& p) {
        Json value = p.defaultValue;
        if (const char* kind = schemaKindOf(p.kind)) {
            if (!value.is_object()) value = Json::object();
            NormalizeContext context;
            normalizeTitleArtObject(value, kind, "", context);
        }
        if (p.kind == ParamKind::RowArray && !value.is_array()) value = Json::array();
        return value;
    }

}

bool titleart::normalizeParamValue(const ParamDef& p, Json& value, const std::string& path, NormalizeContext& context) {
    const bool bounded = p.max > p.min;
    auto clamp = [&](bool integer) {
        double x = value.get<double>();
        if (!std::isfinite(x)) return false;
        double fixed = integer ? std::round(x) : x;
        if (bounded) fixed = std::clamp(fixed, p.min, p.max);
        if (fixed != x) {
            context.warn(path, shortNumber(x) + " clamped to " + shortNumber(fixed));
        }
        if (integer) value = (int64_t)fixed;
        else if (fixed != x) value = fixed;
        return true;
    };
    switch (p.kind) {
        case ParamKind::Number:
            return value.is_number() && clamp(false);
        case ParamKind::Integer:
            return value.is_number() && clamp(true);
        case ParamKind::Bool:
            return value.is_boolean();
        case ParamKind::Enum: {
            if (!value.is_string()) return false;
            const std::string v = value.get<std::string>();
            return std::any_of(p.options.begin(), p.options.end(), [&](const EnumOption& o) { return v == o.value; });
        }
        case ParamKind::Color: {
            TitleColor c;
            if (!parseTitleColor(value, c)) return false;
            value = formatTitleColor(c);
            return true;
        }
        case ParamKind::Vec2:
            if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number()) return false;
            return std::isfinite(value[0].get<double>()) && std::isfinite(value[1].get<double>());
        case ParamKind::Asset:
            if (value.is_null()) return true;
            if (value.is_string()) {
                value = Json{{"path", value.get<std::string>()}, {"guid", ""}};
                return true;
            }
            if (!value.is_object()) return false;
            if (!value.contains("path") || !value["path"].is_string()) value["path"] = "";
            if (!value.contains("guid") || !value["guid"].is_string() ||
                (!value["guid"].get<std::string>().empty() && !isTitleArtGuid(value["guid"].get<std::string>()))) {
                value["guid"] = "";
            }
            return true;
        case ParamKind::Layer:
        case ParamKind::String:
            return value.is_string();
        case ParamKind::Gradient:
        case ParamKind::Curve:
        case ParamKind::Adjustment:
        case ParamKind::Material:
            if (!value.is_object()) return false;
            normalizeTitleArtObject(value, schemaKindOf(p.kind), path, context);
            return true;
        case ParamKind::RowArray: {
            if (!value.is_array()) return false;
            Json out = Json::array();
            for (size_t i = 0; i < value.size(); i++) {
                Json row = value[i];
                if (!row.is_object()) {
                    context.warn(jsonPathIndex(path, i), "not an object: dropped");
                    continue;
                }
                normalizeParams(p.rows, row, jsonPathIndex(path, out.size()), context);
                out.push_back(std::move(row));
            }
            if (p.maxRows && out.size() > p.maxRows) {
                context.warn(path, std::to_string(out.size()) + " rows: only the first " + std::to_string(p.maxRows) + " are kept");
                out.erase(out.begin() + (long)p.maxRows, out.end());
            }
            value = std::move(out);
            return true;
        }
    }
    return false;
}

Json titleart::defaultParams(const std::vector<ParamDef>& params) {
    Json values = Json::object();
    std::vector<const ParamDef*> sorted;
    for (const ParamDef& p : params) sorted.push_back(&p);
    std::sort(sorted.begin(), sorted.end(), [](const ParamDef* a, const ParamDef* b) { return std::string_view(a->key) < std::string_view(b->key); });
    for (const ParamDef* p : sorted) values[p->key] = storedDefault(*p);
    return values;
}

void titleart::normalizeParams(const std::vector<ParamDef>& params, Json& values, const std::string& path, NormalizeContext& context) {
    if (!values.is_object()) {
        context.warn(path, "expected an object: the defaults are used");
        values = Json::object();
    }
    std::set<std::string> keys;
    for (auto it = values.begin(); it != values.end(); ++it) keys.insert(it.key());
    for (const ParamDef& p : params) keys.insert(p.key);
    Json out = Json::object();
    for (const std::string& key : keys) {
        const ParamDef* p = findParam(params, key);
        auto it = values.find(key);
        if (!p) {
            out[key] = *it;     // unknown params are kept
            continue;
        }
        const std::string at = jsonPathMember(path, key);
        if (it == values.end()) {
            out[key] = storedDefault(*p);
            continue;
        }
        Json value = *it;
        if (!normalizeParamValue(*p, value, at, context)) {
            context.warn(at, std::string("expected a ") + paramKindName(p->kind) + ": the default is used");
            value = storedDefault(*p);
        }
        out[key] = std::move(value);
    }
    values = std::move(out);
}

uint32_t titleart::titleArtBakeVersion(const TitleArtDoc& doc) {
    std::set<std::string> effects, generators;
    std::function<void(const Json&)> collectEffects = [&](const Json& list) {
        if (!list.is_array()) return;
        for (const Json& e : list) effects.insert(e.value("type", std::string()));
    };
    forEachLayer(doc.root, [&](const Json& layer, const Json*, int) {
        collectEffects(layer.value("effects", Json::array()));
        auto parts = layer.find("part_effects");
        if (parts != layer.end() && parts->is_object()) {
            for (auto it = parts->begin(); it != parts->end(); ++it) collectEffects(it.value());
        }
        if (layerTypeOf(layer) == LayerType::Generator) {
            generators.insert(layer.value("generator", Json::object()).value("type", std::string()));
        }
        return true;
    });
    uint32_t version = TITLE_ART_BAKE_VERSION_BASE;
    for (const std::string& type : effects) {
        if (const EffectDef* def = findEffect(type)) version += def->passVersion;
    }
    for (const std::string& type : generators) {
        if (const GeneratorDef* def = findGenerator(type)) version += def->passVersion;
    }
    return version;
}
