// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VideoTextureLink.h"

#include "Catalog.h"
#include "component/VideoPlayerComponent.h"
#include "media/VideoTextureConverter.h"

#include <memory>

using namespace jasperengine;

namespace {
    const std::string ID_PREFIX = "video|";
}

std::string editor::VideoTextureLink::makeId(Entity videoEntity){
    return ID_PREFIX + std::to_string(videoEntity);
}

Entity editor::VideoTextureLink::parse(const std::string& textureId){
    if (textureId.compare(0, ID_PREFIX.size(), ID_PREFIX) != 0)
        return NULL_ENTITY;

    const std::string number = textureId.substr(ID_PREFIX.size());
    if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos)
        return NULL_ENTITY;

    try {
        return static_cast<Entity>(std::stoul(number));
    } catch (const std::exception&) {
        return NULL_ENTITY;
    }
}

bool editor::VideoTextureLink::isVideoTexture(const Texture& texture){
    return parse(texture.getId()) != NULL_ENTITY;
}

Framebuffer* editor::VideoTextureLink::outputFramebuffer(EntityRegistry* registry, Entity videoEntity){
    if (!registry || videoEntity == NULL_ENTITY || !registry->isEntityCreated(videoEntity) ||
        !registry->isComponentRegistered<VideoPlayerComponent>())
        return nullptr;

    VideoPlayerComponent* player = registry->findComponent<VideoPlayerComponent>(videoEntity);
    if (!player)
        return nullptr;

    if (!player->output)
        player->output = std::make_shared<VideoTextureConverter>();

    return player->output->getFramebuffer();
}

Texture editor::VideoTextureLink::make(EntityRegistry* registry, Entity videoEntity){
    Texture texture;

    Framebuffer* framebuffer = outputFramebuffer(registry, videoEntity);
    if (framebuffer){
        texture.setFramebuffer(framebuffer); // clears the id
        texture.setId(makeId(videoEntity));
    }

    return texture;
}

void editor::VideoTextureLink::retarget(EntityRegistry* registry, Texture& texture, Entity videoEntity){
    if (videoEntity == NULL_ENTITY){
        texture = Texture();
        return;
    }

    texture.setFramebuffer(outputFramebuffer(registry, videoEntity)); // clears the id
    texture.setId(makeId(videoEntity));
}

std::string editor::VideoTextureLink::videoName(EntityRegistry* registry, const Texture& texture){
    Entity videoEntity = parse(texture.getId());
    if (videoEntity == NULL_ENTITY)
        return "";

    if (registry && registry->isEntityCreated(videoEntity) && registry->isComponentRegistered<VideoPlayerComponent>() &&
        registry->findComponent<VideoPlayerComponent>(videoEntity))
        return registry->getEntityName(videoEntity);

    return "< Missing video player >";
}

bool editor::VideoTextureLink::isVideoUsed(EntityRegistry* registry, Entity videoEntity){
    for (Entity entity : registry->getEntityList()){
        for (ComponentType cpType : Catalog::findComponents(registry, entity)){
            if (!Catalog::mayHoldReferences(registry, entity, cpType, false, true))
                continue;
            std::map<std::string, PropertyData> properties = Catalog::findEntityProperties(registry, entity, cpType);
            for (auto& [name, prop] : properties){
                if (prop.type != PropertyType::Texture || !prop.ref)
                    continue;

                if (parse(static_cast<Texture*>(prop.ref)->getId()) == videoEntity)
                    return true;
            }
        }
    }

    return false;
}
