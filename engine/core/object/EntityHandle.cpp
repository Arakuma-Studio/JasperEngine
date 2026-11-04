// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EntityHandle.h"

using namespace jasperengine;

EntityHandle::EntityHandle(Scene* scene){
    this->scene = scene;
    this->entity = scene->createEntity();
    this->entityOwned = true;
    this->sceneLifetime = scene->getLifetime();
}

EntityHandle::EntityHandle(Scene* scene, Entity entity){
    this->scene = scene;
    this->entity = entity;
    this->entityOwned = false;
}

// The entity goes with its owning handle, unless the scene went first: it destroyed the
// entity with the others, and its pointer is dangling.
EntityHandle::~EntityHandle(){
    if (ownsLiveEntity())
        scene->destroyEntity(entity);
}

bool EntityHandle::ownsLiveEntity() const{
    return entityOwned && scene && !sceneLifetime.expired() && scene->isEntityCreated(entity);
}

// Copies are non-owning observers: at most one handle may own an entity, otherwise the
// owned entity would be destroyed (and its sub-resources cleared) more than once. Use a
// move when you want to transfer ownership.
EntityHandle::EntityHandle(const EntityHandle& rhs){
    scene = rhs.scene;
    entity = rhs.entity;
    entityOwned = false;
}

EntityHandle& EntityHandle::operator=(const EntityHandle& rhs){
    if (this != &rhs){
        scene = rhs.scene;
        entity = rhs.entity;
        entityOwned = false;
        sceneLifetime.reset();
    }

    return *this;
}

// Moves transfer ownership: the source is left as a non-owning observer so only the
// destination destroys the entity.
EntityHandle::EntityHandle(EntityHandle&& rhs) noexcept{
    scene = rhs.scene;
    entity = rhs.entity;
    entityOwned = rhs.entityOwned;
    sceneLifetime = std::move(rhs.sceneLifetime);
    rhs.entityOwned = false;
}

EntityHandle& EntityHandle::operator=(EntityHandle&& rhs) noexcept{
    if (this != &rhs){
        scene = rhs.scene;
        entity = rhs.entity;
        entityOwned = rhs.entityOwned;
        sceneLifetime = std::move(rhs.sceneLifetime);
        rhs.entityOwned = false;
    }

    return *this;
}

Scene* EntityHandle::getScene() const{
    return scene;
}

Entity EntityHandle::getEntity() const{
    return entity;
}

EntityHandle::operator bool() const{
    return scene && entity != NULL_ENTITY && scene->isEntityCreated(entity);
}

void EntityHandle::setName(const std::string& name){
    scene->setEntityName(entity, name);
}

std::string EntityHandle::getName() const{
    return scene->getEntityName(entity);
}

void EntityHandle::setActive(bool active){
    scene->setEntityActive(entity, active);
}

bool EntityHandle::isActive() const{
    return scene->isEntityActive(entity);
}

bool EntityHandle::isActiveInHierarchy() const{
    return scene->isEntityActiveInHierarchy(entity);
}

void EntityHandle::setTag(const std::string& tag){
    // Unity refuses a tag missing from Project Settings > Tags and Layers
    if (!TagManager::isTagDefined(tag)){
        Log::error("Tag: %s is not defined.", tag.c_str());
        return;
    }
    scene->setEntityTag(entity, tag);
}

std::string EntityHandle::getTag() const{
    return scene->getEntityTag(entity);
}

bool EntityHandle::compareTag(const std::string& tag) const{
    if (!TagManager::isTagDefined(tag)){
        Log::error("Tag: %s is not defined.", tag.c_str());
        return false;
    }
    return scene->getEntityTag(entity) == tag;
}

void EntityHandle::setLayer(int layer){
    if (layer < 0 || layer >= MAX_LAYERS){
        Log::error("A GameObject can only be in one layer. The layer needs to be in the range [0...31]");
        return;
    }
    scene->setEntityLayer(entity, layer);
}

int EntityHandle::getLayer() const{
    return scene->getEntityLayer(entity);
}

bool EntityHandle::isEntityOwned() const{
    return entityOwned;
}

void EntityHandle::rebind(Scene* scene, Entity entity){
    this->scene = scene;
    this->entity = entity;
    if (entityOwned){
        sceneLifetime = scene ? scene->getLifetime() : std::weak_ptr<char>();
    }
}

void EntityHandle::setEntityOwned(bool entityOwned){
    // taking ownership: from here on the handle watches the scene's lifetime
    if (entityOwned && !this->entityOwned && scene){
        sceneLifetime = scene->getLifetime();
    }else if (!entityOwned){
        sceneLifetime.reset();
    }
    this->entityOwned = entityOwned;
}