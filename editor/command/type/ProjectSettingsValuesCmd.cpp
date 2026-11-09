// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProjectSettingsValuesCmd.h"

#include "Project.h"
#include "settings/SettingsRegistry.h"

using namespace jasperengine;

editor::ProjectSettingsValuesCmd::ProjectSettingsValuesCmd(Project* project, std::vector<std::pair<std::string, mcp::Json>> values, std::string name):
    project(project), newValues(std::move(values)), name(std::move(name)){
    SettingsContext context;
    context.projectPtr = project;
    for (const auto& [key, value] : newValues){
        const SettingRow* row = SettingsRegistry::get().find(key);
        oldValues.push_back({key, row && row->get ? row->get(context) : mcp::Json(nullptr)});
    }
}

bool editor::ProjectSettingsValuesCmd::apply(const std::vector<std::pair<std::string, mcp::Json>>& values, std::string& why){
    SettingsContext context;
    context.projectPtr = project;
    // every value is checked before any applies: a refused one changes nothing
    for (const auto& [key, value] : values){
        const SettingRow* row = SettingsRegistry::get().find(key);
        if (!row || !row->set){
            why = key + ": not a project setting";
            return false;
        }
        if (!row->set(context, value, false, why)){
            why = key + ": " + why;
            return false;
        }
    }
    for (const auto& [key, value] : values){
        std::string ignored;
        SettingsRegistry::get().find(key)->set(context, value, true, ignored);
    }
    project->markGeneratedSourcesDirty();
    project->saveProjectFile();
    return true;
}

bool editor::ProjectSettingsValuesCmd::execute(){
    error.clear();
    return apply(newValues, error);
}

void editor::ProjectSettingsValuesCmd::undo(){
    // in reverse, so a value that depended on an earlier one is restored first
    std::vector<std::pair<std::string, mcp::Json>> reversed(oldValues.rbegin(), oldValues.rend());
    std::string why;
    apply(reversed, why);
}
