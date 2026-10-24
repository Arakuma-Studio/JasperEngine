// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CameraTextureLink.h"

#include "Catalog.h"
#include "FluidTextureLink.h"
#include "component/CameraComponent.h"
#include "component/FluidSimulationComponent.h"

using namespace jasperengine;

namespace {
    const std::string ID_PREFIX = "camera|";
}

std::string editor::CameraTextureLink::makeId(Entity cameraEntity){
    return ID_PREFIX + std::to_string(cameraEntity);
}

Entity editor::CameraTextureLink::parse(const std::string& textureId){
    if (textureId.compare(0, ID_PREFIX.size(), ID_PREFIX) != 0)
        return NULL_ENTITY;

    try {
        return static_cast<Entity>(std::stoul(textureId.substr(ID_PREFIX.size())));
    } catch (const std::exception&) {
        return NULL_ENTITY;
    }
}

bool editor::CameraTextureLink::isCameraTexture(const Texture& texture){
    return parse(texture.getId()) != NULL_ENTITY;
}

Entity editor::CameraTextureLink::parseLink(const std::string& textureId){
    Entity entity = parse(textureId);
    if (entity == NULL_ENTITY)
        entity = FluidTextureLink::parse(textureId);
    return entity;
}

bool editor::CameraTextureLink::isLinkTexture(const Texture& texture){
    return parseLink(texture.getId()) != NULL_ENTITY;
}

Framebuffer* editor::CameraTextureLink::linkFramebuffer(EntityRegistry* registry, const std::string& textureId, Entity entity){
    if (!registry || entity == NULL_ENTITY)
        return nullptr;
    if (FluidTextureLink::parse(textureId) != NULL_ENTITY)
        return FluidTextureLink::outputFramebuffer(registry, entity);
    CameraComponent* camera = registry->findComponent<CameraComponent>(entity);
    return camera ? camera->framebuffer : nullptr;
}

Texture editor::CameraTextureLink::make(EntityRegistry* registry, Entity cameraEntity){
    Texture texture;

    CameraComponent* camera = registry->findComponent<CameraComponent>(cameraEntity);
    if (camera){
        texture.setFramebuffer(camera->framebuffer);
        texture.setId(makeId(cameraEntity));
    }

    return texture;
}

void editor::CameraTextureLink::retarget(EntityRegistry* registry, Texture& texture, Entity cameraEntity){
    if (FluidTextureLink::isFluidTexture(texture)){
        FluidTextureLink::retarget(registry, texture, cameraEntity);
        return;
    }
    if (cameraEntity == NULL_ENTITY){
        texture = Texture();
        return;
    }

    CameraComponent* camera = registry ? registry->findComponent<CameraComponent>(cameraEntity) : nullptr;
    texture.setFramebuffer(camera ? camera->framebuffer : nullptr); // clears the id
    texture.setId(makeId(cameraEntity));
}

std::string editor::CameraTextureLink::cameraName(EntityRegistry* registry, const Texture& texture){
    Entity cameraEntity = parse(texture.getId());
    if (cameraEntity == NULL_ENTITY)
        return "";

    if (registry && registry->findComponent<CameraComponent>(cameraEntity))
        return registry->getEntityName(cameraEntity);

    return "< Missing camera >";
}

bool editor::CameraTextureLink::isLinkSource(EntityRegistry* registry, Entity entity){
    return registry && registry->isEntityCreated(entity) &&
           (registry->findComponent<CameraComponent>(entity) || registry->findComponent<FluidSimulationComponent>(entity));
}

bool editor::CameraTextureLink::isLinkSourceType(ComponentType type){
    return type == ComponentType::CameraComponent || type == ComponentType::FluidSimulationComponent;
}

bool editor::CameraTextureLink::isCameraUsed(EntityRegistry* registry, Entity cameraEntity){
    for (Entity entity : registry->getEntityList()){
        for (ComponentType cpType : Catalog::findComponents(registry, entity)){
            if (!Catalog::mayHoldReferences(registry, entity, cpType, false, true))
                continue;
            std::map<std::string, PropertyData> properties = Catalog::findEntityProperties(registry, entity, cpType);
            for (auto& [name, prop] : properties){
                if (prop.type != PropertyType::Texture || !prop.ref)
                    continue;

                if (parse(static_cast<Texture*>(prop.ref)->getId()) == cameraEntity)
                    return true;
            }
        }
    }

    return false;
}

namespace {
    bool resolveEntity(EntityRegistry* registry, Entity entity);
}

bool editor::CameraTextureLink::resolve(EntityRegistry* registry){
    bool changed = false;

    for (Entity entity : registry->getEntityList()){
        changed |= resolveEntity(registry, entity);
    }

    return changed;
}

bool editor::CameraTextureLink::resolve(EntityRegistry* registry, const std::vector<Entity>& entities){
    // A camera or a fluid among them may be what textures anywhere name
    for (Entity entity : entities){
        if (isLinkSource(registry, entity)){
            return resolve(registry);
        }
    }

    bool changed = false;
    for (Entity entity : entities){
        if (registry->isEntityCreated(entity)){
            changed |= resolveEntity(registry, entity);
        }
    }
    return changed;
}

namespace {
bool resolveEntity(EntityRegistry* registry, Entity entity){
    using namespace jasperengine::editor;
    bool changed = false;
    for (ComponentType cpType : Catalog::findComponents(registry, entity)){
        if (!Catalog::mayHoldReferences(registry, entity, cpType, false, true))
            continue;
        std::map<std::string, PropertyData> properties = Catalog::findEntityProperties(registry, entity, cpType);
        for (auto& [name, prop] : properties){
            if (prop.type != PropertyType::Texture || !prop.ref)
                continue;

            Texture* texture = static_cast<Texture*>(prop.ref);
            Framebuffer* target = nullptr;
            Entity cameraEntity = CameraTextureLink::parse(texture->getId());
            if (cameraEntity != NULL_ENTITY){
                CameraComponent* camera = registry->findComponent<CameraComponent>(cameraEntity);
                target = camera ? camera->framebuffer : nullptr;
            }else{
                Entity fluidEntity = FluidTextureLink::parse(texture->getId());
                if (fluidEntity == NULL_ENTITY)
                    continue;
                target = FluidTextureLink::outputFramebuffer(registry, fluidEntity);
            }

            if (texture->getFramebuffer() != target){
                std::string textureId = texture->getId();
                texture->setFramebuffer(target); // clears the id
                texture->setId(textureId);

                Catalog::updateEntity(registry, entity, prop.updateFlags);
                changed = true;
            }
        }
    }

    return changed;
}
}
