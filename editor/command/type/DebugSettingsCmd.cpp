// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "DebugSettingsCmd.h"

using namespace jasperengine;

editor::DebugSettingsCmd::DebugSettingsCmd(Project* project, const DebugSettings& before, const DebugSettings& after)
    : project(project), before(before), after(after) {
}

bool editor::DebugSettingsCmd::apply(const DebugSettings& settings){
    // project.yaml is written from memory: a refused save puts the previous settings back, so memory
    // and the file never disagree (the history drops a command whose execute failed)
    const DebugSettings previous = project->getDebugSettings();
    project->setDebugSettings(settings);
    if (!project->getProjectPath().empty() && !project->saveProjectFile()){
        project->setDebugSettings(previous);
        return false;
    }
    // the standalone build's main.cpp bakes them in
    project->markGeneratedSourcesDirty();
    return true;
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
