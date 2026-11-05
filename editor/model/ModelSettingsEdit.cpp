// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelSettingsEdit.h"

#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/SetAssetMetaCmd.h"
#include "util/FileUtils.h"

#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // a section as text in its canonical form; no section, a null and an all-default one are one
    std::string canonicalSection(const YAML::Node& section){
        if (!section || section.IsNull())
            return std::string();
        ModelImportSettings parsed;
        std::string error;
        if (!ModelImportSettings::parse(section, parsed, &error))
            return YAML::Dump(section);
        return parsed.isDefault() ? std::string() : YAML::Dump(parsed.serialize());
    }

}

bool editor::editModelSection(const YAML::Node& stored, const ModelSettingsEdit& edit, YAML::Node& out, std::string& error){
    YAML::Node node = stored && stored.IsMap() ? YAML::Clone(stored) : YAML::Node(YAML::NodeType::Map);
    node["version"] = ModelImportSettings::currentVersion;
    YAML::Node target = node;
    if (edit.platform){
        if (!node["platforms"] || !node["platforms"].IsMap())
            node["platforms"] = YAML::Node(YAML::NodeType::Map);
        YAML::Node platforms = node["platforms"];
        const std::string name = modelFamilyName(*edit.platform);
        if (!platforms[name] || !platforms[name].IsMap())
            platforms[name] = YAML::Node(YAML::NodeType::Map);
        // (a node bound once: yaml-cpp's assignment to a bound node overwrites what it holds)
        target.reset(platforms[name]);
        if (!edit.set.empty())
            target["override"] = true;
    }
    for (const auto& [key, value] : edit.set){
        if (value && !value.IsNull()){
            if (!validateModelSettingsField(key, value, error))
                return false;
            target[key] = YAML::Clone(value);
        }else{
            target.remove(key);
        }
    }
    for (const std::string& key : edit.reset)
        target.remove(key);
    ModelImportSettings parsed;
    if (!ModelImportSettings::parse(node, parsed, &error))
        return false;
    // the canonical form; nothing set: no section at all
    out = parsed.isDefault() ? YAML::Node() : parsed.serialize();
    return true;
}

bool editor::writeModelSections(Project& project, const std::vector<std::string>& assetPaths, const std::vector<YAML::Node>& sections,
                                const std::string& label, std::vector<std::string>& changed, std::string& error){
    changed.clear();
    CommandHistory* history = project.getProjectCommandHistory();
    InteractiveEdit edit = history->beginInteractive(label);
    for (size_t i = 0; i < assetPaths.size() && i < sections.size(); i++){
        const std::filesystem::path absolute = project.getAssetsPath() / FileUtils::pathFromUtf8(assetPaths[i]);
        // what the sidecar holds already is no change and no undo step (a command only knows its
        // old value once it has run)
        if (canonicalSection(SetAssetMetaCmd::read(absolute, ModelImportSettings::metaKey)) == canonicalSection(sections[i]))
            continue;
        Command* command = new SetAssetMetaCmd(&project, project.normalizeToProjectRelative(absolute), ModelImportSettings::metaKey, sections[i]);
        command->setNoMerge();
        if (!history->addCommand(command)){
            edit.cancel();
            changed.clear();
            error = "couldn't write the import settings of " + assetPaths[i];
            return false;
        }
        changed.push_back(assetPaths[i]);
    }
    edit.commit();
    return true;
}
