// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AndroidSettingsCmd.h"

using namespace jasperengine;

editor::AndroidSettingsCmd::AndroidSettingsCmd(Project* project, const AndroidProjectSettings& newSettings):
    project(project), oldSettings(project->getAndroidProjectSettings()), newSettings(newSettings){
}

bool editor::AndroidSettingsCmd::execute(){
    project->getAndroidProjectSettings() = newSettings;
    project->saveProjectFile();
    return true;
}

void editor::AndroidSettingsCmd::undo(){
    project->getAndroidProjectSettings() = oldSettings;
    project->saveProjectFile();
}
