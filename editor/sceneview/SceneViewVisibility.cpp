// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SceneViewVisibility.h"
#include "render/SceneRender.h"

#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "picking/PickTypes.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // the entity and, with children, its whole Transform branch
    std::vector<Entity> branch(const SceneProject& sceneProject, Entity entity, bool children){
        std::vector<Entity> entities;
        Scene* scene = sceneProject.scene;
        if (!scene || !scene->isEntityCreated(entity)){
            return entities;
        }
        entities.push_back(entity);
        if (!children){
            return entities;
        }
        auto transforms = scene->getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++){
            const Entity candidate = transforms->getEntity(i);
            Entity ancestor = transforms->getComponentFromIndex(i).parent;
            for (size_t depth = 0; ancestor != NULL_ENTITY && depth < 4096; depth++){
                if (ancestor == entity){
                    entities.push_back(candidate);
                    break;
                }
                const Transform* parent = scene->findComponent<Transform>(ancestor);
                ancestor = parent ? parent->parent : NULL_ENTITY;
            }
        }
        return entities;
    }

    void writeFlags(SceneProject& sceneProject, Entity entity){
        if (!sceneProject.scene || !sceneProject.scene->isEntityCreated(entity)){
            return;
        }
        uint8_t flags = 0;
        if (sceneProject.viewState.hiddenEntities.count(entity)) flags |= EDITOR_HIDDEN;
        if (sceneProject.viewState.pickLockedEntities.count(entity)) flags |= EDITOR_PICK_LOCKED;
        sceneProject.scene->setEntityEditorFlags(entity, flags);
    }

}

void sceneview::setHidden(SceneProject& sceneProject, Entity entity, bool hidden, bool children){
    for (Entity target : branch(sceneProject, entity, children)){
        if (hidden){
            sceneProject.viewState.hiddenEntities.insert(target);
        }else{
            sceneProject.viewState.hiddenEntities.erase(target);
        }
        writeFlags(sceneProject, target);
    }
    sceneProject.needUpdateRender = true;
}

void sceneview::setPickLocked(SceneProject& sceneProject, Entity entity, bool locked, bool children){
    for (Entity target : branch(sceneProject, entity, children)){
        if (locked){
            sceneProject.viewState.pickLockedEntities.insert(target);
        }else{
            sceneProject.viewState.pickLockedEntities.erase(target);
        }
        writeFlags(sceneProject, target);
    }
}

void sceneview::showAll(SceneProject& sceneProject){
    const std::set<Entity> hidden = sceneProject.viewState.hiddenEntities;
    sceneProject.viewState.hiddenEntities.clear();
    for (Entity entity : hidden){
        writeFlags(sceneProject, entity);
    }
    sceneProject.needUpdateRender = true;
}

bool sceneview::isHidden(const SceneProject& sceneProject, Entity entity){
    return sceneProject.viewState.hiddenEntities.count(entity) > 0;
}

bool sceneview::isPickLocked(const SceneProject& sceneProject, Entity entity){
    return sceneProject.viewState.pickLockedEntities.count(entity) > 0;
}

size_t sceneview::hiddenCount(const SceneProject& sceneProject){
    if (!sceneProject.scene){
        return sceneProject.viewState.hiddenEntities.size();
    }
    size_t count = 0;
    for (Entity entity : sceneProject.viewState.hiddenEntities){
        if (sceneProject.scene->isEntityCreated(entity)){
            count++;
        }
    }
    return count;
}

void sceneview::applyLayerFilter(SceneProject& sceneProject, uint32_t visible, uint32_t pickable){
    sceneProject.viewState.layerFilter.visible = visible;
    sceneProject.viewState.layerFilter.pickable = pickable;
    if (sceneProject.sceneRender){
        if (Camera* camera = sceneProject.sceneRender->getCamera()){
            camera->setCullingMask(visible);
        }
    }
    sceneProject.needUpdateRender = true;
}

void sceneview::applyToScene(SceneProject& sceneProject){
    if (!sceneProject.scene){
        return;
    }
    for (Entity entity : sceneProject.viewState.hiddenEntities){
        writeFlags(sceneProject, entity);
    }
    for (Entity entity : sceneProject.viewState.pickLockedEntities){
        writeFlags(sceneProject, entity);
    }
    if (sceneProject.sceneRender){
        if (Camera* camera = sceneProject.sceneRender->getCamera()){
            camera->setCullingMask(sceneProject.viewState.layerFilter.visible);
            // what the Game view and the game never see is hidden through this camera only
            if (CameraComponent* component = sceneProject.scene->findComponent<CameraComponent>(camera->getEntity())){
                component->editorView = true;
            }
        }
    }
    sceneProject.needUpdateRender = true;
}

bool sceneview::hasHiddenContent(const SceneProject& sceneProject){
    return !sceneProject.viewState.hiddenEntities.empty() || sceneProject.viewState.layerFilter.visible != LAYER_MASK_EVERYTHING;
}

std::vector<Entity> sceneview::visibleEntities(const SceneProject& sceneProject, const std::vector<Entity>& entities){
    if (!hasHiddenContent(sceneProject) || !sceneProject.scene){
        return entities;
    }
    std::vector<Entity> visible;
    visible.reserve(entities.size());
    const uint32_t layers = sceneProject.viewState.layerFilter.visible;
    for (Entity entity : entities){
        if (sceneProject.viewState.hiddenEntities.count(entity)){
            continue;
        }
        if (sceneProject.scene->isEntityCreated(entity) && !(layers & (1u << (sceneProject.scene->getEntityLayer(entity) & 31)))){
            continue;
        }
        visible.push_back(entity);
    }
    return visible;
}
