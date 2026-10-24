// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ThreadingSettingsCmd.h"

#include "Project.h"

using namespace jasperengine;

editor::ThreadingSettingsCmd::ThreadingSettingsCmd(Project* project, const ThreadingProjectSettings& newSettings):
    project(project), oldSettings(project->getThreadingSettings()), newSettings(newSettings){
}

bool editor::ThreadingSettingsCmd::execute(){
    project->setThreadingSettings(newSettings);
    project->saveProjectFile();
    return true;
}

void editor::ThreadingSettingsCmd::undo(){
    project->setThreadingSettings(oldSettings);
    project->saveProjectFile();
}
