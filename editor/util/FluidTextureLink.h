// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "texture/Texture.h"

namespace jasperengine::editor {

    // A texture can show a FluidSimulation's output (its TEXTURE output mode, or any material
    // slot in any mode). Like a camera's render texture (CameraTextureLink), the link is kept in
    // the texture id as "fluid|<entity>" so it survives serialization, and the framebuffer is
    // (re)bound by CameraTextureLink::resolve, which resolves both kinds of links (the entity
    // remaps of pastes, duplicates and prefabs go through CameraTextureLink::parseLink and
    // CameraTextureLink::retarget, which keep the kind of the link).
    class FluidTextureLink {
    public:
        static std::string makeId(Entity fluidEntity);
        static Entity parse(const std::string& textureId);
        static bool isFluidTexture(const Texture& texture);

        // The output framebuffer of the entity's FluidSimulation, its simulator object created
        // when missing (as FluidSimulation::getTexture does; no GPU work until it runs);
        // nullptr when the entity has none
        static Framebuffer* outputFramebuffer(EntityRegistry* registry, Entity fluidEntity);

        // a texture showing that fluid's output (empty when the entity has no FluidSimulation)
        static Texture make(EntityRegistry* registry, Entity fluidEntity);

        // Points a fluid-linked texture at another fluid of the registry, bound to its output
        // when it has a FluidSimulation (resolve binds it later otherwise); NULL_ENTITY leaves
        // no texture
        static void retarget(EntityRegistry* registry, Texture& texture, Entity fluidEntity);

        // display name for the texture slot ("fluid|7" -> the entity's name)
        static std::string fluidName(EntityRegistry* registry, const Texture& texture);

        // true if any texture in the registry links to this fluid entity
        static bool isFluidUsed(EntityRegistry* registry, Entity fluidEntity);
    };

}
