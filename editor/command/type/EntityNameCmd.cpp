// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EntityNameCmd.h"
#include "command/PlayChangeReview.h"

using namespace jasperengine;

editor::EntityNameCmd::EntityNameCmd(Project* project, uint32_t sceneId, Entity entity, std::string name){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = entity;
    this->newName = name;
}

bool editor::EntityNameCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    // A Play-created entity is gone after Stop, and naming it would recreate the id
    if (!sceneProject || !sceneProject->scene->isEntityCreated(entity)){
        return false;
    }

    oldName = sceneProject->scene->getEntityName(entity);
    wasModified = project->getScene(sceneId)->isModified;

    prefabUndo.capture(project, sceneId, entity);
    sceneProject->scene->setEntityName(entity, newName);
    // Unity: a prefab instance's GameObject renamed is a name override (an instance root's
    // name is its own anyway)
    if (oldName != newName){
        project->recordPrefabStateEdit(sceneId, entity, PrefabObjectOverrides::StateName);
        sceneProject->isModified = true;
    }

    return true;
}

void editor::EntityNameCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene->isEntityCreated(entity)){
        return;
    }

    sceneProject->scene->setEntityName(entity, oldName);
    prefabUndo.restore(project, sceneId);

    sceneProject->isModified = wasModified;
}

bool editor::EntityNameCmd::mergeWith(editor::Command* otherCommand){
    EntityNameCmd* otherCmd = dynamic_cast<EntityNameCmd*>(otherCommand);
    if (otherCmd != nullptr){
        if (sceneId == otherCmd->sceneId && entity == otherCmd->entity){
            this->oldName = otherCmd->oldName;
            prefabUndo.mergeOlder(otherCmd->prefabUndo);

            this->wasModified = this->wasModified && otherCmd->wasModified;
            return true;
        }
    }
    return false;
}
void editor::EntityNameCmd::collectKeepCommands(KeepContext& context) const{
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene->isEntityCreated(entity)){
        return;
    }
    const std::string current = sceneProject->scene->getEntityName(entity);
    KeepRow row;
    row.kind = KeepRow::Kind::Name;
    row.entity = entity;
    row.property = "Name";
    row.playValue = KeepContext::text(current);
    Project* target = project;
    const uint32_t id = sceneId;
    const Entity edited = entity;
    row.factory = [target, id, edited, current]() -> std::unique_ptr<Command> {
        return std::make_unique<EntityNameCmd>(target, id, edited, current);
    };
    row.readEditValue = [target, id, edited]() -> std::string {
        SceneProject* restored = target->getScene(id);
        return restored && restored->scene->isEntityCreated(edited) ? KeepContext::text(restored->scene->getEntityName(edited)) : std::string("(none)");
    };
    context.add(std::move(row));
}
