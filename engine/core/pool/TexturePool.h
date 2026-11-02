// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXTUREPOOL_H
#define TEXTUREPOOL_H

#include "render/TextureRender.h"
#include "texture/TextureData.h"
#include <map>
#include <memory>
#include <array>
#include <set>

namespace jasperengine{

    typedef std::map< std::string, std::shared_ptr<TextureRender> > textures_t;

    class JASPERENGINE_API TexturePool{
    private:
        static textures_t& getMap();
        static std::set<std::string>& getInvalidated();

    public:
        // the sampler is baked into the backend texture, so a non-default one is part of the key
        static std::string getRenderId(const std::string& id, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV);

        static std::shared_ptr<TextureRender> get(const std::string& id);
        static std::shared_ptr<TextureRender> get(const std::string& id, TextureType type, const std::shared_ptr<std::array<TextureData,6>> &data, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV);
        static void add(const std::string& id, std::shared_ptr<TextureRender> render);
        // whether the pool's entry of the id is this render (created, stale or not)
        static bool holds(const std::string& id, const std::shared_ptr<TextureRender>& render);
        static void remove(const std::string& id);
        // also the id's sampler variants
        static void invalidate(const std::string& id);

        // Goes up when an image can be made where it could not before: a GPU texture made, pixels
        // pooled or replaced, a generator registered (TextureDataPool). A material that bound a
        // stand-in (Texture::getStandInCount) binds again when it moved on (RenderSystem): an image
        // ready, or assigned, after its material was bound reaches the GPU binding (BF15).
        static uint32_t getReadyEpoch();
        static void noteReady();
        // Goes up when a stale GPU texture is made again in place (invalidate, then get with data):
        // its old view is gone from every binding holding it, which binds again
        static uint32_t getRebuildEpoch();

        // necessary for engine shutdown
        static void clear();
        // destroys only entries no one else references (safe while other scenes are alive)
        static void clearUnused();

    };
}

#endif /* TEXTUREPOOL_H */
