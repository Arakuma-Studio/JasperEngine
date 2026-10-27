// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShortcutOverrides.h"

#include "AppSettings.h"
#include "Out.h"
#include "util/UserFiles.h"

#include "yaml-cpp/yaml.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::string commandTitle(const CommandCatalog& catalog, const std::string& id) {
        const CommandDef* def = catalog.find(id);
        return def ? def->group + " / " + def->name : id;
    }

}

std::filesystem::path ShortcutOverrides::defaultFile() {
    return AppSettings::getConfigDirectory() / "shortcuts.yaml";
}

bool ShortcutOverrides::parse(const std::string& text, std::map<std::string, std::vector<std::string>>& overrides, std::string* error) {
    overrides.clear();
    YAML::Node root;
    try {
        root = YAML::Load(text);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
    if (root.IsNull()) return true;
    if (!root.IsMap()) {
        if (error) *error = "not a map";
        return false;
    }
    const YAML::Node map = root["overrides"];
    if (!map) return true;
    if (!map.IsMap()) {
        if (error) *error = "overrides is not a map";
        return false;
    }
    for (const auto& entry : map) {
        const std::string id = entry.first.as<std::string>("");
        if (id.empty()) continue;
        std::vector<std::string>& chords = overrides[id];
        if (entry.second.IsSequence()) {
            for (const auto& chord : entry.second) chords.push_back(chord.as<std::string>(""));
        } else if (entry.second.IsScalar()) {
            chords.push_back(entry.second.as<std::string>(""));
        }
    }
    return true;
}

std::string ShortcutOverrides::serialize(const CommandCatalog& catalog) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "shortcutsVersion" << YAML::Value << currentVersion;
    out << YAML::Key << "overrides" << YAML::Value << YAML::BeginMap;
    for (const auto& [id, entry] : catalog.storedOverrides()) {
        out << YAML::Key << id << YAML::Value << YAML::Flow << YAML::BeginSeq;
        for (const std::string& chord : entry.chords) out << YAML::DoubleQuoted << chord;
        out << YAML::EndSeq;
    }
    out << YAML::EndMap;
    out << YAML::EndMap;
    return std::string(out.c_str()) + "\n";
}

void ShortcutOverrides::load(const std::filesystem::path& file, CommandCatalog& catalog) {
    std::string text;
    std::map<std::string, std::vector<std::string>> overrides;
    if (userfiles::read(file, text)) {
        std::string error;
        if (!parse(text, overrides, &error)) {
            Out::warning("Shortcuts: %s could not be read (%s); the default shortcuts apply. Edit > Shortcuts... rewrites it.",
                         file.generic_string().c_str(), error.c_str());
            overrides.clear();
        }
    }
    const std::vector<CommandCatalog::Rejected> rejected = catalog.loadOverrides(overrides);
    if (!rejected.empty()) {
        std::string list;
        for (const CommandCatalog::Rejected& entry : rejected) {
            if (!list.empty()) list += ", ";
            list += entry.id + " (" + entry.reason + ")";
        }
        Out::warning("Shortcuts: %s has entries this editor cannot use, kept in the file and ignored: %s.",
                     file.generic_string().c_str(), list.c_str());
    }
    const std::vector<CommandCatalog::ResolvedConflict> conflicts = catalog.resolveConflicts();
    if (!conflicts.empty()) {
        std::string list;
        for (const CommandCatalog::ResolvedConflict& conflict : conflicts) {
            if (!list.empty()) list += "; ";
            list += conflict.chord.format(catalog.isMac()) + " is bound to " + commandTitle(catalog, conflict.winner) + " and " +
                    commandTitle(catalog, conflict.loser) + ": " + commandTitle(catalog, conflict.winner) + " keeps it";
        }
        Out::warning("Shortcuts: %s. Fix the bindings in Edit > Shortcuts...", list.c_str());
    }
}

bool ShortcutOverrides::save(const std::filesystem::path& file, const CommandCatalog& catalog, std::string* error) {
    return userfiles::write(file, serialize(catalog), error);
}
