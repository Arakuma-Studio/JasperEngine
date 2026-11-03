// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "DebugSettingsCmd.h"

using namespace jasperengine;

editor::DebugSettingsCmd::DebugSettingsCmd(Project* project, const DebugSettings& before, const DebugSettings& after)
    : project(project), before(before), after(after) {
}

bool editor::DebugSettingsCmd::apply(const DebugSettings& settings){
    project->setDebugSettings(settings);
    // the standalone build's main.cpp bakes them in
    project->markGeneratedSourcesDirty();
    if (project->getProjectPath().empty())
        return true;
    return project->saveProjectFile();
}

bool editor::DebugSettingsCmd::execute(){
    return apply(after);
}

void editor::DebugSettingsCmd::undo(){
    apply(before);
}

std::vector<editor::Command::Touched> editor::DebugSettingsCmd::touchedPaths(bool) const{
    if (project->getProjectPath().empty())
        return {};
    return {{project->getProjectPath() / "project.yaml", false}};
}
