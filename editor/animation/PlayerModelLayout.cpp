// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PlayerModelLayout.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"

#include "Out.h"
#include "Project.h"
#include "command/type/ModelLoadCmd.h"
#include "subsystem/MeshSystem.h"

using namespace jasperengine;

std::vector<Entity> editor::PlayerModelLayout::modelsNeedingReload(Project* project, uint32_t sceneId, Entity entity){
    std::vector<Entity> out;
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene || entity == NULL_ENTITY) return out;
    Scene* scene = sceneProject->scene;
    std::shared_ptr<MeshSystem> meshSystem = scene->getSystem<MeshSystem>();

    // the entity and its Transform descendants, in Transform order (parents first)
    auto transforms = scene->getComponentArray<Transform>();
    std::vector<Entity> branch;
    if (!scene->findComponent<Transform>(entity)){
        branch.push_back(entity);
    }
    for (size_t i = 0; i < transforms->size(); i++){
        Entity candidate = transforms->getEntity(i);
        bool under = candidate == entity;
        Entity parent = transforms->getComponentFromIndex(i).parent;
        for (size_t depth = 0; !under && parent != NULL_ENTITY && depth < transforms->size(); depth++){
            under = parent == entity;
            Transform* parentTransform = scene->findComponent<Transform>(parent);
            parent = parentTransform ? parentTransform->parent : NULL_ENTITY;
        }
        if (under) branch.push_back(candidate);
    }

    for (Entity candidate : branch){
        if (meshSystem->needsNodeHierarchyReload(candidate)) out.push_back(candidate);
    }
    return out;
}

size_t editor::PlayerModelLayout::ensureNodeHierarchyForPlayer(Project* project, uint32_t sceneId, Entity entity, Reloads& reloads){
    size_t ran = 0;
    for (Entity model : modelsNeedingReload(project, sceneId, entity)){
        Scene* scene = project->getScene(sceneId)->scene;
        const ModelComponent& component = scene->getComponent<ModelComponent>(model);
        const std::string filename = component.filename;
        std::unique_ptr<Command> reload;
        if (component.mergeStaticMeshes){
            reload.reset(new ModelLoadCmd(project, sceneId, model, filename, true));
        }else{
            reload.reset(new ModelLoadCmd(project, sceneId, model, filename));
        }
        if (!reload->execute()){
            Out::warning("Cannot reload '%s' in the node hierarchy its Animation Player needs", filename.c_str());
            continue;
        }
        reloads.push_back(std::move(reload));
        ran++;
    }
    return ran;
}

void editor::PlayerModelLayout::undoReloads(Reloads& reloads){
    for (auto it = reloads.rbegin(); it != reloads.rend(); ++it){
        (*it)->undo();
    }
}

void editor::PlayerModelLayout::redoReloads(Reloads& reloads){
    for (auto& reload : reloads){
        if (!reload->execute()){
            Out::warning("Cannot reload a model in the node hierarchy its Animation Player needs");
        }
    }
}
