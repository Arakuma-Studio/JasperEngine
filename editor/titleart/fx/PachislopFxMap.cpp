// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PachislopFxMap.h"

#include "title_pachislop_fx_map.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cctype>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

namespace {

    Json scalarJson(const YAML::Node& node) {
        const std::string text = node.Scalar();
        if (node.Tag() == "!") return text;
        if (text == "true") return true;
        if (text == "false") return false;
        char* end = nullptr;
        const double d = std::strtod(text.c_str(), &end);
        if (end && *end == '\0' && !text.empty()) return d;
        return text;
    }

    // camelCase -> snake_case (Pachislop's option values: softLight -> soft_light)
    std::string snake(const std::string& s) {
        std::string out;
        for (char c : s) {
            if (std::isupper((unsigned char)c)) {
                out += '_';
                out += (char)std::tolower((unsigned char)c);
            } else {
                out += c;
            }
        }
        return out;
    }

    struct Store {
        std::once_flag once;
        std::map<std::string, PachislopFxEntry> effects;
    };

    Store& store() {
        static Store* s = new Store();
        std::call_once(s->once, [] {
            YAML::Node root;
            try {
                root = YAML::Load(std::string(reinterpret_cast<const char*>(title_pachislop_fx_map)));
            } catch (const YAML::Exception&) {
                return;     // PachislopFxMapTest reports a malformed file
            }
            if (!root["effects"]) return;
            for (const auto& kv : root["effects"]) {
                PachislopFxEntry e;
                e.type = kv.second["type"].as<std::string>();
                for (const auto& p : kv.second["params"]) e.params[p.first.as<std::string>()] = p.second.as<std::string>();
                if (kv.second["vec2"]) {
                    for (const auto& v : kv.second["vec2"]) {
                        for (const auto& k : v.second) e.vec2[v.first.as<std::string>()].push_back(k.as<std::string>());
                    }
                }
                if (kv.second["negate"]) {
                    for (const auto& n : kv.second["negate"]) e.negate.push_back(n.as<std::string>());
                }
                if (kv.second["set"]) {
                    for (const auto& p : kv.second["set"]) e.set[p.first.as<std::string>()] = scalarJson(p.second);
                }
                s->effects[kv.first.as<std::string>()] = std::move(e);
            }
        });
        return *s;
    }

}

const std::map<std::string, PachislopFxEntry>& editor::pachislopEffectMap() {
    return store().effects;
}

PachislopFxConversion editor::convertPachislopEffect(const std::string& pachislopType, const Json& params) {
    PachislopFxConversion out;
    const auto& map = pachislopEffectMap();
    auto it = map.find(pachislopType);
    if (it == map.end()) {
        out.error = "'" + pachislopType + "' is not a Pachislop effect type the map knows";
        return out;
    }
    const PachislopFxEntry& e = it->second;
    out.type = e.type;
    out.params = e.set;
    for (auto p = params.begin(); p != params.end(); ++p) {
        const std::string& key = p.key();
        bool inVec2 = false;
        for (const auto& [ours, xy] : e.vec2) {
            for (size_t i = 0; i < xy.size() && i < 2; i++) {
                if (xy[i] != key) continue;
                if (!out.params.contains(ours)) out.params[ours] = Json::array({0.0, 0.0});
                out.params[ours][i] = p.value();
                inVec2 = true;
            }
        }
        if (inVec2) continue;
        auto m = e.params.find(key);
        if (m == e.params.end()) {
            out.error = "'" + key + "' is not a param of Pachislop's " + pachislopType;
            return out;
        }
        Json value = p.value();
        if (std::find(e.negate.begin(), e.negate.end(), key) != e.negate.end() && value.is_number()) value = -value.get<double>();
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            const bool word = !text.empty() && std::isalpha((unsigned char)text[0]) &&
                              std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isalnum(c) != 0; });
            if (word) value = snake(text);
        }
        out.params[m->second] = value;
    }
    out.ok = true;
    return out;
}
