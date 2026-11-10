// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FxPresets.h"

#include "title_fx_presets.h"

#include "yaml-cpp/yaml.h"

#include <cstdlib>
#include <map>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

namespace {

    // a YAML node as JSON: scalars become booleans, integers, numbers or strings
    Json toJson(const YAML::Node& node) {
        switch (node.Type()) {
            case YAML::NodeType::Map: {
                Json out = Json::object();
                for (const auto& kv : node) out[kv.first.as<std::string>()] = toJson(kv.second);
                return out;
            }
            case YAML::NodeType::Sequence: {
                Json out = Json::array();
                for (const auto& v : node) out.push_back(toJson(v));
                return out;
            }
            case YAML::NodeType::Scalar: {
                const std::string text = node.Scalar();
                if (node.Tag() == "!") return text;     // quoted: always a string
                if (text == "true") return true;
                if (text == "false") return false;
                if (text == "null" || text == "~") return nullptr;
                char* end = nullptr;
                const long long i = std::strtoll(text.c_str(), &end, 10);
                if (end && *end == '\0' && !text.empty()) return (int64_t)i;
                const double d = std::strtod(text.c_str(), &end);
                if (end && *end == '\0' && !text.empty()) return d;
                return text;
            }
            default:
                return nullptr;
        }
    }

    struct Store {
        std::once_flag once;
        std::map<std::string, std::vector<TitleFxPreset>> byType;
    };

    Store& store() {
        static Store* s = new Store();
        std::call_once(s->once, [] {
            for (const TitleFxPresetEmbeddedFile& file : title_fx_preset_embedded_files) {
                if (!file.name) continue;
                std::string type = file.name;
                const size_t dot = type.rfind('.');
                if (dot != std::string::npos) type = type.substr(0, dot);
                std::vector<TitleFxPreset> presets;
                std::string error;
                // a malformed file is TitleFxPresetsTest's to report; the editor offers its type none
                if (TitleFxPresets::parse(std::string(reinterpret_cast<const char*>(file.data), file.size), presets, error)) {
                    s->byType[type] = std::move(presets);
                }
            }
        });
        return *s;
    }

}

bool TitleFxPresets::parse(const std::string& text, std::vector<TitleFxPreset>& out, std::string& error) {
    out.clear();
    YAML::Node root;
    try {
        root = YAML::Load(text);
    } catch (const YAML::Exception& e) {
        error = e.what();
        return false;
    }
    if (!root.IsMap() || !root["presets"] || !root["presets"].IsSequence()) {
        error = "a preset file is {presets: [...]}";
        return false;
    }
    for (const YAML::Node& node : root["presets"]) {
        if (!node.IsMap() || !node["id"] || !node["name"]) {
            error = "a preset needs an id and a name";
            return false;
        }
        TitleFxPreset p;
        p.id = node["id"].as<std::string>();
        p.name = node["name"].as<std::string>();
        if (node["aliases"]) {
            for (const YAML::Node& a : node["aliases"]) p.aliases.push_back(a.as<std::string>());
        }
        if (node["params"]) {
            p.params = toJson(node["params"]);
            if (!p.params.is_object()) {
                error = p.id + ": params is a map";
                return false;
            }
        }
        if (node["cost"]) p.cost = node["cost"].as<std::string>();
        if (node["layer"]) p.layer = toJson(node["layer"]);
        out.push_back(std::move(p));
    }
    return true;
}

const std::vector<TitleFxPreset>& TitleFxPresets::forType(const std::string& type) {
    static const std::vector<TitleFxPreset> none;
    const Store& s = store();
    auto it = s.byType.find(type);
    return it != s.byType.end() ? it->second : none;
}

const TitleFxPreset* TitleFxPresets::find(const std::string& type, const std::string& id) {
    for (const TitleFxPreset& p : forType(type)) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

std::vector<std::string> TitleFxPresets::types() {
    std::vector<std::string> out;
    for (const auto& kv : store().byType) out.push_back(kv.first);
    return out;
}
