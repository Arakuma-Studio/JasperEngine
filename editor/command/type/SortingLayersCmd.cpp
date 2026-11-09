// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "SortingLayersCmd.h"

#include "Project.h"

using namespace jasperengine;

editor::SortingSettings editor::SortingSettings::current(){
    SortingSettings settings;
    settings.layers = SortingLayers::getLayers();
    settings.mode = SortingLayers::getTransparencySortMode();
    settings.axis = SortingLayers::getTransparencySortAxis();
    return settings;
}

editor::SortingLayersCmd::SortingLayersCmd(Project* project, const SortingSettings& newSettings, std::string description):
    project(project), oldSettings(SortingSettings::current()), newSettings(newSettings), description(std::move(description)){
}

void editor::SortingLayersCmd::apply(const SortingSettings& settings){
    project->setSortingLayers(settings.layers);
    project->setTransparencySort(settings.mode, settings.axis);
    project->saveProjectFile();
}

bool editor::SortingLayersCmd::execute(){
    apply(newSettings);
    return true;
}

void editor::SortingLayersCmd::undo(){
    apply(oldSettings);
}
