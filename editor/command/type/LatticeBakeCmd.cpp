// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LatticeBakeCmd.h"
#include "command/CommandNames.h"

#include "Log.h"
#include "command/type/ModelLoadCmd.h"
#include "component/ModelComponent.h"
#include "subsystem/MeshSystem.h"
#include "util/ProjectUtils.h"

#include <algorithm>
#include "component/MeshComponent.h"

using namespace jasperengine;

editor::LatticeBakeCmd::LatticeBakeCmd(Project* project, uint32_t sceneId, const std::vector<ModelChange>& models,
                                       const std::vector<BindingsChange>& bindings){
    this->project = project;
    this->sceneId = sceneId;
    for (const ModelChange& change : models){
        ModelState state;
        state.entity = change.entity;
        state.newFilename = change.filename;
        this->models.push_back(state);
    }
    for (const BindingsChange& change : bindings){
        bindingCmds.push_back(std::make_unique<LatticeBindingsCmd>(project, sceneId, change.entity, change.bindings, change.includeChildren));
    }
    this->wasModified = project->getScene(sceneId)->isModified;
}

bool editor::LatticeBakeCmd::loadModel(Entity entity, const std::string& filename){
    SceneProject* sceneProject = project->getScene(sceneId);
    Scene* scene = sceneProject->scene;
    if (!scene->isEntityCreated(entity) || !scene->findComponent<ModelComponent>(entity) || !scene->findComponent<MeshComponent>(entity)){
        return false;
    }

    // a load into this Model still running on a worker ends first, so it cannot land afterwards
    ModelLoadCmd::completePendingLoad(sceneId, entity);

    // the generated entities stay (as when a scene is reopened): the baked file has the same
    // nodes, so the node, part, bone and animation entities and their components keep working
    const ModelComponent& model = scene->getComponent<ModelComponent>(entity);
    const bool skipEntities = !model.bonesIdMapping.empty() || !model.animations.empty() || !model.meshNodesMapping.empty();
    const bool loaded = scene->getSystem<MeshSystem>()->loadGLTF(entity, filename, false, skipEntities, false);

    // the load may have made entities (a node that had none): they join the scene's list
    ModelComponent& reloaded = scene->getComponent<ModelComponent>(entity);
    reloaded.needUpdateModel = false;
    std::vector<Entity> subEntities;
    ProjectUtils::collectModelEntities(scene, reloaded, subEntities);
    for (Entity subEntity : subEntities){
        if (std::find(sceneProject->entities.begin(), sceneProject->entities.end(), subEntity) == sceneProject->entities.end()){
            sceneProject->entities.push_back(subEntity);
        }
    }
    if (!loaded){
        // a failed load leaves no file in the Model: name the one it should have
        reloaded.filename = filename;
    }
    return loaded;
}

bool editor::LatticeBakeCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject){
        return false;
    }
    Scene* scene = sceneProject->scene;

    for (size_t c = 0; c < bindingCmds.size(); c++){
        if (!bindingCmds[c]->execute()){
            for (size_t u = c; u-- > 0;){
                bindingCmds[u]->undo();
            }
            return false;
        }
    }

    for (size_t m = 0; m < models.size(); m++){
        ModelState& state = models[m];
        ModelComponent* model = scene->findComponent<ModelComponent>(state.entity);
        if (!model){
            Log::error("Lattice bake: GameObject %u has no Model anymore", (unsigned)state.entity);
        }
        if (model){
            state.oldFilename = model->filename;
            prefabUndo.capture(project, sceneId, state.entity);
        }
        if (!model || !loadModel(state.entity, state.newFilename)){
            if (model){
                Log::error("Lattice bake: cannot load '%s', '%s' keeps '%s'", state.newFilename.c_str(),
                           scene->getEntityName(state.entity).c_str(), state.oldFilename.c_str());
            }
            for (size_t u = m + (model ? 1 : 0); u-- > 0;){
                loadModel(models[u].entity, models[u].oldFilename);
            }
            prefabUndo.restore(project, sceneId);
            prefabUndo.clear();
            for (size_t u = bindingCmds.size(); u-- > 0;){
                bindingCmds[u]->undo();
            }
            return false;
        }
        project->recordPrefabEdit(sceneId, state.entity, ComponentType::ModelComponent, {"filename"});
    }

    sceneProject->isModified = true;
    return true;
}

void editor::LatticeBakeCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject){
        return;
    }

    for (size_t m = models.size(); m-- > 0;){
        const ModelState& state = models[m];
        if (!loadModel(state.entity, state.oldFilename)){
            Log::error("Lattice bake undo: cannot load '%s' back into '%s'", state.oldFilename.c_str(),
                       sceneProject->scene->getEntityName(state.entity).c_str());
        }
    }
    prefabUndo.restore(project, sceneId);
    prefabUndo.clear();

    for (size_t c = bindingCmds.size(); c-- > 0;){
        bindingCmds[c]->undo();
    }

    sceneProject->isModified = wasModified;
}

std::string editor::LatticeBakeCmd::describe() const{
    std::vector<Entity> entities;
    for (const ModelState& model : models){
        entities.push_back(model.entity);
    }
    return entities.empty() ? std::string("Bake Lattice") : "Bake Lattice (" + CommandNames::entities(project, sceneId, entities) + ")";
}

size_t editor::LatticeBakeCmd::memoryBytes() const{
    size_t bytes = sizeof(*this) + models.capacity() * sizeof(ModelState);
    for (const ModelState& model : models){
        bytes += model.newFilename.capacity() + model.oldFilename.capacity();
    }
    for (const std::unique_ptr<LatticeBindingsCmd>& cmd : bindingCmds){
        bytes += cmd->memoryBytes();
    }
    return bytes;
}
