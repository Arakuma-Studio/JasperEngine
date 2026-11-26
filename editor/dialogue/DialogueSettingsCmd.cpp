// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DialogueSettingsCmd.h"

#include <utility>

using namespace jasperengine;

editor::DialogueSettingsCmd::DialogueSettingsCmd(Project* project, DialogueSettings before, DialogueSettings after, std::string label)
    : project(project), before(std::move(before)), after(std::move(after)), label(std::move(label)) {
}

bool editor::DialogueSettingsCmd::apply(const DialogueSettings& settings){
    project->getDialogueSettings() = settings;
    if (project->getProjectPath().empty())
        return true;
    return project->saveProjectFile();
}

bool editor::DialogueSettingsCmd::execute(){
    return apply(after);
}

void editor::DialogueSettingsCmd::undo(){
    apply(before);
}

std::vector<editor::Command::Touched> editor::DialogueSettingsCmd::touchedPaths(bool) const{
    if (project->getProjectPath().empty())
        return {};
    return {{project->getProjectPath() / "project.yaml", false}};
}
