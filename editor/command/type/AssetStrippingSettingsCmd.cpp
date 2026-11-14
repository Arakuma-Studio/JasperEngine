// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetStrippingSettingsCmd.h"

using namespace jasperengine;

editor::AssetStrippingSettingsCmd::AssetStrippingSettingsCmd(Project* project, const AssetStrippingSettings& newSettings, const std::string& description):
    project(project), oldSettings(project->getAssetStripping()), newSettings(newSettings), description(description){
}

bool editor::AssetStrippingSettingsCmd::execute(){
    project->setAssetStripping(newSettings);
    project->saveProjectFile();
    return true;
}

void editor::AssetStrippingSettingsCmd::undo(){
    project->setAssetStripping(oldSettings);
    project->saveProjectFile();
}
