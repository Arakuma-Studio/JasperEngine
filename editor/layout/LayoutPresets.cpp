// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LayoutPresets.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace jasperengine::editor;

namespace {

    std::string lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::string trimmed(const std::string& text) {
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) begin++;
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
        return text.substr(begin, end - begin);
    }

    void emitPreset(YAML::Emitter& out, const LayoutPreset& preset, bool working, bool versioned) {
        out << YAML::BeginMap;
        if (versioned) out << YAML::Key << "layoutVersion" << YAML::Value << LayoutPresets::currentVersion;
        out << YAML::Key << "name" << YAML::Value << preset.name;
        if (working) out << YAML::Key << "activePreset" << YAML::Value << preset.activePreset;
        out << YAML::Key << "uiScale" << YAML::Value << preset.uiScale;
        out << YAML::Key << "panels" << YAML::Value << YAML::Flow << YAML::BeginMap;
        for (const auto& [panel, open] : preset.panels) out << YAML::Key << panel << YAML::Value << open;
        out << YAML::EndMap;
        out << YAML::Key << "ini" << YAML::Value << YAML::Literal << preset.ini;
        out << YAML::EndMap;
    }

    bool readPreset(const YAML::Node& node, LayoutPreset& preset, std::string* error) {
        if (!node.IsMap()) {
            if (error) *error = "a layout is a map";
            return false;
        }
        if (const YAML::Node version = node["layoutVersion"]; version && version.as<int>(0) > LayoutPresets::currentVersion) {
            if (error) *error = "layoutVersion " + version.as<std::string>("?") + " is newer than this editor's";
            return false;
        }
        const YAML::Node ini = node["ini"];
        if (!ini || !ini.IsScalar()) {
            if (error) *error = "no ini";
            return false;
        }
        preset.name = node["name"] ? node["name"].as<std::string>("") : std::string();
        preset.activePreset = node["activePreset"] ? node["activePreset"].as<std::string>("") : std::string();
        preset.uiScale = node["uiScale"] ? node["uiScale"].as<float>(1.0f) : 1.0f;
        if (!std::isfinite(preset.uiScale) || preset.uiScale < 0.5f || preset.uiScale > 8.0f) preset.uiScale = 1.0f;
        preset.panels.clear();
        if (const YAML::Node panels = node["panels"]; panels && panels.IsMap()) {
            for (const auto& entry : panels) preset.panels[entry.first.as<std::string>("")] = entry.second.as<bool>(false);
        }
        preset.panels.erase("");
        preset.ini = LayoutPresets::filterDocumentWindows(ini.as<std::string>(""));
        return true;
    }

}

std::string LayoutPresets::filterDocumentWindows(const std::string& ini) {
    std::string out;
    out.reserve(ini.size());
    bool skipping = false;
    size_t begin = 0;
    while (begin < ini.size()) {
        size_t end = ini.find('\n', begin);
        if (end == std::string::npos) end = ini.size();
        std::string line = ini.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        begin = end + 1;
        if (!line.empty() && line.front() == '[') {
            // a section header: [Window][###Scene3]
            skipping = line.rfind("[Window][###", 0) == 0;
        }
        if (skipping) continue;
        out += line;
        out += '\n';
    }
    // one trailing blank line at most
    while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') out.pop_back();
    return out;
}

std::string LayoutPresets::uniqueName(const std::string& name, const std::vector<std::string>& existing) {
    auto taken = [&existing](const std::string& candidate) {
        const std::string key = lower(candidate);
        return std::any_of(existing.begin(), existing.end(), [&key](const std::string& other) { return lower(other) == key; });
    };
    const std::string base = trimmed(name).empty() ? std::string("Layout") : trimmed(name);
    if (!taken(base)) return base;
    for (int n = 2;; n++) {
        const std::string candidate = base + " (" + std::to_string(n) + ")";
        if (!taken(candidate)) return candidate;
    }
}

std::string LayoutPresets::slug(const std::string& name) {
    std::string out;
    bool dash = false;
    for (unsigned char c : name) {
        if (std::isalnum(c)) {
            if (dash && !out.empty()) out.push_back('-');
            dash = false;
            out.push_back(static_cast<char>(std::tolower(c)));
        } else {
            dash = true;
        }
    }
    return out.empty() ? std::string("layout") : out;
}

bool LayoutPresets::isValidName(const std::string& name, std::string* why) {
    const std::string text = trimmed(name);
    if (text.empty() || text.size() > 64) {
        if (why) *why = "A layout name has 1 to 64 characters.";
        return false;
    }
    if (std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20; })) {
        if (why) *why = "A layout name has no control characters.";
        return false;
    }
    if (lower(text) == lower(DEFAULT_NAME)) {
        if (why) *why = "Default is the built-in layout.";
        return false;
    }
    return true;
}

std::string LayoutPresets::serialize(const LayoutPreset& preset, bool working) {
    YAML::Emitter out;
    emitPreset(out, preset, working, true);
    return std::string(out.c_str()) + "\n";
}

bool LayoutPresets::parse(const std::string& text, LayoutPreset& preset, std::string* error) {
    try {
        return readPreset(YAML::Load(text), preset, error);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

std::string LayoutPresets::serializeBundle(const std::vector<LayoutPreset>& presets) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "layoutBundleVersion" << YAML::Value << bundleVersion;
    out << YAML::Key << "presets" << YAML::Value << YAML::BeginSeq;
    for (const LayoutPreset& preset : presets) emitPreset(out, preset, false, false);
    out << YAML::EndSeq;
    out << YAML::EndMap;
    return std::string(out.c_str()) + "\n";
}

bool LayoutPresets::parseFile(const std::string& text, std::vector<LayoutPreset>& presets, std::string* error) {
    presets.clear();
    YAML::Node root;
    try {
        root = YAML::Load(text);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
    if (root.IsMap() && root["layoutBundleVersion"]) {
        if (root["layoutBundleVersion"].as<int>(0) > bundleVersion) {
            if (error) *error = "layoutBundleVersion " + root["layoutBundleVersion"].as<std::string>("?") + " is newer than this editor's";
            return false;
        }
        const YAML::Node list = root["presets"];
        if (!list || !list.IsSequence()) {
            if (error) *error = "the bundle has no presets list";
            return false;
        }
        for (const auto& node : list) {
            LayoutPreset preset;
            std::string why;
            if (!readPreset(node, preset, &why)) {
                if (error) *error = "preset " + std::to_string(presets.size() + 1) + ": " + why;
                return false;
            }
            presets.push_back(std::move(preset));
        }
        return true;
    }
    LayoutPreset preset;
    if (!readPreset(root, preset, error)) return false;
    presets.push_back(std::move(preset));
    return true;
}
