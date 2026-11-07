// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderOverridesCmd.h"

#include <utility>

using namespace jasperengine;

editor::ShaderOverridesCmd::ShaderOverridesCmd(Project* project, ShaderOverrides before, ShaderOverrides after)
    : project(project), before(std::move(before)), after(std::move(after)) {
}

bool editor::ShaderOverridesCmd::apply(const ShaderOverrides& overrides){
    // project.yaml is written from memory: a refused save puts the previous lists back, so memory and
    // the file never disagree (the history drops a command whose execute failed)
    const ShaderOverrides previous = project->getShaderOverrides();
    project->getShaderOverrides() = overrides;
    if (!project->getProjectPath().empty() && !project->saveProjectFile()){
        project->getShaderOverrides() = previous;
        return false;
    }
    return true;
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
