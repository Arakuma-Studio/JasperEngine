// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "PhysicsQuerySettingsCmd.h"

using namespace jasperengine;

editor::PhysicsQuerySettingsCmd::PhysicsQuerySettingsCmd(Project* project, const PhysicsQuerySettings& before, const PhysicsQuerySettings& after)
    : project(project), before(before), after(after) {
}

bool editor::PhysicsQuerySettingsCmd::apply(const PhysicsQuerySettings& settings){
    // a refused save puts the previous settings back, so memory and project.yaml never disagree
    const PhysicsQuerySettings previous = project->getPhysicsQuerySettings();
    project->setPhysicsQuerySettings(settings);
    if (!project->getProjectPath().empty() && !project->saveProjectFile()){
        project->setPhysicsQuerySettings(previous);
        return false;
    }
    // the standalone build's main.cpp bakes them in
    project->markGeneratedSourcesDirty();
    return true;
}

bool editor::PhysicsQuerySettingsCmd::execute(){
    return apply(after);
}

void editor::PhysicsQuerySettingsCmd::undo(){
    apply(before);
}

std::vector<editor::Command::Touched> editor::PhysicsQuerySettingsCmd::touchedPaths(bool) const{
    if (project->getProjectPath().empty())
        return {};
    return {{project->getProjectPath() / "project.yaml", false}};
}
