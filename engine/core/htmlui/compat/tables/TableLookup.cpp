// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lookups over the support tables (the data is in the other files of this directory).

#include "htmlui/compat/SupportTables.h"
#include "htmlui/compat/StringIndex.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

using namespace jasperengine::htmlui::compat;

namespace {

    char toLower(char c){
        return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }

    bool equalsIgnoreCase(std::string_view a, std::string_view b){
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++){
            if (toLower(a[i]) != toLower(b[i]))
                return false;
        }
        return true;
    }

    bool startsWith(std::string_view text, std::string_view prefix){
        return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
    }

    // name matches a table pattern: exact, "data-*" (prefix), "on*" (prefix + at least one char)
    bool matchesPattern(std::string_view pattern, std::string_view name){
        if (!pattern.empty() && pattern.back() == '*'){
            const std::string_view prefix = pattern.substr(0, pattern.size() - 1);
            return name.size() > prefix.size() && startsWith(name, prefix);
        }
        return equalsIgnoreCase(pattern, name);
    }

}

const PropertySupport* jasperengine::htmlui::compat::findProperty(std::string_view name){
    if (startsWith(name, "--"))
        name = "--*";

    // the rewriter looks up every declaration (several times): an index built once
    static const Span<const PropertySupport> rows = propertyTable();
    static const StringIndex index = [](){
        std::vector<std::pair<std::string_view, uint32_t>> entries;
        for (size_t i = 0; i < rows.size(); i++)
            entries.emplace_back(std::string_view(rows[i].name), (uint32_t)i);
        return StringIndex(entries);
    }();
    const uint32_t i = index.find(name);
    return i == StringIndex::npos ? nullptr : &rows[i];
}

Span<const ValueSupport> jasperengine::htmlui::compat::valuesOf(std::string_view property){
    // the rows of a property are consecutive: (first, count) per property, built once
    static const std::unordered_map<std::string_view, std::pair<const ValueSupport*, size_t>> index = [](){
        std::unordered_map<std::string_view, std::pair<const ValueSupport*, size_t>> map;
        for (const ValueSupport& row : valueTable()){
            auto& entry = map[std::string_view(row.property)];
            if (!entry.first)
                entry.first = &row;
            if (entry.first + entry.second == &row)
                entry.second++;
        }
        return map;
    }();
    const auto it = index.find(property);
    return it == index.end() ? Span<const ValueSupport>() : Span<const ValueSupport>(it->second.first, it->second.second);
}

const ValueSupport* jasperengine::htmlui::compat::findValue(std::string_view property, std::string_view value){
    for (const ValueSupport& row : valuesOf(property)){
        if (equalsIgnoreCase(row.value, value))
            return &row;
    }
    return nullptr;
}

const SelectorSupport* jasperengine::htmlui::compat::findSelector(SelectorKind kind, std::string_view name){
    for (const SelectorSupport& row : selectorTable()){
        if (row.kind == kind && equalsIgnoreCase(row.name, name))
            return &row;
    }
    return nullptr;
}

const SelectorSupport* jasperengine::htmlui::compat::findPseudoClass(std::string_view name){
    while (!name.empty() && name.front() == ':')
        name.remove_prefix(1);
    const size_t paren = name.find('(');
    if (paren != std::string_view::npos)
        name = name.substr(0, paren);

    for (const SelectorSupport& row : selectorTable()){
        if (row.kind != SelectorKind::PseudoClass)
            continue;
        std::string_view rowName(row.name);
        while (!rowName.empty() && rowName.front() == ':')
            rowName.remove_prefix(1);
        const size_t rowParen = rowName.find('(');
        if (rowParen != std::string_view::npos)
            rowName = rowName.substr(0, rowParen);
        if (equalsIgnoreCase(rowName, name))
            return &row;
    }
    return nullptr;
}

const SelectorSupport* jasperengine::htmlui::compat::findPseudoElement(std::string_view name){
    while (!name.empty() && name.front() == ':')
        name.remove_prefix(1);
    const size_t paren = name.find('(');
    if (paren != std::string_view::npos)
        name = name.substr(0, paren);

    for (const SelectorSupport& row : selectorTable()){
        if (row.kind != SelectorKind::PseudoElement)
            continue;
        std::string_view rowName(row.name);
        while (!rowName.empty() && rowName.front() == ':')
            rowName.remove_prefix(1);
        const size_t rowParen = rowName.find('(');
        if (rowParen != std::string_view::npos)
            rowName = rowName.substr(0, rowParen);
        if (equalsIgnoreCase(rowName, name))
            return &row;
    }
    return nullptr;
}

const AtRuleSupport* jasperengine::htmlui::compat::findAtRule(std::string_view name){
    if (!name.empty() && name.front() == '@')
        name.remove_prefix(1);
    for (const AtRuleSupport& row : atRuleTable()){
        std::string_view rowName(row.name);
        if (!rowName.empty() && rowName.front() == '@')
            rowName.remove_prefix(1);
        if (equalsIgnoreCase(rowName, name))
            return &row;
    }
    return nullptr;
}

const MediaFeatureSupport* jasperengine::htmlui::compat::findMediaFeature(std::string_view name){
    for (const MediaFeatureSupport& row : mediaFeatureTable()){
        if (equalsIgnoreCase(row.name, name))
            return &row;
    }
    return nullptr;
}

const ElementSupport* jasperengine::htmlui::compat::findElement(std::string_view name){
    for (const ElementSupport& row : elementTable()){
        if (equalsIgnoreCase(row.name, name))
            return &row;
    }
    return nullptr;
}

const AttributeSupport* jasperengine::htmlui::compat::findAttribute(std::string_view element, std::string_view name){
    const AttributeSupport* global = nullptr;
    const AttributeSupport* globalPattern = nullptr;
    const AttributeSupport* ownPattern = nullptr;
    for (const AttributeSupport& row : attributeTable()){
        const bool own = equalsIgnoreCase(row.element, element);
        const bool isGlobal = std::strcmp(row.element, "*") == 0;
        if (!own && !isGlobal)
            continue;
        const bool pattern = std::strchr(row.name, '*') != nullptr;
        if (!matchesPattern(row.name, name))
            continue;
        if (own && !pattern)
            return &row;
        if (own && pattern && !ownPattern)
            ownPattern = &row;
        if (isGlobal && !pattern && !global)
            global = &row;
        if (isGlobal && pattern && !globalPattern)
            globalPattern = &row;
    }
    if (ownPattern)
        return ownPattern;
    if (global)
        return global;
    return globalPattern;
}

const InputTypeSupport* jasperengine::htmlui::compat::findInputType(std::string_view type){
    for (const InputTypeSupport& row : inputTypeTable()){
        if (equalsIgnoreCase(row.type, type))
            return &row;
    }
    return nullptr;
}

const UnitSupport* jasperengine::htmlui::compat::findUnit(std::string_view unit){
    for (const UnitSupport& row : unitTable()){
        if (equalsIgnoreCase(row.unit, unit))
            return &row;
    }
    return nullptr;
}

const FunctionSupport* jasperengine::htmlui::compat::findFunction(std::string_view name){
    for (const FunctionSupport& row : functionTable()){
        if (equalsIgnoreCase(row.name, name))
            return &row;
    }
    return nullptr;
}

Support jasperengine::htmlui::compat::effectiveSupport(Support base, ForkFeature fork, Support withFork, const ForkFeatures& features){
    if (fork != ForkFeature::None && features.has(fork))
        return withFork;
    return base;
}

const char* jasperengine::htmlui::compat::supportName(Support support){
    switch (support){
        case Support::Native: return "native";
        case Support::Rewritten: return "rewritten";
        case Support::Approximated: return "approximated";
        case Support::Emulated: return "emulated";
        case Support::Fallback: return "fallback";
        case Support::Unsupported: return "unsupported";
        case Support::Ignored: return "ignored";
    }
    return "unsupported";
}

const char* jasperengine::htmlui::compat::rewriteIdName(RewriteId id){
    static const char* names[] = {
        "None", "Background", "BackgroundLonghand", "Border", "BorderStyle", "BorderRadius", "BorderImage",
        "Outline", "BoxShadow", "Font", "FontFamily", "FontSize", "FontWeight", "FontStyle", "LineHeight",
        "TextShadow", "TextStroke", "TextFillColor", "TextDecoration", "TextAlign", "Transition",
        "TransitionLonghand", "Animation", "AnimationLonghand", "Display", "Position", "LogicalBox",
        "PlaceAlign", "AlignKeyword", "Overflow", "OverflowWrap", "WhiteSpace", "Filter", "MaskImage",
        "ClipPath", "ListStyle", "ObjectFit", "ImageRendering", "AccentColor", "Appearance",
        "IndividualTransform", "ColorValue", "Cursor", "PointerEvents", "TabIndexLike", "Direction",
        "SvgPaint", "ScrollbarStyle", "Gap", "BorderSpacing", "Visibility", "ZIndex", "Opacity",
        "Sizing", "Flex", "Grid", "Content", "All", "LineClamp", "UserSelect", "TextEffect",
    };
    static_assert(sizeof(names) / sizeof(names[0]) == (size_t)RewriteId::Count, "one name per RewriteId");
    const size_t index = (size_t)id;
    return index < (size_t)RewriteId::Count ? names[index] : "None";
}

const char* jasperengine::htmlui::compat::selectorKindName(SelectorKind kind){
    switch (kind){
        case SelectorKind::Selector: return "selector";
        case SelectorKind::PseudoClass: return "pseudo_class";
        case SelectorKind::PseudoElement: return "pseudo_element";
    }
    return "selector";
}
