// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavigationSettingsCmd.h"

#include "Project.h"

using namespace jasperengine;

editor::NavigationSettingsCmd::NavigationSettingsCmd(Project* project, const NavigationProjectSettings& newSettings, std::string name):
    project(project), oldSettings(project->getNavigationSettings()), newSettings(newSettings), name(std::move(name)) {
}

bool editor::NavigationSettingsCmd::execute() {
    project->setNavigationSettings(newSettings);
    project->saveProjectFile();
    return true;
}

void editor::NavigationSettingsCmd::undo() {
    project->setNavigationSettings(oldSettings);
    project->saveProjectFile();
}
