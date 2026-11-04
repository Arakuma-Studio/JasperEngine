// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderOverridesCmd.h"

#include <utility>

using namespace jasperengine;

editor::ShaderOverridesCmd::ShaderOverridesCmd(Project* project, ShaderOverrides before, ShaderOverrides after)
    : project(project), before(std::move(before)), after(std::move(after)) {
}

bool editor::ShaderOverridesCmd::apply(const ShaderOverrides& overrides){
    project->getShaderOverrides() = overrides;
    if (project->getProjectPath().empty())
        return true;
    return project->saveProjectFile();
}

bool editor::ShaderOverridesCmd::execute(){
    return apply(after);
}

void editor::ShaderOverridesCmd::undo(){
    apply(before);
}

std::vector<editor::Command::Touched> editor::ShaderOverridesCmd::touchedPaths(bool) const{
    if (project->getProjectPath().empty())
        return {};
    return {{project->getProjectPath() / "project.yaml", false}};
}
