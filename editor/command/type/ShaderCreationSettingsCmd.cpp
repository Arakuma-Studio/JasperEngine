// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderCreationSettingsCmd.h"

#include "Project.h"

using namespace jasperengine;

editor::ShaderCreationSettingsCmd::ShaderCreationSettingsCmd(Project* project, const ShaderCreationProjectSettings& newSettings):
    project(project), oldSettings(project->getShaderCreationSettings()), newSettings(newSettings){
}

bool editor::ShaderCreationSettingsCmd::execute(){
    project->setShaderCreationSettings(newSettings);
    project->saveProjectFile();
    return true;
}

void editor::ShaderCreationSettingsCmd::undo(){
    project->setShaderCreationSettings(oldSettings);
    project->saveProjectFile();
}
