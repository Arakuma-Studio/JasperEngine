// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ApplePrivacySettingsCmd.h"

using namespace jasperengine;

editor::ApplePrivacySettingsCmd::ApplePrivacySettingsCmd(Project* project, const ApplePrivacy::Manifest& after)
    : project(project), before(project->getApplePrivacy()), after(ApplePrivacy::normalized(after)) {
}

bool editor::ApplePrivacySettingsCmd::apply(const ApplePrivacy::Manifest& privacy){
    project->setApplePrivacy(privacy);
    if (project->getProjectPath().empty())
        return true;
    return project->saveProjectFile();
}

bool editor::ApplePrivacySettingsCmd::execute(){
    return apply(after);
}

void editor::ApplePrivacySettingsCmd::undo(){
    apply(before);
}

bool editor::ApplePrivacySettingsCmd::mergeWith(Command* otherCommand){
    ApplePrivacySettingsCmd* older = dynamic_cast<ApplePrivacySettingsCmd*>(otherCommand);
    if (older == nullptr || older->project != project)
        return false;
    before = older->before;
    return true;
}

std::vector<editor::Command::Touched> editor::ApplePrivacySettingsCmd::touchedPaths(bool) const{
    if (project->getProjectPath().empty())
        return {};
    return {{project->getProjectPath() / "project.yaml", false}};
}
