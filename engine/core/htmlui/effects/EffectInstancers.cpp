// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The RmlUi side of the effects (phase-5 spec 3.6): typed property parsers, the jfx-<name>
// decorator and filter instancers, the fallback `effect` instancers, and what getEffects() reads.

#include "EffectInstancers.h"
#include "EffectRegistry.h"
#include "EffectSyntax.h"

#include "Log.h"

#include "../../../libs/rmlui/Source/Core/BackgroundLayerValues.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/RenderBox.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/StyleSheetTypes.h>

#include <algorithm>
#include <set>
#include <cmath>
#include <functional>
#include <map>

using namespace jasperengine;
using namespace jasperengine::htmlui;

namespace {

    constexpr size_t kDecoratorCacheSize = 256;

    // ---- property values <-> effect values

    Rml::Unit lengthUnit(EffectLength::Unit unit){
        switch (unit){
            case EffectLength::Unit::Px: return Rml::Unit::PX;
            case EffectLength::Unit::Em: return Rml::Unit::EM;
            case EffectLength::Unit::Rem: return Rml::Unit::REM;
            case EffectLength::Unit::Vw: return Rml::Unit::VW;
            case EffectLength::Unit::Vh: return Rml::Unit::VH;
            case EffectLength::Unit::Vmin: return Rml::Unit::VMIN;
            case EffectLength::Unit::Vmax: return Rml::Unit::VMAX;
            case EffectLength::Unit::Percent: return Rml::Unit::PERCENT;
        }
        return Rml::Unit::PX;
    }

    bool lengthUnitOf(Rml::Unit unit, EffectLength::Unit& out){
        switch (unit){
            case Rml::Unit::PX: out = EffectLength::Unit::Px; return true;
            case Rml::Unit::EM: out = EffectLength::Unit::Em; return true;
            case Rml::Unit::REM: out = EffectLength::Unit::Rem; return true;
            case Rml::Unit::VW: out = EffectLength::Unit::Vw; return true;
            case Rml::Unit::VH: out = EffectLength::Unit::Vh; return true;
            case Rml::Unit::VMIN: out = EffectLength::Unit::Vmin; return true;
            case Rml::Unit::VMAX: out = EffectLength::Unit::Vmax; return true;
            case Rml::Unit::PERCENT: out = EffectLength::Unit::Percent; return true;
            default: return false;
        }
    }

    uint8_t toByte(float c){
        return (uint8_t)std::lround(std::min(std::max(c, 0.f), 1.f) * 255.f);
    }

    Rml::Colourb colourOf(const Vector4& linear){
        return Rml::Colourb(toByte(EffectValueParser::linearToSrgb(linear.x)), toByte(EffectValueParser::linearToSrgb(linear.y)),
                            toByte(EffectValueParser::linearToSrgb(linear.z)), toByte(linear.w));
    }

    Vector4 linearOf(Rml::Colourb c){
        return Vector4(EffectValueParser::srgbToLinear(c.red / 255.f), EffectValueParser::srgbToLinear(c.green / 255.f),
                       EffectValueParser::srgbToLinear(c.blue / 255.f), c.alpha / 255.f);
    }

    // the options of an enum parser's parameter map, by index
    std::vector<std::string> optionsOf(const Rml::ParameterMap& parameters){
        std::vector<std::string> options(parameters.size());
        for (const auto& entry : parameters){
            if (entry.second >= 0 && entry.second < (int)options.size())
                options[entry.second] = entry.first;
        }
        return options;
    }

    std::string joinOptions(const std::vector<std::string>& options){
        std::string text;
        for (const std::string& option : options)
            text += (text.empty() ? "" : ",") + option;
        return text;
    }

    // a property of a slot back into an effect value (a raw string is parsed again)
    bool valueOfProperty(const EffectParamDef& def, const Rml::Property* p, const Rml::Property* py, EffectValue& out, std::string& problem){
        out = EffectValue();
        out.type = def.type;
        if (!p){
            std::string error;
            EffectValueParser::parse(def, def.defaultText, out, error);
            return true;
        }
        // a value the parser kept as text: parsed now (the default when it is not valid)
        auto reparse = [&](const std::string& text) -> bool {
            std::string reason;
            if (EffectValueParser::parse(def, text, out, reason) && !out.isVar)
                return true;
            problem = "'" + text + "' is not a valid " + def.name + " (" + (reason.empty() ? std::string("unresolved var()") : reason) +
                      "); the default is used";
            EffectValueParser::parse(def, def.defaultText, out, reason);
            return false;
        };
        if (p->unit == Rml::Unit::STRING && def.type != EffectParamType::Position)
            return reparse(p->Get<Rml::String>());
        switch (def.type){
            case EffectParamType::Number:
            case EffectParamType::Integer:
            case EffectParamType::Percentage:
            case EffectParamType::Time:
            case EffectParamType::Angle:
                out.f = p->Get<float>();
                if (def.type == EffectParamType::Integer)
                    out.f = std::round(out.f);
                return true;
            case EffectParamType::Length: {
                EffectLength::Unit unit;
                if (!lengthUnitOf(p->unit, unit))
                    return reparse(p->ToString());
                out.length.number = p->Get<float>();
                out.length.unit = unit;
                if (unit == EffectLength::Unit::Px)
                    out.f = out.length.number;
                return true;
            }
            case EffectParamType::Color:
                out.color = linearOf(p->Get<Rml::Colourb>());
                return true;
            case EffectParamType::Enum:
            case EffectParamType::Boolean:
                out.f = (float)p->Get<int>();
                return true;
            case EffectParamType::Palette: {
                if (p->unit != Rml::Unit::COLORSTOPLIST)
                    return reparse(p->ToString());
                const Rml::ColorStopList& stops = p->value.GetReference<Rml::ColorStopList>();
                for (const Rml::ColorStop& stop : stops){
                    Rml::Colourb c = stop.color.ToNonPremultiplied();
                    out.palette.push_back({linearOf(c), stop.position.number});
                }
                return true;
            }
            case EffectParamType::Position: {
                // each axis: a fraction, px, or the whole position as text
                auto axisOf = [&](const Rml::Property* q, char axis, EffectPositionAxis& result) -> bool {
                    if (!q){
                        result = EffectPositionAxis{0.5f, 0.f};
                        return true;
                    }
                    if (q->unit == Rml::Unit::NUMBER){
                        result = EffectPositionAxis{q->Get<float>(), 0.f};
                        return true;
                    }
                    if (q->unit == Rml::Unit::PX){
                        result = EffectPositionAxis{0.f, q->Get<float>()};
                        return true;
                    }
                    EffectValue whole;
                    std::string reason;
                    const std::string text = q->Get<Rml::String>();
                    if (q->unit == Rml::Unit::STRING && EffectValueParser::parse(def, text, whole, reason) && !whole.isVar){
                        result = axis == 'x' ? whole.x : whole.y;
                        return true;
                    }
                    problem = "'" + text + "' is not a valid " + def.name + "; the default is used";
                    return false;
                };
                EffectValue fallback;
                std::string reason;
                EffectValueParser::parse(def, def.defaultText, fallback, reason);
                const bool okX = axisOf(p, 'x', out.x);
                const bool okY = axisOf(py, 'y', out.y);
                if (!okX)
                    out.x = fallback.x;
                if (!okY)
                    out.y = fallback.y;
                return okX && okY;
            }
        }
        return true;
    }

    // registers one property per slot of an effect form; the shorthand over all of them
    EffectSlots registerSlots(Rml::EffectSpecification& specification, const EffectManifest& manifest, const char* shorthandName,
                              std::function<Rml::PropertyDefinition&(const std::string&, const std::string&)> registerProperty,
                              std::function<void(const std::string&, const std::string&)> registerShorthand){
        EffectSlots slots;
        (void)specification;
        std::string names;
        auto add = [&](const std::string& name){
            names += (names.empty() ? "" : ", ") + name;
        };
        for (const EffectParamDef& def : manifest.params){
            const std::string defaultText = EffectSyntax::defaultCanonical(def);
            if (def.type == EffectParamType::Position){
                Rml::PropertyDefinition& x = registerProperty(def.name + "-x", defaultText);
                x.AddParser(EffectPropertyParser::parserName(def.type, 'x'));
                Rml::PropertyDefinition& y = registerProperty(def.name + "-y", defaultText);
                y.AddParser(EffectPropertyParser::parserName(def.type, 'y'));
                slots.params.push_back(x.GetId());
                slots.positionY.push_back(y.GetId());
                add(def.name + "-x");
                add(def.name + "-y");
                continue;
            }
            Rml::PropertyDefinition& property = registerProperty(def.name, defaultText);
            if (def.type == EffectParamType::Enum)
                property.AddParser(EffectPropertyParser::parserName(def.type), joinOptions(def.options));
            else
                property.AddParser(EffectPropertyParser::parserName(def.type));
            slots.params.push_back(property.GetId());
            slots.positionY.push_back(Rml::PropertyId::Invalid);
            add(def.name);
        }
        for (const EffectParamDef& def : EffectSyntax::universalParams()){
            const bool blend = def.name == "blend";
            if (blend && manifest.kind != EffectKind::Paint)
                continue;
            Rml::PropertyDefinition& property = registerProperty(def.name, EffectSyntax::defaultCanonical(def));
            if (def.type == EffectParamType::Enum)
                property.AddParser(EffectPropertyParser::parserName(def.type), joinOptions(def.options));
            else
                property.AddParser(EffectPropertyParser::parserName(def.type));
            if (def.name == "opacity") slots.opacity = property.GetId();
            else if (def.name == "time-scale") slots.timeScale = property.GetId();
            else if (def.name == "time-offset") slots.timeOffset = property.GetId();
            else if (blend){
                slots.blend = property.GetId();
                slots.hasBlend = true;
            }
            add(def.name);
        }
        registerShorthand(shorthandName, names);
        return slots;
    }

    EffectSnapshotResult snapshotOfSlots(const EffectManifest& manifest, const EffectSlots& slots, const Rml::PropertyDictionary& properties){
        EffectSnapshotResult result;
        auto snapshot = std::make_shared<EffectParamsSnapshot>();
        snapshot->manifest = &manifest;
        for (size_t i = 0; i < manifest.params.size(); i++){
            const EffectParamDef& def = manifest.params[i];
            const Rml::Property* p = i < slots.params.size() ? properties.GetProperty(slots.params[i]) : nullptr;
            const Rml::Property* py = (i < slots.positionY.size() && slots.positionY[i] != Rml::PropertyId::Invalid)
                                      ? properties.GetProperty(slots.positionY[i]) : nullptr;
            EffectValue value;
            std::string problem;
            valueOfProperty(def, p, py, value, problem);
            if (!problem.empty())
                result.problems.push_back(problem);
            EffectValue before = value;
            if (EffectValueParser::clamp(def, value))
                result.problems.push_back("'" + EffectValueParser::canonical(def, before) + "' is outside the range of " + def.name +
                                          "; clamped to " + EffectValueParser::canonical(def, value));
            snapshot->values.push_back(value);
        }
        auto number = [&](Rml::PropertyId id, float fallback){
            const Rml::Property* p = properties.GetProperty(id);
            return (p && p->unit == Rml::Unit::NUMBER) ? p->Get<float>() : fallback;
        };
        snapshot->opacity = std::min(std::max(number(slots.opacity, 1.f), 0.f), 1.f);
        snapshot->timeScale = std::min(std::max(number(slots.timeScale, 1.f), -10.f), 10.f);
        snapshot->timeOffset = std::min(std::max(number(slots.timeOffset, 0.f), -3600.f), 3600.f);
        if (slots.hasBlend){
            const Rml::Property* p = properties.GetProperty(slots.blend);
            snapshot->blend = (p && p->unit == Rml::Unit::KEYWORD) ? std::min(std::max(p->Get<int>(), 0), 3) : 0;
        }
        finishSnapshot(*snapshot);
        result.snapshot = snapshot;
        return result;
    }

    std::string cacheKeyOf(const EffectParamsSnapshot& snapshot){
        std::string key = snapshot.manifest ? snapshot.manifest->name : std::string();
        for (const auto& entry : snapshot.canonical){
            key += '|';
            key += entry.second;
        }
        return key;
    }

    void reportProblems(Rml::Element* element, const EffectManifest* manifest, const std::vector<std::string>& problems, bool& reported){
        if (reported || problems.empty())
            return;
        reported = true;
        const std::string where = element ? element->GetAddress() : std::string("?");
        for (const std::string& problem : problems)
            Log::warn("[HTML UI] effect(%s) on %s: %s", manifest ? manifest->name.c_str() : "?", where.c_str(), problem.c_str());
    }

    // decorator data: the instance (owned by the compiled shader) and its geometry
    struct DecoratorData {
        Rml::Geometry geometry;
        Rml::CompiledShader shader;
        Rml::Context* context = nullptr;      // the element's, asked to redraw while the clock runs
    };

    float scaledLength(Rml::Element* element, const EffectParamsSnapshot& snapshot, const EffectScaledParam& scaled, Rml::Vector2f area){
        float value = 0.f;
        if (!scaled.param.empty()){
            const int index = snapshot.manifest->paramIndex(scaled.param);
            if (index >= 0 && index < (int)snapshot.values.size()){
                const EffectValue& v = snapshot.values[index];
                if (v.type == EffectParamType::Length){
                    EffectLengthContext context;
                    // the font sizes and the viewport only for the units that read them (a filter's
                    // ink overflow is asked for every frame)
                    if (v.length.unit != EffectLength::Unit::Px && v.length.unit != EffectLength::Unit::Percent){
                        context.fontSize = element->GetComputedValues().font_size();
                        if (Rml::ElementDocument* document = element->GetOwnerDocument())
                            context.rootFontSize = document->GetComputedValues().font_size();
                        if (Rml::Context* rml = element->GetContext())
                            context.viewport = Vector2((float)rml->GetDimensions().x, (float)rml->GetDimensions().y);
                    }
                    context.areaShorterSide = std::min(area.x, area.y);
                    value = EffectValueParser::resolveLength(v.length, context);
                }else{
                    value = v.f;
                }
            }
        }
        return value * scaled.scale + scaled.addPx;
    }

}

// ---- parsers

EffectPropertyParser::EffectPropertyParser(EffectParamType type, char axis): type(type), axis(axis){
}

const char* EffectPropertyParser::parserName(EffectParamType type, char axis){
    switch (type){
        case EffectParamType::Number: return "jfx-number";
        case EffectParamType::Integer: return "jfx-integer";
        case EffectParamType::Percentage: return "jfx-percentage";
        case EffectParamType::Length: return "jfx-length";
        case EffectParamType::Angle: return "jfx-angle";
        case EffectParamType::Time: return "jfx-time";
        case EffectParamType::Color: return "jfx-color";
        case EffectParamType::Position: return axis == 'y' ? "jfx-position-y" : "jfx-position-x";
        case EffectParamType::Enum: return "jfx-enum";
        case EffectParamType::Boolean: return "jfx-boolean";
        case EffectParamType::Palette: return "jfx-palette";
    }
    return "jfx-number";
}

bool EffectPropertyParser::ParseValue(Rml::Property& property, const Rml::String& value, const Rml::ParameterMap& parameters) const{
    std::string inner;
    char slotAxis = 0;
    if (!EffectSyntax::unwrapSlot(value, inner, slotAxis))
        inner = value;
    const std::vector<std::string> options = type == EffectParamType::Enum ? optionsOf(parameters) : std::vector<std::string>();
    EffectValue v;
    std::string error;
    if (!EffectValueParser::parseType(type, options, inner, v, error) || v.isVar){
        // kept as text, parsed again (and reported) when the effect is used: a slot never fails,
        // so the FallThrough shorthand keeps every slot in its place
        property.value = Rml::Variant(Rml::String(inner));
        property.unit = Rml::Unit::STRING;
        return true;
    }
    switch (type){
        case EffectParamType::Number:
        case EffectParamType::Integer:
        case EffectParamType::Percentage:
        case EffectParamType::Time:
            property.value = Rml::Variant(v.f);
            property.unit = Rml::Unit::NUMBER;
            return true;
        case EffectParamType::Angle:
            property.value = Rml::Variant(v.f);
            property.unit = Rml::Unit::RAD;
            return true;
        case EffectParamType::Length:
            property.value = Rml::Variant(v.length.number);
            property.unit = lengthUnit(v.length.unit);
            return true;
        case EffectParamType::Color:
            if (v.currentColor){
                property.value = Rml::Variant(Rml::String("currentcolor"));
                property.unit = Rml::Unit::STRING;
                return true;
            }
            property.value = Rml::Variant(colourOf(v.color));
            property.unit = Rml::Unit::COLOUR;
            return true;
        case EffectParamType::Position: {
            const EffectPositionAxis& a = axis == 'y' ? v.y : v.x;
            if (a.px == 0.f){
                property.value = Rml::Variant(a.fraction);
                property.unit = Rml::Unit::NUMBER;
            }else if (a.fraction == 0.f){
                property.value = Rml::Variant(a.px);
                property.unit = Rml::Unit::PX;
            }else{
                property.value = Rml::Variant(Rml::String(inner));
                property.unit = Rml::Unit::STRING;
            }
            return true;
        }
        case EffectParamType::Enum:
        case EffectParamType::Boolean:
            property.value = Rml::Variant((int)std::lround(v.f));
            property.unit = Rml::Unit::KEYWORD;
            return true;
        case EffectParamType::Palette: {
            Rml::ColorStopList stops;
            for (const auto& entry : v.palette){
                Rml::ColorStop stop;
                stop.color = colourOf(entry.first).ToPremultiplied();
                stop.position = Rml::NumericValue(entry.second, Rml::Unit::NUMBER);
                stops.push_back(stop);
            }
            property.value = Rml::Variant(std::move(stops));
            property.unit = Rml::Unit::COLORSTOPLIST;
            return true;
        }
    }
    return false;
}

// ---- snapshots

void jasperengine::htmlui::applyEffectPreset(const EffectManifest& m, std::vector<EffectValue>& values, const std::set<std::string>& written){
    if (m.presets.empty())
        return;
    const int presetIndex = m.paramIndex("preset");
    if (presetIndex < 0 || (size_t)presetIndex >= values.size() || values[(size_t)presetIndex].isVar)
        return;
    const EffectParamDef& presetDef = m.params[(size_t)presetIndex];
    const EffectPresetDef* preset = m.findPreset(EffectValueParser::canonical(presetDef, values[(size_t)presetIndex]));
    if (!preset)
        return;
    // a parameter the author did not write takes the preset's value
    for (const auto& entry : preset->values){
        const int index = m.paramIndex(entry.first);
        if (index < 0 || (size_t)index >= values.size() || written.count(entry.first))
            continue;
        const EffectParamDef& def = m.params[(size_t)index];
        EffectValue value;
        std::string reason;
        if (!EffectValueParser::parse(def, entry.second, value, reason) || value.isVar)
            continue;
        EffectValueParser::clamp(def, value);
        values[(size_t)index] = value;
    }
}

void jasperengine::htmlui::finishSnapshot(EffectParamsSnapshot& snapshot){
    snapshot.canonical.clear();
    if (!snapshot.manifest)
        return;
    const EffectManifest& m = *snapshot.manifest;
    for (size_t i = 0; i < m.params.size() && i < snapshot.values.size(); i++)
        snapshot.canonical.push_back({m.params[i].name, EffectValueParser::canonical(m.params[i], snapshot.values[i])});
    const std::vector<EffectParamDef>& universal = EffectSyntax::universalParams();
    for (const EffectParamDef& def : universal){
        EffectValue v;
        v.type = def.type;
        if (def.name == "opacity") v.f = snapshot.opacity;
        else if (def.name == "time-scale") v.f = snapshot.timeScale;
        else if (def.name == "time-offset") v.f = snapshot.timeOffset;
        else if (def.name == "blend"){
            if (m.kind != EffectKind::Paint)
                continue;
            v.f = (float)snapshot.blend;
        }
        snapshot.canonical.push_back({def.name, EffectValueParser::canonical(def, v)});
    }
}

EffectSnapshotResult jasperengine::htmlui::snapshotFromCall(const std::string& arguments, bool filterForm){
    EffectSnapshotResult result;
    EffectCall call;
    std::vector<EffectDiagnostic> diags;
    if (!EffectSyntax::parseArguments(arguments, call, diags)){
        for (const EffectDiagnostic& d : diags)
            result.problems.push_back(d.message);
        return result;
    }
    const EffectManifest* manifest = EffectRegistry::get().find(call.name);
    if (!manifest){
        std::string message = "unknown effect '" + call.name + "'";
        const std::vector<std::string> suggestions = EffectRegistry::get().suggestions(call.name);
        if (!suggestions.empty())
            message += " (did you mean " + suggestions.front() + "?)";
        result.problems.push_back(message + "; dropped");
        return result;
    }
    if (!filterForm && manifest->kind == EffectKind::Filter){
        result.problems.push_back("'" + manifest->name + "' is a filter effect: it goes in filter or backdrop-filter; dropped");
        return result;
    }
    // the typed form's checks (unknown parameters, bad values, ranges), then its values
    std::string rcss = EffectSyntax::toRcss(call, *manifest, filterForm, std::string_view(), diags);
    for (const EffectDiagnostic& d : diags)
        result.problems.push_back(d.message);
    auto snapshot = std::make_shared<EffectParamsSnapshot>();
    snapshot->manifest = manifest;
    std::map<std::string, std::string> given;
    for (const EffectArg& arg : call.args)
        given[arg.name] = arg.value;
    for (const EffectParamDef& def : manifest->params){
        EffectValue value;
        std::string reason;
        auto it = given.find(def.name);
        if (it == given.end() || it->second.empty() || !EffectValueParser::parse(def, it->second, value, reason) || value.isVar)
            EffectValueParser::parse(def, def.defaultText, value, reason);
        EffectValueParser::clamp(def, value);
        snapshot->values.push_back(value);
    }
    // @preset: the parameters the call does not write take the preset's values
    std::set<std::string> written;
    for (const auto& entry : given)
        if (!entry.second.empty())
            written.insert(entry.first);
    applyEffectPreset(*manifest, snapshot->values, written);
    auto universal = [&](const char* name, float fallback, float lo, float hi){
        auto it = given.find(name);
        if (it == given.end())
            return fallback;
        for (const EffectParamDef& def : EffectSyntax::universalParams()){
            if (def.name != name)
                continue;
            EffectValue value;
            std::string reason;
            if (!EffectValueParser::parse(def, it->second, value, reason) || value.isVar)
                return fallback;
            return std::min(std::max(value.f, lo), hi);
        }
        return fallback;
    };
    snapshot->opacity = universal("opacity", 1.f, 0.f, 1.f);
    snapshot->timeScale = universal("time-scale", 1.f, -10.f, 10.f);
    snapshot->timeOffset = universal("time-offset", 0.f, -3600.f, 3600.f);
    snapshot->blend = manifest->kind == EffectKind::Paint ? (int)universal("blend", 0.f, 0.f, 3.f) : 0;
    finishSnapshot(*snapshot);
    result.snapshot = snapshot;
    return result;
}

// ---- the decorator

EffectDecorator::EffectDecorator(std::shared_ptr<const EffectParamsSnapshot> snapshot, std::vector<std::string> problems):
    snapshot(std::move(snapshot)), problems(std::move(problems)){
}

Rml::DecoratorDataHandle EffectDecorator::GenerateElementData(Rml::Element* element, Rml::BoxArea paint_area) const{
    Rml::RenderManager* renderManager = element->GetRenderManager();
    if (!renderManager || !snapshot || !snapshot->manifest)
        return INVALID_DECORATORDATAHANDLE;
    reportProblems(element, snapshot->manifest, problems, reported);
    const EffectManifest& m = *snapshot->manifest;

    auto* data = new DecoratorData();
    data->context = element->GetContext();
    EffectRuntime& runtime = EffectRuntime::get();
    EffectInstance* instance = runtime.acquire(element, m, false, snapshot, (int)paint_area);
    data->shader = renderManager->CompileShader("jfx", Rml::Dictionary{{"instance", Rml::Variant((void*)instance)}});
    // the handle owns the instance; a render interface that is not the engine's gives none back
    if (!data->shader)
        runtime.release(instance);

    // a quad over the paint area (the shader clips to its rounded box), or over the area grown
    // by @geometry bounds; texture coordinates 0..1 over the area
    const Rml::RenderBox box = element->GetRenderBox(paint_area);
    const Rml::Vector2f size = box.GetFillSize();
    const Rml::Vector2f offset = box.GetFillOffset();
    if (size.x > 0.f && size.y > 0.f){
        float extent = 0.f;
        if (m.boundsGeometry)
            extent = std::max(0.f, scaledLength(element, *snapshot, m.boundsExtent, size));
        const Rml::Vector2f p0 = offset - Rml::Vector2f(extent);
        const Rml::Vector2f p1 = offset + size + Rml::Vector2f(extent);
        const Rml::byte alpha = (Rml::byte)std::lround(std::min(std::max(element->GetDrawOpacity(), 0.f), 1.f) * 255.f);
        const Rml::ColourbPremultiplied colour(alpha, alpha);
        Rml::Mesh mesh;
        const Rml::Vector2f corners[4] = {p0, Rml::Vector2f(p1.x, p0.y), p1, Rml::Vector2f(p0.x, p1.y)};
        for (const Rml::Vector2f& corner : corners){
            Rml::Vertex vertex;
            vertex.position = corner;
            vertex.colour = colour;
            vertex.tex_coord = (corner - offset) / size;
            mesh.vertices.push_back(vertex);
        }
        mesh.indices = {0, 1, 2, 0, 2, 3};
        data->geometry = renderManager->MakeGeometry(std::move(mesh));
    }
    return reinterpret_cast<Rml::DecoratorDataHandle>(data);
}

void EffectDecorator::ReleaseElementData(Rml::DecoratorDataHandle element_data) const{
    delete reinterpret_cast<DecoratorData*>(element_data);
}

void EffectDecorator::RenderElement(Rml::Element* element, Rml::DecoratorDataHandle element_data) const{
    auto* data = reinterpret_cast<DecoratorData*>(element_data);
    if (!data || !data->shader || !data->geometry)
        return;
    data->geometry.Render(element->GetAbsoluteOffset(Rml::BoxArea::Border), {}, data->shader);
    // a running clock redraws the effect every frame (the editor redraws on GetNextUpdateDelay)
    if (!EffectClock::isFixed())
        EffectRuntime::get().requestRedraw(data->context);
}

// ---- the filter

EffectFilter::EffectFilter(std::shared_ptr<const EffectParamsSnapshot> snapshot, std::vector<std::string> problems):
    snapshot(std::move(snapshot)), problems(std::move(problems)){
}

Rml::CompiledFilter EffectFilter::CompileFilter(Rml::Element* element) const{
    Rml::RenderManager* renderManager = element->GetRenderManager();
    if (!renderManager || !snapshot || !snapshot->manifest)
        return {};
    reportProblems(element, snapshot->manifest, problems, reported);
    EffectRuntime& runtime = EffectRuntime::get();
    runtime.countFilterCompile();
    EffectInstance* instance = runtime.acquire(element, *snapshot->manifest, true, snapshot, (int)Rml::BoxArea::Border);
    Rml::CompiledFilter compiled = renderManager->CompileFilter("jfx", Rml::Dictionary{{"instance", Rml::Variant((void*)instance)}});
    if (!compiled)
        runtime.release(instance);
    if (!EffectClock::isFixed()){
        if (Rml::Context* context = element->GetContext())
            context->RequestNextUpdate(0);
    }
    return compiled;
}

void EffectFilter::ExtendInkOverflow(Rml::Element* element, Rml::Rectanglef& overflow) const{
    if (!snapshot || !snapshot->manifest)
        return;
    const EffectManifest& m = *snapshot->manifest;
    if (m.margin.param.empty() && m.margin.addPx == 0.f)
        return;
    const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
    const float margin = std::max(0.f, scaledLength(element, *snapshot, m.margin, size));
    overflow = overflow.Extend(margin);
}

// ---- typed instancers

EffectDecoratorInstancer::EffectDecoratorInstancer(const EffectManifest& manifest): manifest(manifest){
    slots = registerSlots(*this, manifest, "decorator",
        [this](const std::string& name, const std::string& defaultText) -> Rml::PropertyDefinition& { return RegisterProperty(name, defaultText); },
        [this](const std::string& name, const std::string& items){ RegisterShorthand(name, items, Rml::ShorthandType::FallThrough); });
}

EffectSnapshotResult EffectDecoratorInstancer::snapshotOf(const Rml::PropertyDictionary& properties) const{
    return snapshotOfSlots(manifest, slots, properties);
}

Rml::SharedPtr<Rml::Decorator> EffectDecoratorInstancer::InstanceDecorator(const Rml::String& /*name*/, const Rml::PropertyDictionary& properties,
                                                                           const Rml::DecoratorInstancerInterface& /*instancer_interface*/){
    EffectSnapshotResult result = snapshotOf(properties);
    if (!result.snapshot)
        return nullptr;
    EffectRuntime::get().countReinstance();
    // repeated values share one decorator
    const std::string key = cacheKeyOf(*result.snapshot);
    auto it = cacheIndex.find(key);
    if (it != cacheIndex.end()){
        cache.splice(cache.end(), cache, it->second);
        return it->second->second;
    }
    auto decorator = Rml::MakeShared<EffectDecorator>(result.snapshot, std::move(result.problems));
    cache.push_back({key, decorator});
    cacheIndex[key] = std::prev(cache.end());
    while (cache.size() > kDecoratorCacheSize){
        cacheIndex.erase(cache.front().first);
        cache.pop_front();
    }
    return decorator;
}

EffectFilterInstancer::EffectFilterInstancer(const EffectManifest& manifest): manifest(manifest){
    slots = registerSlots(*this, manifest, "filter",
        [this](const std::string& name, const std::string& defaultText) -> Rml::PropertyDefinition& { return RegisterProperty(name, defaultText); },
        [this](const std::string& name, const std::string& items){ RegisterShorthand(name, items, Rml::ShorthandType::FallThrough); });
}

EffectSnapshotResult EffectFilterInstancer::snapshotOf(const Rml::PropertyDictionary& properties) const{
    return snapshotOfSlots(manifest, slots, properties);
}

Rml::SharedPtr<Rml::Filter> EffectFilterInstancer::InstanceFilter(const Rml::String& /*name*/, const Rml::PropertyDictionary& properties){
    EffectSnapshotResult result = snapshotOf(properties);
    if (!result.snapshot)
        return nullptr;
    EffectRuntime::get().countReinstance();
    return Rml::MakeShared<EffectFilter>(result.snapshot, std::move(result.problems));
}

// ---- fallback instancers

RawEffectDecoratorInstancer::RawEffectDecoratorInstancer(){
    valueId = RegisterProperty("value", "").AddParser("string").GetId();
    RegisterShorthand("decorator", "value#", Rml::ShorthandType::RecursiveCommaSeparated);
}

EffectSnapshotResult RawEffectDecoratorInstancer::snapshotOf(const Rml::PropertyDictionary& properties) const{
    const Rml::Property* p = properties.GetProperty(valueId);
    return snapshotFromCall(p ? p->Get<Rml::String>() : Rml::String(), false);
}

Rml::SharedPtr<Rml::Decorator> RawEffectDecoratorInstancer::InstanceDecorator(const Rml::String& /*name*/, const Rml::PropertyDictionary& properties,
                                                                              const Rml::DecoratorInstancerInterface& /*instancer_interface*/){
    EffectSnapshotResult result = snapshotOf(properties);
    if (!result.snapshot){
        const Rml::Property* p = properties.GetProperty(valueId);
        for (const std::string& problem : result.problems)
            Log::warn("[HTML UI] effect(%s): %s", p ? p->Get<Rml::String>().c_str() : "", problem.c_str());
        return nullptr;
    }
    return Rml::MakeShared<EffectDecorator>(result.snapshot, std::move(result.problems));
}

RawEffectFilterInstancer::RawEffectFilterInstancer(){
    valueId = RegisterProperty("value", "").AddParser("string").GetId();
    RegisterShorthand("filter", "value#", Rml::ShorthandType::RecursiveCommaSeparated);
}

EffectSnapshotResult RawEffectFilterInstancer::snapshotOf(const Rml::PropertyDictionary& properties) const{
    const Rml::Property* p = properties.GetProperty(valueId);
    return snapshotFromCall(p ? p->Get<Rml::String>() : Rml::String(), true);
}

Rml::SharedPtr<Rml::Filter> RawEffectFilterInstancer::InstanceFilter(const Rml::String& /*name*/, const Rml::PropertyDictionary& properties){
    EffectSnapshotResult result = snapshotOf(properties);
    if (!result.snapshot){
        const Rml::Property* p = properties.GetProperty(valueId);
        for (const std::string& problem : result.problems)
            Log::warn("[HTML UI] effect(%s): %s", p ? p->Get<Rml::String>().c_str() : "", problem.c_str());
        return nullptr;
    }
    return Rml::MakeShared<EffectFilter>(result.snapshot, std::move(result.problems));
}

// ---- what an element uses (getEffects)

namespace {

    void addDecoratorUse(std::vector<EffectUse>& uses, const std::string& property, int index, const Rml::DecoratorDeclaration& declaration){
        EffectSnapshotResult result;
        if (auto* typed = dynamic_cast<EffectDecoratorInstancer*>(declaration.instancer))
            result = typed->snapshotOf(declaration.properties);
        else if (auto* raw = dynamic_cast<RawEffectDecoratorInstancer*>(declaration.instancer))
            result = raw->snapshotOf(declaration.properties);
        else
            return;
        if (!result.snapshot || !result.snapshot->manifest)
            return;
        EffectUse use;
        use.property = property;
        use.index = index;
        use.name = result.snapshot->manifest->name;
        use.filterForm = false;
        use.params = result.snapshot->canonical;
        uses.push_back(std::move(use));
    }

    void addFilterUses(std::vector<EffectUse>& uses, const std::string& property, const Rml::Property* p){
        if (!p || p->unit != Rml::Unit::FILTER || p->value.GetType() != Rml::Variant::FILTERSPTR)
            return;
        const Rml::FiltersPtr& filters = p->value.GetReference<Rml::FiltersPtr>();
        if (!filters)
            return;
        for (size_t i = 0; i < filters->list.size(); i++){
            const Rml::FilterDeclaration& declaration = filters->list[i];
            EffectSnapshotResult result;
            if (auto* typed = dynamic_cast<EffectFilterInstancer*>(declaration.instancer))
                result = typed->snapshotOf(declaration.properties);
            else if (auto* raw = dynamic_cast<RawEffectFilterInstancer*>(declaration.instancer))
                result = raw->snapshotOf(declaration.properties);
            else
                continue;
            if (!result.snapshot || !result.snapshot->manifest)
                continue;
            EffectUse use;
            use.property = property;
            use.index = (int)i + 1;
            use.name = result.snapshot->manifest->name;
            use.filterForm = true;
            use.params = result.snapshot->canonical;
            uses.push_back(std::move(use));
        }
    }

    void addLayerUses(std::vector<EffectUse>& uses, const std::string& property, const Rml::Property* p){
        if (!p || p->unit != Rml::Unit::STYLE_VALUE)
            return;
        const auto* list = rmlui_dynamic_cast<const Rml::ImageLayerList*>(p->value.GetReference<Rml::StyleValuePtr>().get());
        if (!list || !list->decorators)
            return;
        for (size_t i = 0; i < list->layers.size(); i++){
            const Rml::ImageLayer& layer = list->layers[i];
            if (layer.kind != Rml::ImageLayer::Kind::Decorator || layer.decorator_index < 0 ||
                layer.decorator_index >= (int)list->decorators->list.size())
                continue;
            addDecoratorUse(uses, property, (int)i + 1, list->decorators->list[layer.decorator_index]);
        }
    }

}

std::vector<EffectUse> jasperengine::htmlui::collectEffectUses(Rml::Element* element){
    std::vector<EffectUse> uses;
    if (!element)
        return uses;
    addLayerUses(uses, "background-image", element->GetProperty(Rml::PropertyId::BackgroundImage));
    // the decorator property (compat without the fork's background layers)
    if (const Rml::Property* p = element->GetProperty(Rml::PropertyId::Decorator)){
        if (p->unit == Rml::Unit::DECORATOR && p->value.GetType() == Rml::Variant::DECORATORSPTR){
            if (const Rml::DecoratorsPtr& decorators = p->value.GetReference<Rml::DecoratorsPtr>()){
                for (size_t i = 0; i < decorators->list.size(); i++)
                    addDecoratorUse(uses, "background-image", (int)i + 1, decorators->list[i]);
            }
        }
    }
    addLayerUses(uses, "mask-image", element->GetProperty(Rml::PropertyId::MaskImage));
    // gradient text: the effect layers of background-image drawn in filter form (-jfx-text-fill)
    const Rml::PropertyId textFill = Rml::StyleSheetSpecification::GetPropertyId("-jfx-text-fill");
    if (textFill != Rml::PropertyId::Invalid){
        std::vector<EffectUse> fills;
        addFilterUses(fills, "background-image", element->GetProperty(textFill));
        // bottom layer first in the list: the source order is the reverse
        for (size_t i = 0; i < fills.size(); i++)
            fills[i].index = (int)(fills.size() - i);
        uses.insert(uses.end(), fills.rbegin(), fills.rend());
    }
    addFilterUses(uses, "filter", element->GetProperty(Rml::PropertyId::Filter));
    addFilterUses(uses, "backdrop-filter", element->GetProperty(Rml::PropertyId::BackdropFilter));
    return uses;
}
