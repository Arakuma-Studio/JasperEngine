// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUiRenderSettingsCmd.h"

#include "Project.h"

using namespace jasperengine;

editor::HtmlUiRenderSettingsCmd::HtmlUiRenderSettingsCmd(Project* project, const HtmlUiRenderSettings& newSettings):
    project(project), oldSettings(project->getHtmlUiRenderSettings()), newSettings(newSettings){
}

bool editor::HtmlUiRenderSettingsCmd::execute(){
    project->setHtmlUiRenderSettings(newSettings);
    project->saveProjectFile();
    return true;
}

void editor::HtmlUiRenderSettingsCmd::undo(){
    project->setHtmlUiRenderSettings(oldSettings);
    project->saveProjectFile();
}
