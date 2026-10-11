// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/CompatProperties.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

#include <array>

using namespace jasperengine::htmlui::compat;

namespace {

    struct Parser {
        const char* name;
        const char* parameters;
    };

    struct Definition {
        const char* name;
        const char* defaultValue;
        bool inherited;
        bool forcesLayout;
        Parser parsers[3];
    };

    // phase-2 §3.11.1; the order of CompatProperty
    const Definition kDefinitions[(size_t)CompatProperty::Count] = {
        {"list-style-type", "disc", true, true,
            {{"keyword", "none, disc, circle, square, decimal, decimal-leading-zero, lower-alpha, lower-latin, upper-alpha, "
                         "upper-latin, lower-roman, upper-roman, lower-greek, disclosure-open, disclosure-closed"},
             {"string", ""}, {nullptr, nullptr}}},
        {"list-style-position", "outside", true, true, {{"keyword", "outside, inside"}, {nullptr, nullptr}, {nullptr, nullptr}}},
        {"list-style-image", "none", true, true, {{"keyword", "none"}, {"string", ""}, {nullptr, nullptr}}},
        {"object-fit", "fill", false, false, {{"keyword", "fill, contain, cover, none, scale-down"}, {nullptr, nullptr}, {nullptr, nullptr}}},
        {"object-position", "50% 50%", false, false, {{"string", ""}, {nullptr, nullptr}, {nullptr, nullptr}}},
        {"image-rendering", "auto", true, false,
            {{"keyword", "auto, smooth, high-quality, pixelated, crisp-edges"}, {nullptr, nullptr}, {nullptr, nullptr}}},
        {"-html-animation-fill", "none", false, false, {{"keyword", "none, forwards, backwards, both"}, {nullptr, nullptr}, {nullptr, nullptr}}},
        {"fill", "", true, false, {{"keyword", "none, currentcolor"}, {"color", ""}, {nullptr, nullptr}}},
        {"stroke", "", true, false, {{"keyword", "none, currentcolor"}, {"color", ""}, {nullptr, nullptr}}},
        {"stroke-width", "", true, false, {{"number_length_percent", ""}, {nullptr, nullptr}, {nullptr, nullptr}}},
        {"stroke-opacity", "", true, false, {{"number_percent", ""}, {nullptr, nullptr}, {nullptr, nullptr}}},
        {"fill-opacity", "", true, false, {{"number_percent", ""}, {nullptr, nullptr}, {nullptr, nullptr}}},
        {"stroke-linecap", "", true, false, {{"keyword", "butt, round, square"}, {nullptr, nullptr}, {nullptr, nullptr}}},
        {"stroke-linejoin", "", true, false, {{"keyword", "miter, round, bevel, miter-clip, arcs"}, {nullptr, nullptr}, {nullptr, nullptr}}},
        {"stroke-dasharray", "", true, false, {{"string", ""}, {nullptr, nullptr}, {nullptr, nullptr}}},
    };

    struct State {
        bool registered = false;
        std::array<Rml::PropertyId, (size_t)CompatProperty::Count> ids{};
        std::array<bool, (size_t)CompatProperty::Count> ours{};
    };

    State& state(){
        static State value;
        return value;
    }

}

void CompatProperties::registerAll(){
    State& s = state();
    if (s.registered)
        return;

    for (size_t i = 0; i < (size_t)CompatProperty::Count; i++){
        const Definition& definition = kDefinitions[i];
        s.ours[i] = false;
        // the fork has it (as a property or a shorthand): its definition wins
        if (Rml::StyleSheetSpecification::GetProperty(definition.name) || Rml::StyleSheetSpecification::GetShorthand(definition.name)){
            s.ids[i] = Rml::StyleSheetSpecification::GetPropertyId(definition.name);
            continue;
        }
        Rml::PropertyDefinition& property = Rml::StyleSheetSpecification::RegisterProperty(definition.name, definition.defaultValue,
                                                                                           definition.inherited, definition.forcesLayout);
        for (const Parser& parser : definition.parsers){
            if (parser.name)
                property.AddParser(parser.name, parser.parameters);
        }
        s.ids[i] = property.GetId();
        s.ours[i] = true;
    }
    s.registered = true;
}

void CompatProperties::reset(){
    state() = State();
}

bool CompatProperties::isRegistered(){
    return state().registered;
}

const char* CompatProperties::name(CompatProperty property){
    return property < CompatProperty::Count ? kDefinitions[(size_t)property].name : "";
}

Rml::PropertyId CompatProperties::id(CompatProperty property){
    if (property >= CompatProperty::Count)
        return Rml::PropertyId::Invalid;
    const State& s = state();
    if (s.registered)
        return s.ids[(size_t)property];
    return Rml::StyleSheetSpecification::GetPropertyId(kDefinitions[(size_t)property].name);
}

bool CompatProperties::registeredByCompat(CompatProperty property){
    return property < CompatProperty::Count && state().ours[(size_t)property];
}

std::vector<std::string> CompatProperties::registeredNames(){
    std::vector<std::string> names;
    for (size_t i = 0; i < (size_t)CompatProperty::Count; i++){
        if (state().ours[i])
            names.emplace_back(kDefinitions[i].name);
    }
    return names;
}

const Rml::Property* CompatProperties::get(Rml::Element& element, CompatProperty property){
    const Rml::PropertyId propertyId = id(property);
    if (propertyId == Rml::PropertyId::Invalid)
        return nullptr;
    return element.GetProperty(propertyId);
}

std::string CompatProperties::text(Rml::Element& element, CompatProperty property){
    // the fork's object-position is a shorthand of its two axes (phase 3, p3-paint)
    if (property == CompatProperty::ObjectPosition && !registeredByCompat(property)){
        const Rml::Property* x = element.GetProperty("object-position-x");
        const Rml::Property* y = element.GetProperty("object-position-y");
        if (x && y)
            return x->ToString() + " " + y->ToString();
    }
    const Rml::Property* value = get(element, property);
    if (!value || value->unit == Rml::Unit::UNKNOWN)
        return std::string();
    return value->ToString();
}

bool CompatProperties::color(Rml::Element& element, CompatProperty property, Rml::Colourb& out){
    const Rml::Property* value = get(element, property);
    if (!value || value->unit != Rml::Unit::COLOUR)
        return false;
    out = value->Get<Rml::Colourb>();
    return true;
}
