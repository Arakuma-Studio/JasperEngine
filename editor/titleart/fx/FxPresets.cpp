// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FxPresets.h"

#include "titleart/TitleArtIO.h"
#include "titleart/TitleEffectRegistry.h"

#include "titleart_fx_presets.h"
#include "yaml-cpp/yaml.h"

#include <cstring>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace ta = jasperengine::titleart;

namespace {

    // a YAML node as JSON: quoted scalars stay strings, plain ones become booleans and numbers
    ta::Json toJson(const YAML::Node& node) {
        switch (node.Type()) {
            case YAML::NodeType::Map: {
                ta::Json out = ta::Json::object();
                for (const auto& entry : node) out[entry.first.as<std::string>()] = toJson(entry.second);
                return out;
            }
            case YAML::NodeType::Sequence: {
                ta::Json out = ta::Json::array();
                for (const auto& entry : node) out.push_back(toJson(entry));
                return out;
            }
            case YAML::NodeType::Scalar: {
                const std::string& text = node.Scalar();
                if (node.Tag() == "!") return text;
                if (text == "true") return true;
                if (text == "false") return false;
                if (text == "null" || text == "~") return nullptr;
                char* end = nullptr;
                const long long i = std::strtoll(text.c_str(), &end, 10);
                if (!text.empty() && end && *end == '\0') return (int64_t)i;
                const double d = std::strtod(text.c_str(), &end);
                if (!text.empty() && end && *end == '\0') return d;
                return text;
            }
            default:
                return nullptr;
        }
    }

    std::vector<std::string> strings(const ta::Json& list) {
        std::vector<std::string> out;
        if (!list.is_array()) return out;
        for (const ta::Json& v : list) {
            if (v.is_string()) out.push_back(v.get<std::string>());
        }
        return out;
    }

    // every embedded file parsed, by file name ("light_glow.yaml")
    const std::map<std::string, std::vector<TitleFxPreset>>& files() {
        static const std::map<std::string, std::vector<TitleFxPreset>> parsed = []() {
            std::map<std::string, std::vector<TitleFxPreset>> out;
            for (const TitleArtFxPresetFile* f = titleart_fx_preset_files; f->name; f++) {
                std::vector<TitleFxPreset>& list = out[f->name];
                YAML::Node root;
                try {
                    root = YAML::Load(std::string(reinterpret_cast<const char*>(f->data), f->size));
                } catch (const YAML::Exception&) {
                    continue;
                }
                const ta::Json file = toJson(root);
                if (!file.is_object() || !file.contains("presets") || !file["presets"].is_array()) continue;
                for (const ta::Json& p : file["presets"]) {
                    if (!p.is_object()) continue;
                    TitleFxPreset preset;
                    preset.id = p.value("id", std::string());
                    preset.name = p.value("name", preset.id);
                    preset.aliases = strings(p.value("aliases", ta::Json::array()));
                    preset.source = p.value("source", std::string());
                    preset.costClass = p.value("class", std::string());
                    if (p.contains("params") && p["params"].is_object()) preset.params = p["params"];
                    if (p.contains("layer") && p["layer"].is_object()) preset.layer = p["layer"];
                    list.push_back(std::move(preset));
                }
            }
            return out;
        }();
        return parsed;
    }

    std::string fileOf(std::string_view type) {
        const char* path = nullptr;
        if (const ta::EffectDef* e = ta::findEffect(type)) path = e->presetFile;
        if (const ta::GeneratorDef* g = ta::findGenerator(type)) path = g->presetFile;
        if (!path) return {};
        const std::string p = path;
        const size_t slash = p.find_last_of('/');
        return slash == std::string::npos ? p : p.substr(slash + 1);
    }

    const std::vector<ta::ParamDef>* paramsOf(std::string_view type) {
        if (const ta::EffectDef* e = ta::findEffect(type)) return &e->params;
        if (const ta::GeneratorDef* g = ta::findGenerator(type)) return &g->params;
        return nullptr;
    }

}

const std::vector<TitleFxPreset>& editor::titleFxPresets(std::string_view type) {
    static const std::vector<TitleFxPreset> none;
    const std::string file = fileOf(type);
    if (file.empty()) return none;
    auto it = files().find(file);
    return it == files().end() ? none : it->second;
}

const TitleFxPreset* editor::findTitleFxPreset(std::string_view type, std::string_view id) {
    for (const TitleFxPreset& p : titleFxPresets(type)) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

std::vector<std::string> editor::titleFxPresetIds(std::string_view type) {
    std::vector<std::string> ids;
    for (const TitleFxPreset& p : titleFxPresets(type)) ids.push_back(p.id);
    return ids;
}

std::vector<std::string> editor::checkTitleFxPresets() {
    std::vector<std::string> problems;
    std::vector<std::string> types;
    for (const ta::EffectDef* e : ta::TitleEffectRegistry::get().effects()) types.push_back(e->type);
    for (const ta::GeneratorDef* g : ta::TitleEffectRegistry::get().generators()) types.push_back(g->type);
    for (const std::string& type : types) {
        const std::vector<ta::ParamDef>* defs = paramsOf(type);
        const std::string file = fileOf(type);
        if (file.empty()) continue;
        if (!files().count(file)) {
            problems.push_back(type + ": its preset file " + file + " is missing");
            continue;
        }
        std::set<std::string> ids;
        for (const TitleFxPreset& p : titleFxPresets(type)) {
            const std::string at = type + "/" + p.id;
            if (p.id.empty() || !ids.insert(p.id).second) problems.push_back(at + ": a preset id is empty or used twice");
            if (p.name.empty()) problems.push_back(at + ": no name");
            for (auto it = p.params.begin(); it != p.params.end(); ++it) {
                const ta::ParamDef* def = ta::findParam(*defs, it.key());
                if (!def) {
                    problems.push_back(at + ": unknown param '" + it.key() + "'");
                    continue;
                }
                ta::Json value = it.value();
                std::vector<ta::TitleWarning> warnings;
                ta::NormalizeContext context;
                context.warnings = &warnings;
                if (!ta::normalizeParamValue(*def, value, at + "." + it.key(), context)) {
                    problems.push_back(at + "." + it.key() + ": not a valid " + ta::paramKindName(def->kind));
                } else if (!warnings.empty()) {
                    problems.push_back(at + "." + it.key() + ": " + warnings.front().message);
                }
            }
            if (!p.layer.empty() && !ta::findGenerator(type)) problems.push_back(at + ": an effect preset sets params only");
        }
    }
    return problems;
}
