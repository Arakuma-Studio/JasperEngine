// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "InputActionsCmd.h"

using namespace jasperengine;

editor::InputActionsCmd::InputActionsCmd(Project* project, std::vector<InputAction> before, std::vector<InputAction> after)
    : project(project), before(std::move(before)), after(std::move(after)) {
}

bool editor::InputActionsCmd::apply(const std::vector<InputAction>& actions){
    project->setInputActions(actions);
    // the standalone build's main.cpp bakes them in
    project->markGeneratedSourcesDirty();
    if (project->getProjectPath().empty())
        return true;
    return project->saveProjectFile();
}

bool editor::InputActionsCmd::execute(){
    return apply(after);
}

void editor::InputActionsCmd::undo(){
    apply(before);
}

std::vector<editor::Command::Touched> editor::InputActionsCmd::touchedPaths(bool) const{
    if (project->getProjectPath().empty())
        return {};
    return {{project->getProjectPath() / "project.yaml", false}};
}
