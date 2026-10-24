// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "texture/Texture.h"

namespace jasperengine::editor {

    enum class ComponentType : int;

    // A texture can use a camera framebuffer as source (render to texture).
    // The link is kept in the texture id as "camera|<entity>" so it survives
    // serialization; the framebuffer pointer is (re)bound by resolve().
    class CameraTextureLink {
    public:
        static std::string makeId(Entity cameraEntity);
        static Entity parse(const std::string& textureId);
        static bool isCameraTexture(const Texture& texture);

        // The entity any texture link names: a camera's ("camera|<e>") or a fluid's
        // ("fluid|<e>", FluidTextureLink); NULL_ENTITY when the id is neither. Entity remaps
        // (prefab boundaries, pastes, duplicates) follow every link through it.
        static Entity parseLink(const std::string& textureId);
        static bool isLinkTexture(const Texture& texture);
        // The framebuffer a link of this id's kind binds to with `entity` as its target: the
        // camera's framebuffer or the fluid's output (nullptr when the entity has neither)
        static Framebuffer* linkFramebuffer(EntityRegistry* registry, const std::string& textureId, Entity entity);

        static Texture make(EntityRegistry* registry, Entity cameraEntity);

        // Points a camera-linked texture at another camera of the registry, bound to its
        // framebuffer when it has a Camera (resolve() binds it later otherwise); NULL_ENTITY
        // leaves no texture. For entity remaps: prefab boundaries, pastes, duplicates. A fluid
        // link stays one (FluidTextureLink::retarget).
        static void retarget(EntityRegistry* registry, Texture& texture, Entity cameraEntity);

        // display name for the texture slot ("camera|7" -> camera entity name)
        static std::string cameraName(EntityRegistry* registry, const Texture& texture);

        // the entity is what texture links can name: a Camera or a FluidSimulation. Removing,
        // adding or replacing one changes what links resolve to (resolve() the scene then)
        static bool isLinkSource(EntityRegistry* registry, Entity entity);
        static bool isLinkSourceType(ComponentType type);

        // true if any texture in the registry links to this camera entity
        static bool isCameraUsed(EntityRegistry* registry, Entity cameraEntity);

        // rebind every camera-linked texture in the registry to the current
        // framebuffer of its camera entity (or unbind if the camera is gone), and every
        // fluid-linked one to its FluidSimulation's output (FluidTextureLink),
        // flagging affected components to reload; returns true if any changed
        static bool resolve(EntityRegistry* registry);
        // resolve for what changed in these entities only: their own textures, or all of
        // the scene when one of them is a camera or a fluid
        static bool resolve(EntityRegistry* registry, const std::vector<Entity>& entities);
    };

}
