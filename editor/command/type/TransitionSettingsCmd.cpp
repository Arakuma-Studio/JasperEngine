// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TransitionSettingsCmd.h"

#include "Project.h"

using namespace jasperengine;

editor::TransitionSettingsCmd::TransitionSettingsCmd(Project* project, const jasperengine::pixelart::TransitionDefaults& newSettings):
    project(project), oldSettings(project->getTransitionDefaults()), newSettings(newSettings){
}

bool editor::TransitionSettingsCmd::execute(){
    project->setTransitionDefaults(newSettings);
    project->saveProjectFile();
    return true;
}

void editor::TransitionSettingsCmd::undo(){
    project->setTransitionDefaults(oldSettings);
    project->saveProjectFile();
}
