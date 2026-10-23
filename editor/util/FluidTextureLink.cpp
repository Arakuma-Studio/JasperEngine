// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FluidTextureLink.h"

#include "Catalog.h"
#include "component/FluidSimulationComponent.h"
#include "fluid/FluidSimulator.h"

#include <memory>

using namespace jasperengine;

namespace {
    const std::string ID_PREFIX = "fluid|";
}

std::string editor::FluidTextureLink::makeId(Entity fluidEntity){
    return ID_PREFIX + std::to_string(fluidEntity);
}

Entity editor::FluidTextureLink::parse(const std::string& textureId){
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

bool editor::FluidTextureLink::isFluidTexture(const Texture& texture){
    return parse(texture.getId()) != NULL_ENTITY;
}

Framebuffer* editor::FluidTextureLink::outputFramebuffer(EntityRegistry* registry, Entity fluidEntity){
    if (!registry || fluidEntity == NULL_ENTITY || !registry->isEntityCreated(fluidEntity))
        return nullptr;

    FluidSimulationComponent* fluid = registry->findComponent<FluidSimulationComponent>(fluidEntity);
    if (!fluid)
        return nullptr;

    if (!fluid->simulator)
        fluid->simulator = std::make_shared<FluidSimulator>();

    return fluid->simulator->getOutputFramebuffer();
}

Texture editor::FluidTextureLink::make(EntityRegistry* registry, Entity fluidEntity){
    Texture texture;

    Framebuffer* framebuffer = outputFramebuffer(registry, fluidEntity);
    if (framebuffer){
        texture.setFramebuffer(framebuffer); // clears the id
        texture.setId(makeId(fluidEntity));
    }

    return texture;
}

void editor::FluidTextureLink::retarget(EntityRegistry* registry, Texture& texture, Entity fluidEntity){
    if (fluidEntity == NULL_ENTITY){
        texture = Texture();
        return;
    }

    texture.setFramebuffer(outputFramebuffer(registry, fluidEntity)); // clears the id
    texture.setId(makeId(fluidEntity));
}

std::string editor::FluidTextureLink::fluidName(EntityRegistry* registry, const Texture& texture){
    Entity fluidEntity = parse(texture.getId());
    if (fluidEntity == NULL_ENTITY)
        return "";

    if (registry && registry->isEntityCreated(fluidEntity) && registry->findComponent<FluidSimulationComponent>(fluidEntity))
        return registry->getEntityName(fluidEntity);

    return "< Missing fluid >";
}

bool editor::FluidTextureLink::isFluidUsed(EntityRegistry* registry, Entity fluidEntity){
    for (Entity entity : registry->getEntityList()){
        for (ComponentType cpType : Catalog::findComponents(registry, entity)){
            if (!Catalog::mayHoldReferences(registry, entity, cpType, false, true))
                continue;
            std::map<std::string, PropertyData> properties = Catalog::findEntityProperties(registry, entity, cpType);
            for (auto& [name, prop] : properties){
                if (prop.type != PropertyType::Texture || !prop.ref)
                    continue;

                if (parse(static_cast<Texture*>(prop.ref)->getId()) == fluidEntity)
                    return true;
            }
        }
    }

    return false;
}
