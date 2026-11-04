// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DeviceSettingsCmd.h"

#include "Project.h"

using namespace jasperengine;

editor::DeviceSettingsCmd::DeviceSettingsCmd(Project* project, const DeviceSettings& newSettings):
    project(project), oldSettings(project->getDeviceSettings()), newSettings(newSettings){
}

bool editor::DeviceSettingsCmd::execute(){
    project->setDeviceSettings(newSettings);
    // the standalone build's main.cpp bakes them in
    project->markGeneratedSourcesDirty();
    project->saveProjectFile();
    return true;
}

void editor::DeviceSettingsCmd::undo(){
    project->setDeviceSettings(oldSettings);
    project->markGeneratedSourcesDirty();
    project->saveProjectFile();
}
