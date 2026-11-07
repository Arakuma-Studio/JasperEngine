// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelSettingsCmd.h"

#include "Project.h"

using namespace jasperengine;

editor::ModelSettingsCmd::ModelSettingsCmd(Project* project, const ModelProjectSettings& newSettings, std::string name):
    project(project), oldSettings(project->getModelSettings()), newSettings(newSettings), name(std::move(name)) {
}

bool editor::ModelSettingsCmd::execute() {
    project->setModelSettings(newSettings);
    project->saveProjectFile();
    return true;
}

void editor::ModelSettingsCmd::undo() {
    project->setModelSettings(oldSettings);
    project->saveProjectFile();
}
