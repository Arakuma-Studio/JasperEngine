// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/CustomPropertyIndex.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/Units.h"

using namespace jasperengine::htmlui::compat;

namespace {

    constexpr int kMaxDepth = 8;
    constexpr size_t kMaxAlternatives = 32;

    bool containsVar(const std::vector<ComponentValue>& values){
        for (const ComponentValue& v : values){
            if (v.kind == ComponentValue::Kind::Function && equalsNoCase(v.name(), "var"))
                return true;
            if (v.kind != ComponentValue::Kind::Token && containsVar(v.children))
                return true;
        }
        return false;
    }

}

void CustomPropertyIndex::clear(){
    entries.clear();
    uses.clear();
    rawUses.clear();
}

void CustomPropertyIndex::add(const std::string& name, Entry entry){
    entries[name].push_back(std::move(entry));
}

const std::vector<CustomPropertyIndex::Entry>* CustomPropertyIndex::find(const std::string& name) const{
    const auto it = entries.find(name);
    return it == entries.end() ? nullptr : &it->second;
}

std::string CustomPropertyIndex::varName(const ComponentValue& var, std::vector<ComponentValue>* fallback, bool* hasFallback){
    if (hasFallback)
        *hasFallback = false;
    if (var.kind != ComponentValue::Kind::Function || !equalsNoCase(var.name(), "var"))
        return std::string();
    std::string name;
    bool afterComma = false;
    for (const ComponentValue& child : var.children){
        if (afterComma){
            if (fallback)
                fallback->push_back(child);
            continue;
        }
        if (child.isWhitespace() || child.isToken(TokenType::Comment))
            continue;
        if (child.isToken(TokenType::Comma)){
            afterComma = true;
            if (hasFallback)
                *hasFallback = true;
            continue;
        }
        if (child.isToken(TokenType::Ident) && name.empty() && child.token.value.size() > 2 && child.token.value.compare(0, 2, "--") == 0)
            name = child.token.value;
        else
            return std::string();
    }
    if (fallback)
        *fallback = trimWhitespace(*fallback);
    return name;
}

void CustomPropertyIndex::referencedVariables(const std::vector<ComponentValue>& value, std::vector<std::string>& names){
    for (const ComponentValue& v : value){
        if (v.kind == ComponentValue::Kind::Function && equalsNoCase(v.name(), "var")){
            const std::string name = varName(v);
            if (!name.empty())
                names.push_back(name);
        }
        if (v.kind != ComponentValue::Kind::Token)
            referencedVariables(v.children, names);
    }
}

namespace {

    // replaces every var() of value by its static replacement; false when one has none
    bool substitute(const CustomPropertyIndex& index, const std::vector<ComponentValue>& value, std::vector<ComponentValue>& out, int depth){
        if (depth > kMaxDepth)
            return false;
        for (const ComponentValue& v : value){
            if (v.kind == ComponentValue::Kind::Function && equalsNoCase(v.name(), "var")){
                std::vector<ComponentValue> replacement;
                if (!index.resolve(v, replacement))
                    return false;
                if (!substitute(index, replacement, out, depth + 1))
                    return false;
                continue;
            }
            if (v.kind != ComponentValue::Kind::Token && containsVar(v.children)){
                ComponentValue copy = v;
                copy.children.clear();
                if (!substitute(index, v.children, copy.children, depth + 1))
                    return false;
                out.push_back(std::move(copy));
                continue;
            }
            out.push_back(v);
        }
        return true;
    }

}

std::optional<std::vector<ComponentValue>> CustomPropertyIndex::staticValue(const std::string& name) const{
    const std::vector<Entry>* list = find(name);
    if (!list || list->empty())
        return std::nullopt;
    std::optional<std::string> text;
    std::vector<ComponentValue> value;
    for (const Entry& entry : *list){
        std::vector<ComponentValue> resolved;
        thread_local int depth = 0;
        if (depth > kMaxDepth)
            return std::nullopt;
        depth++;
        const bool ok = substitute(*this, entry.value, resolved, 0);
        depth--;
        if (!ok)
            return std::nullopt;
        const std::string serialized = serialize(trimWhitespace(resolved));
        if (text && *text != serialized)
            return std::nullopt;
        if (!text){
            text = serialized;
            value = trimWhitespace(resolved);
        }
    }
    return value;
}

bool CustomPropertyIndex::resolve(const ComponentValue& var, std::vector<ComponentValue>& out) const{
    std::vector<ComponentValue> fallback;
    bool hasFallback = false;
    const std::string name = varName(var, &fallback, &hasFallback);
    if (name.empty())
        return false;
    if (std::optional<std::vector<ComponentValue>> value = staticValue(name)){
        out = std::move(*value);
        return true;
    }
    if (!declared(name) && hasFallback){
        out = fallback;
        return true;
    }
    return false;
}

bool CustomPropertyIndex::expandedValues(const std::string& name, std::vector<std::vector<ComponentValue>>& out) const{
    thread_local int depth = 0;
    const std::vector<Entry>* list = find(name);
    if (!list || depth > kMaxDepth)
        return false;
    depth++;
    bool ok = true;
    for (const Entry& entry : *list){
        // expand nested vars into every combination (bounded)
        std::vector<std::vector<ComponentValue>> partial{{}};
        for (const ComponentValue& v : entry.value){
            if (v.kind == ComponentValue::Kind::Function && equalsNoCase(v.name(), "var")){
                std::vector<ComponentValue> fallback;
                bool hasFallback = false;
                const std::string nested = varName(v, &fallback, &hasFallback);
                std::vector<std::vector<ComponentValue>> options;
                if (nested.empty() || (!expandedValues(nested, options) && !(hasFallback && !declared(nested)))){
                    ok = false;
                    break;
                }
                if (options.empty())
                    options.push_back(fallback);
                std::vector<std::vector<ComponentValue>> next;
                for (const std::vector<ComponentValue>& prefix : partial){
                    for (const std::vector<ComponentValue>& option : options){
                        std::vector<ComponentValue> combined = prefix;
                        combined.insert(combined.end(), option.begin(), option.end());
                        next.push_back(std::move(combined));
                        if (next.size() > kMaxAlternatives)
                            break;
                    }
                }
                partial = std::move(next);
                continue;
            }
            for (std::vector<ComponentValue>& prefix : partial)
                prefix.push_back(v);
        }
        if (!ok)
            break;
        for (std::vector<ComponentValue>& p : partial)
            out.push_back(trimWhitespace(p));
        if (out.size() > kMaxAlternatives)
            break;
    }
    depth--;
    return ok;
}

void CustomPropertyIndex::addUse(const std::string& name, const std::string& family){
    uses[name].insert(family);
}

void CustomPropertyIndex::addRawUse(const std::string& name){
    rawUses.insert(name);
}

std::set<std::string> CustomPropertyIndex::families(const std::string& name) const{
    const auto it = uses.find(name);
    return it == uses.end() ? std::set<std::string>() : it->second;
}

bool CustomPropertyIndex::inPlace(const std::string& name) const{
    const auto it = uses.find(name);
    return it != uses.end() && it->second.size() == 1 && rawUses.count(name) == 0;
}

std::string CustomPropertyIndex::typedName(const std::string& name, const std::string& family) const{
    if (inPlace(name))
        return name;
    return name + "--" + family;
}
