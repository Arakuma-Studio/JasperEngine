// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DrawSubmissionSettingsCmd.h"

#include "Project.h"

using namespace jasperengine;

editor::DrawSubmissionSettingsCmd::DrawSubmissionSettingsCmd(Project* project, const DrawSubmissionSettings& newSettings):
    project(project), oldSettings(project->getDrawSubmissionSettings()), newSettings(newSettings){
}

bool editor::DrawSubmissionSettingsCmd::execute(){
    project->setDrawSubmissionSettings(newSettings);
    project->saveProjectFile();
    return true;
}

void editor::DrawSubmissionSettingsCmd::undo(){
    project->setDrawSubmissionSettings(oldSettings);
    project->saveProjectFile();
}
