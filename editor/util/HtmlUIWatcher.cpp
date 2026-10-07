// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUIWatcher.h"

#include "Project.h"
#include "Out.h"
#include "component/UIDocumentComponent.h"
#include "subsystem/UIDocumentSystem.h"

#include <set>

using namespace jasperengine;

std::vector<uint32_t> editor::HtmlUIWatcher::update(Project* project){
    std::vector<uint32_t> reloadedScenes;
    if (!project)
        return reloadedScenes;

    const auto now = std::chrono::steady_clock::now();
    if (!checkRequested && std::chrono::duration<double>(now - lastCheck).count() < intervalSec)
        return reloadedScenes;
    lastCheck = now;
    checkRequested = false;

    for (SceneProject* sceneProject : project->getHtmlUIScenes()){
        Scene* scene = sceneProject->scene;
        if (!scene || !scene->isComponentRegistered<UIDocumentComponent>())
            continue;
        auto components = scene->getComponentArray<UIDocumentComponent>();
        if (components->size() == 0)
            continue;
        UIDocumentSystem* system = scene->getSystem<UIDocumentSystem>().get();
        if (!system)
            continue;

        // every file a document of this scene reads; reloadChangedFiles compares their
        // stamps with the ones of each document's last (re)load
        std::set<std::string> unique;
        for (size_t i = 0; i < components->size(); i++){
            if (components->getComponentFromIndex(i).filename.empty())
                continue;
            for (const std::string& file : system->getDocumentFiles(components->getEntity(i))){
                unique.insert(file);
            }
        }
        if (unique.empty())
            continue;

        const std::vector<UIDocumentSystem::Reloaded> reloaded = system->reloadChangedFiles(std::vector<std::string>(unique.begin(), unique.end()));
        if (reloaded.empty())
            continue;

        for (const UIDocumentSystem::Reloaded& entry : reloaded){
            Out::info("[HTML UI] %s %s in scene '%s'", entry.stylesheetOnly ? "Restyled" : "Reloaded",
                      scene->getComponent<UIDocumentComponent>(entry.entity).filename.c_str(), sceneProject->name.c_str());
        }
        sceneProject->needUpdateRender = true;
        reloadedScenes.push_back(sceneProject->id);
    }

    return reloadedScenes;
}

void editor::HtmlUIWatcher::requestCheck(){
    checkRequested = true;
}
