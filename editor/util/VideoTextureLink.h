// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "texture/Texture.h"

namespace jasperengine::editor {

    // A texture can show a Video Player's render texture (Unity's RenderTexture target: UI
    // Images, materials). Like a camera's render texture (CameraTextureLink), the link is kept in
    // the texture id as "video|<entity>" so it survives serialization, and the framebuffer is
    // (re)bound by CameraTextureLink::resolve, which resolves every kind of link (the entity
    // remaps of pastes, duplicates and prefabs go through CameraTextureLink::parseLink and
    // CameraTextureLink::retarget, which keep the kind of the link).
    class VideoTextureLink {
    public:
        static std::string makeId(Entity videoEntity);
        static Entity parse(const std::string& textureId);
        static bool isVideoTexture(const Texture& texture);

        // The render texture of the entity's Video Player, its object created when missing (as
        // VideoPlayer::getFramebuffer does; its GPU side comes with the first frame); nullptr
        // when the entity has none
        static Framebuffer* outputFramebuffer(EntityRegistry* registry, Entity videoEntity);

        // a texture showing that player's frames (empty when the entity has no Video Player)
        static Texture make(EntityRegistry* registry, Entity videoEntity);

        // Points a video-linked texture at another player of the registry, bound to its texture
        // when it has a Video Player (resolve binds it later otherwise); NULL_ENTITY leaves no
        // texture
        static void retarget(EntityRegistry* registry, Texture& texture, Entity videoEntity);

        // display name for the texture slot ("video|7" -> the entity's name)
        static std::string videoName(EntityRegistry* registry, const Texture& texture);

        // true if any texture in the registry links to this player
        static bool isVideoUsed(EntityRegistry* registry, Entity videoEntity);
    };

}
