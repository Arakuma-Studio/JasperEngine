// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SaveGameSettingsCmd.h"

#include "Project.h"

using namespace jasperengine;

editor::SaveGameSettingsCmd::SaveGameSettingsCmd(Project* project, const SaveGameSettings& newSettings):
    project(project), oldSettings(project->getSaveGameSettings()), newSettings(newSettings){
}

bool editor::SaveGameSettingsCmd::execute(){
    project->setSaveGameSettings(newSettings);
    project->saveProjectFile();
    return true;
}

void editor::SaveGameSettingsCmd::undo(){
    project->setSaveGameSettings(oldSettings);
    project->saveProjectFile();
}
