// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtOverrides.h"

#include "subsystem/TitleArtRuntime.h"
#include "util/Color.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    PropertyData entryProperty(TitleArtOverride& entry){
        static std::string defaultText;
        static float defaultNumber = 0.0f;
        static Vector4 defaultColor(0.0f, 0.0f, 0.0f, 1.0f);
        if (entry.type == "number" || entry.type == "bool") return {PropertyType::Float, 0, &defaultNumber, &entry.value.x};
        if (entry.type == "color") return {PropertyType::Vector4, 0, &defaultColor, &entry.value};
        return {PropertyType::String, 0, &defaultText, &entry.text};
    }

    bool hexColor(const std::string& text, Vector4& linear){
        std::string hex = text.size() > 0 && text[0] == '#' ? text.substr(1) : std::string();
        if (hex.size() == 3) hex = {hex[0], hex[0], hex[1], hex[1], hex[2], hex[2]};
        if (hex.size() != 6) return false;
        for (char c : hex) if (!std::isxdigit((unsigned char)c)) return false;
        const unsigned long value = std::strtoul(hex.c_str(), nullptr, 16);
        linear = Color::sRGBToLinear(Vector4(((value >> 16) & 0xff) / 255.0f, ((value >> 8) & 0xff) / 255.0f, (value & 0xff) / 255.0f, 1.0f));
        return true;
    }

}

bool editor::splitTitleArtOverrideProperty(const std::string& property, std::string& name){
    const size_t prefix = std::strlen(TITLE_ART_OVERRIDES_PREFIX);
    if (property.size() <= prefix || property.compare(0, prefix, TITLE_ART_OVERRIDES_PREFIX) != 0) return false;
    name = property.substr(prefix);
    return name.find('.') == std::string::npos && name.find('[') == std::string::npos;
}

PropertyData editor::titleArtOverrideProperty(TitleArtRendererComponent& title, const std::string& property){
    std::string name;
    if (!splitTitleArtOverrideProperty(property, name)) return PropertyData();
    TitleArtOverride* entry = findTitleArtOverride(title, name);
    return entry ? entryProperty(*entry) : PropertyData();
}

void editor::listTitleArtOverrideProperties(TitleArtRendererComponent& title, std::map<std::string, PropertyData>& out){
    for (TitleArtOverride& entry : title.overrides)
        if (!entry.name.empty()) out[TITLE_ART_OVERRIDES_PREFIX + entry.name] = entryProperty(entry);
}

TitleArtOverride* editor::findTitleArtOverride(TitleArtRendererComponent& title, const std::string& name){
    for (TitleArtOverride& entry : title.overrides)
        if (entry.name == name) return &entry;
    return nullptr;
}

const TitleArtOverride* editor::findTitleArtOverride(const TitleArtRendererComponent& title, const std::string& name){
    for (const TitleArtOverride& entry : title.overrides)
        if (entry.name == name) return &entry;
    return nullptr;
}

bool editor::removeTitleArtOverride(TitleArtRendererComponent& title, const std::string& name){
    auto it = std::find_if(title.overrides.begin(), title.overrides.end(), [&](const TitleArtOverride& e){ return e.name == name; });
    if (it == title.overrides.end()) return false;
    title.overrides.erase(it);
    title.needUpdate = true;
    return true;
}

void editor::copyTitleArtOverride(const TitleArtRendererComponent& source, TitleArtRendererComponent& target, const std::string& name){
    const TitleArtOverride* from = findTitleArtOverride(source, name);
    if (!from){
        removeTitleArtOverride(target, name);
        return;
    }
    if (TitleArtOverride* to = findTitleArtOverride(target, name)) *to = *from;
    else target.overrides.push_back(*from);
    target.needUpdate = true;
}

mcp::Json editor::titleArtOverrideToJson(const TitleArtOverride& entry){
    if (entry.type == "number") return mcp::Json((double)entry.value.x);
    if (entry.type == "bool") return mcp::Json(entry.value.x != 0.0f);
    if (entry.type == "color"){
        const Vector4 srgb = Color::linearTosRGB(entry.value);
        char hex[8];
        std::snprintf(hex, sizeof(hex), "#%02x%02x%02x", (int)std::lround(std::clamp(srgb.x, 0.0f, 1.0f) * 255.0f),
                      (int)std::lround(std::clamp(srgb.y, 0.0f, 1.0f) * 255.0f), (int)std::lround(std::clamp(srgb.z, 0.0f, 1.0f) * 255.0f));
        return mcp::Json(hex);
    }
    return mcp::Json(entry.text);
}

bool editor::titleArtOverrideFromJson(const mcp::Json& value, const std::string& type, TitleArtOverride& entry, std::string& error){
    entry.type = type;
    entry.value = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    entry.text.clear();
    if (type == "number"){
        if (!value.is_number()){
            error = "'" + entry.name + "' is a number";
            return false;
        }
        entry.value.x = (float)value.get<double>();
        return true;
    }
    if (type == "bool"){
        if (!value.is_boolean() && !value.is_number()){
            error = "'" + entry.name + "' is a boolean";
            return false;
        }
        entry.value.x = value.is_boolean() ? (value.get<bool>() ? 1.0f : 0.0f) : (value.get<double>() != 0.0 ? 1.0f : 0.0f);
        return true;
    }
    if (type == "color"){
        Vector4 linear;
        if (value.is_string() && hexColor(value.get<std::string>(), linear)){
            entry.value = linear;
            return true;
        }
        if (value.is_array() && (value.size() == 3 || value.size() == 4)){
            for (const auto& c : value) if (!c.is_number()) { error = "'" + entry.name + "' is a color: \"#rrggbb\" or [r, g, b] (sRGB 0..1)"; return false; }
            entry.value = Color::sRGBToLinear(Vector4(value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), 1.0f));
            return true;
        }
        error = "'" + entry.name + "' is a color: \"#rrggbb\" or [r, g, b] (sRGB 0..1)";
        return false;
    }
    if (!value.is_string()){
        error = "'" + entry.name + "' is " + (type == "text" ? std::string("a string") : type == "color_scheme" ? std::string("a color scheme id") :
                                              std::string("an asset path (a string)"));
        return false;
    }
    entry.text = value.get<std::string>();
    return true;
}

bool editor::applyTitleArtOverrideEdits(const TitleArtRendererComponent& title, YAML::Node& component,
                                        const std::vector<std::pair<std::string, mcp::Json>>& edits, std::string& error){
    std::vector<titleart::TitleArtExposedParam> exposed;
    std::string readError;
    const bool known = !title.title.path.empty() && titleart::TitleArtRuntime::readExposed(title.title.path, exposed, &readError);
    std::vector<std::string> names;
    for (const titleart::TitleArtExposedParam& p : exposed) names.push_back(p.name);

    YAML::Node list = component["overrides"];
    if (!list || !list.IsSequence()) list = YAML::Node(YAML::NodeType::Sequence);
    for (const auto& [name, value] : edits){
        // the entry already there (kept even when the document no longer exposes it, R4)
        int at = -1;
        for (size_t i = 0; i < list.size(); i++){
            if (list[i]["name"] && list[i]["name"].as<std::string>() == name){
                at = (int)i;
                break;
            }
        }
        if (value.is_null()){
            if (at >= 0){
                YAML::Node rest(YAML::NodeType::Sequence);
                for (size_t i = 0; i < list.size(); i++) if ((int)i != at) rest.push_back(list[i]);
                list = rest;
            }
            continue;
        }
        const titleart::TitleArtExposedParam* param = nullptr;
        for (const titleart::TitleArtExposedParam& p : exposed) if (p.name == name) param = &p;
        if (!param){
            if (!known){
                error = "overrides." + name + ": " + (title.title.path.empty() ? std::string("assign a Title Art first") : readError);
                return false;
            }
            std::string list = names.empty() ? std::string("none") : std::string();
            for (size_t i = 0; i < names.size(); i++) list += (i ? ", " : "") + names[i];
            error = "overrides." + name + ": the title exposes no parameter '" + name + "'. Exposed: " + list +
                    " (expose one in the Title Designer or with edit_title's expose op).";
            return false;
        }
        TitleArtOverride entry;
        entry.name = name;
        if (!titleArtOverrideFromJson(value, param->type, entry, error)){
            error = "overrides." + name + ": " + error;
            return false;
        }
        YAML::Node node(YAML::NodeType::Map);
        node["name"] = entry.name;
        node["type"] = entry.type;
        if (entry.value != Vector4(0.0f, 0.0f, 0.0f, 0.0f)){
            YAML::Node v(YAML::NodeType::Sequence);
            v.push_back(entry.value.x);
            v.push_back(entry.value.y);
            v.push_back(entry.value.z);
            v.push_back(entry.value.w);
            v.SetStyle(YAML::EmitterStyle::Flow);
            node["value"] = v;
        }
        if (!entry.text.empty()) node["text"] = entry.text;
        if (at >= 0) list[at] = node;
        else list.push_back(node);
    }
    component["overrides"] = list;
    return true;
}
