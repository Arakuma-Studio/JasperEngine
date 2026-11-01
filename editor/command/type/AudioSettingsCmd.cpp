// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "AudioSettingsCmd.h"

using namespace jasperengine;

editor::AudioSettingsCmd::AudioSettingsCmd(Project* project, AudioSettings before, AudioSettings after)
    : project(project), before(std::move(before)), after(std::move(after)) {
}

bool editor::AudioSettingsCmd::apply(const AudioSettings& settings){
    project->setAudioSettings(settings);
    // the standalone build's main.cpp bakes them in
    project->markGeneratedSourcesDirty();
    if (project->getProjectPath().empty())
        return true;
    return project->saveProjectFile();
}

bool editor::AudioSettingsCmd::execute(){
    return apply(after);
}

void editor::AudioSettingsCmd::undo(){
    apply(before);
}

std::vector<editor::Command::Touched> editor::AudioSettingsCmd::touchedPaths(bool) const{
    if (project->getProjectPath().empty())
        return {};
    return {{project->getProjectPath() / "project.yaml", false}};
}
