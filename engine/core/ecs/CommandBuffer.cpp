// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CommandBuffer.h"

#include "EntityRegistry.h"
#include "Log.h"

using namespace jasperengine;

PendingEntity EntityCommandBuffer::create(){
    push(Op::Create, EntityTarget());
    PendingEntity entity;
    entity.index = pendingCount++;
    commands.back().value = (int32_t)entity.index;
    return entity;
}

void EntityCommandBuffer::destroy(EntityTarget entity){
    push(Op::Destroy, entity);
}

void EntityCommandBuffer::destroyHierarchy(EntityTarget entity){
    push(Op::DestroyHierarchy, entity);
}

void EntityCommandBuffer::setParent(EntityTarget child, EntityTarget parent, bool keepWorld){
    Command& command = push(Op::SetParent, child);
    command.other = parent;
    command.value = keepWorld ? 1 : 0;
}

void EntityCommandBuffer::setActive(EntityTarget entity, bool active){
    push(Op::SetActive, entity).value = active ? 1 : 0;
}

void EntityCommandBuffer::setName(EntityTarget entity, std::string name){
    push(Op::SetName, entity).payload.emplace<std::string>(std::move(name));
}

void EntityCommandBuffer::setTag(EntityTarget entity, std::string tag){
    push(Op::SetTag, entity).payload.emplace<std::string>(std::move(tag));
}

void EntityCommandBuffer::setLayer(EntityTarget entity, int layer){
    push(Op::SetLayer, entity).value = layer;
}

void EntityCommandBuffer::defer(std::function<void(Scene&)> fn){
    push(Op::Defer, EntityTarget()).payload.emplace<std::function<void(Scene&)>>(std::move(fn));
}

void EntityCommandBuffer::clear(){
    commands.clear();
    pendingCount = 0;
}

bool EntityCommandBuffer::resolve(EntityRegistry& registry, const EntityTarget& target, const std::vector<Entity>& created, const char* what, Entity& entity) const{
    if (target.isPending()){
        entity = target.pending < created.size() ? created[target.pending] : NULL_ENTITY;
        if (entity == NULL_ENTITY || !registry.isEntityCreated(entity)){
            Log::warn("EntityCommandBuffer: %s skipped, the entity it creates is not there", what);
            return false;
        }
        return true;
    }
    if (!registry.isAlive(target.ref)){
        Log::warn("EntityCommandBuffer: %s skipped, entity %u no longer exists", what, target.ref.id);
        return false;
    }
    entity = target.ref.id;
    return true;
}

void EntityCommandBuffer::playback(EntityRegistry& registry){
    // taken out first: the hooks fired below may record into this buffer again
    std::vector<Command> playing;
    playing.swap(commands);
    const uint32_t pending = pendingCount;
    pendingCount = 0;
    if (playing.empty()){
        return;
    }

    registry.advanceChangeTick();

    std::vector<Entity> created(pending, NULL_ENTITY);
    std::vector<Entity> destroys;

    for (size_t i = 0; i < playing.size(); i++){
        Command& command = playing[i];
        Entity entity = NULL_ENTITY;

        switch (command.op){
            case Op::Create:
                created[(size_t)command.value] = registry.createEntity();
                break;

            case Op::Destroy:
                // consecutive destroys go together: one removal pass per type
                destroys.clear();
                for (; i < playing.size() && playing[i].op == Op::Destroy; i++){
                    if (resolve(registry, playing[i].target, created, "destroy", entity)){
                        destroys.push_back(entity);
                    }
                }
                i--;
                if (!destroys.empty()){
                    registry.destroyEntities(destroys);
                }
                break;

            case Op::DestroyHierarchy:
                if (resolve(registry, command.target, created, "destroyHierarchy", entity)){
                    registry.destroyHierarchy(entity);
                }
                break;

            case Op::Add:
                if (resolve(registry, command.target, created, "add", entity)){
                    command.component(registry, entity, command.payload);
                }
                break;

            case Op::Remove:
                if (resolve(registry, command.target, created, "remove", entity)){
                    command.component(registry, entity, command.payload);
                }
                break;

            case Op::SetParent:
                if (resolve(registry, command.target, created, "setParent", entity)){
                    Entity parent = NULL_ENTITY;
                    if (command.other.isNull() || resolve(registry, command.other, created, "setParent (parent)", parent)){
                        registry.addEntityChild(parent, entity, command.value != 0);
                    }
                }
                break;

            case Op::SetActive:
                if (resolve(registry, command.target, created, "setActive", entity)){
                    registry.setEntityActive(entity, command.value != 0);
                }
                break;

            case Op::SetName:
                if (resolve(registry, command.target, created, "setName", entity)){
                    registry.setEntityName(entity, command.payload.get<std::string>());
                }
                break;

            case Op::SetTag:
                if (resolve(registry, command.target, created, "setTag", entity)){
                    registry.setEntityTag(entity, command.payload.get<std::string>());
                }
                break;

            case Op::SetLayer:
                if (resolve(registry, command.target, created, "setLayer", entity)){
                    registry.setEntityLayer(entity, command.value);
                }
                break;

            case Op::Defer:
                if (Scene* scene = registry.asScene()){
                    command.payload.get<std::function<void(Scene&)>>()(*scene);
                }else{
                    Log::error("EntityCommandBuffer: defer needs a Scene, the function was not run");
                }
                break;
        }
    }
}
