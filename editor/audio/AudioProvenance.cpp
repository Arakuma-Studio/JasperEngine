// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioProvenance.h"

#include "asset/AssetMeta.h"
#include "command/type/SetAssetMetaCmd.h"
#include "mcp/McpValues.h"
#include "util/AssetDatabase.h"

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

bool isObjectKey(const std::string& key) {
    return key == "builtin" || key == "generator" || key == "reference";
}

} // namespace

const std::vector<std::string>& AudioProvenance::keyNames() {
    static const std::vector<std::string> names = {"licence", "licence_url", "author", "attribution", "source", "url", "builtin", "generator", "reference"};
    return names;
}

std::string AudioProvenance::text(const char* key) const {
    const auto it = values.find(key);
    return it != values.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool AudioProvenance::parse(const YAML::Node& section, AudioProvenance& out, std::string& error) {
    out = AudioProvenance();
    if (!section || section.IsNull()) return true;
    if (!section.IsMap()) {
        error = "provenance is not a mapping";
        return false;
    }
    out.values = mcp::McpValues::yamlToJson(section);
    out.values.erase("version");
    return true;
}

bool AudioProvenance::load(const std::filesystem::path& asset, AudioProvenance& out, std::string& error) {
    return parse(SetAssetMetaCmd::read(asset, metaKey), out, error);
}

YAML::Node AudioProvenance::toYaml() const {
    if (isEmpty()) return YAML::Node(YAML::NodeType::Null);
    YAML::Node node(YAML::NodeType::Map);
    node["version"] = currentVersion;
    for (const std::string& key : keyNames()) {
        if (values.contains(key)) node[key] = mcp::McpValues::jsonToYaml(values[key]);
    }
    for (auto it = values.begin(); it != values.end(); ++it) {
        const std::vector<std::string>& names = keyNames();
        if (std::find(names.begin(), names.end(), it.key()) == names.end()) node[it.key()] = mcp::McpValues::jsonToYaml(it.value());
    }
    return node;
}

bool AudioProvenance::apply(const Json& keys, std::string& error) {
    if (!keys.is_object()) {
        error = "provenance must be an object";
        return false;
    }
    Json next = values;
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        const std::string& key = it.key();
        const std::vector<std::string>& names = keyNames();
        if (std::find(names.begin(), names.end(), key) == names.end()) {
            std::string list;
            for (const std::string& name : names) list += (list.empty() ? "" : ", ") + name;
            error = "unknown provenance key `" + key + "`: the keys are " + list;
            return false;
        }
        if (it.value().is_null()) {
            next.erase(key);
        } else if (isObjectKey(key) ? !it.value().is_object() : !it.value().is_string()) {
            error = key + (isObjectKey(key) ? " must be an object" : " must be a string") + std::string(" (null removes it)");
            return false;
        } else {
            next[key] = it.value();
        }
    }
    values = std::move(next);
    return true;
}

AudioProvenance::Json AudioProvenance::licenceJson() const {
    Json out = Json::object();
    out["id"] = licence().empty() ? std::string(LICENCE_UNKNOWN) : licence();
    for (const char* key : {"source", "author", "url", "attribution", "licence_url"}) {
        const std::string value = text(key);
        if (!value.empty()) out[key] = value;
    }
    return out;
}

bool jasperengine::editor::clipMetaText(const std::filesystem::path& asset, const YAML::Node& sections, std::string& text, std::string& error) {
    AssetMeta meta;
    std::ifstream in(SetAssetMetaCmd::metaPathOf(asset), std::ios::binary);
    if (in) {
        std::ostringstream current;
        current << in.rdbuf();
        if (!AssetMeta::parse(current.str(), meta, &error)) {
            error = SetAssetMetaCmd::metaPathOf(asset).filename().string() + " does not read: " + error;
            return false;
        }
    } else {
        meta.guid = Guid::parse(AssetDatabase::generateGuid());
    }
    for (const auto& entry : sections) {
        const std::string name = entry.first.as<std::string>("");
        if (entry.second.IsSequence() && entry.second.size() == 0) continue;
        YAML::Node wrapped(YAML::NodeType::Map);
        wrapped[name] = entry.second;
        YAML::Emitter out;
        out << wrapped;
        const std::string emitted = out.c_str();
        meta.setSection(name, emitted.substr(name.size() + 1));
    }
    text = meta.serialize();
    return true;
}
