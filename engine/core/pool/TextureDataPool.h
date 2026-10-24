// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXTUREDATAPOOL_H
#define TEXTUREDATAPOOL_H

#include "Engine.h"
#include "render/TextureRender.h"
#include "texture/TextureData.h"
#include "jobs/JobSystem.h"

#include <map>
#include <memory>
#include <array>
#include <functional>

#include <mutex>
#include <unordered_map>
#include <atomic>

namespace jasperengine{

    typedef std::map< std::string, std::shared_ptr<std::array<TextureData,6>> > texturesdata_t;

    struct TextureLoadResult {
        std::string id;
        ResourceLoadState state = ResourceLoadState::NotStarted;
        std::string errorMessage;
        std::shared_ptr<std::array<TextureData, 6>> data = nullptr;

        TextureLoadResult() = default;

        explicit operator bool() const {
            return state == ResourceLoadState::Finished;
        }
    };

    class JASPERENGINE_API TextureDataPool{
    private:
        static texturesdata_t& getMap();

        // an async load: the combined result, and the token its reads and decodes share
        struct PendingTexture {
            jobs::Task<std::array<TextureData,6>> task;
            jobs::CancelToken cancel;
        };
        static std::unordered_map<std::string, PendingTexture> pendingBuilds;
        static std::unordered_map<std::string, std::function<TextureData()>> generators;
        static std::mutex cacheMutex;
        static std::atomic<bool> shutdownRequested;

        static bool hasTexturePixels(const std::shared_ptr<std::array<TextureData,6>>& data, size_t numFaces);
        static size_t getTextureFaces(std::array<TextureData,6>& data);

        static std::array<TextureData,6> loadTextureInternal(const std::string& id, const std::array<std::string, 6>& paths, size_t numFaces, bool fixAlphaBorder, bool trackProgress);
        static std::string validateTextureFaces(std::array<TextureData,6>& data, size_t numFaces);
        // fixes the alpha border, applies the texture strategy and squares a cube face
        static void prepareFace(TextureData& face, size_t numFaces, bool fixAlphaBorder);
        // the async load: one asset read per file, decoded as Heavy Background jobs
        static PendingTexture startAsyncLoad(const std::string& id, const std::array<std::string, 6>& paths, size_t numFaces, bool fixAlphaBorder);
        // waits for every pending load (cancelling those not started first when `cancel`)
        // and drops them; cacheMutex held
        static void quiescePending(bool cancel);

    public:
        static std::shared_ptr<std::array<TextureData,6>> get(const std::string& id);
        static std::shared_ptr<std::array<TextureData,6>> get(const std::string& id, std::array<TextureData,6> data);
        // Inserts an already-built face array without copying it (used to hand pre-decoded model
        // textures to the pool). No-op if the id is already cached with pixels. Main-thread only.
        static void put(const std::string& id, std::shared_ptr<std::array<TextureData,6>> data);

        static TextureLoadResult loadFromFile(const std::string& id, const std::array<std::string, 6>& paths, size_t numFaces, bool fixAlphaBorder = false);

        // Pixels made by a function instead of read from a file (a material lobe's map packed
        // from two glTF images, TexturePack). setGenerator registers it under the id;
        // loadGenerated runs it once as a Heavy Background job (never on the main thread) and
        // is Loading until it is done, NotStarted for an id without one. The generator returns
        // a one-face TextureData whose pixels it hands over (not owned: the pool's copy owns
        // them); an empty one fails the load.
        static void setGenerator(const std::string& id, std::function<TextureData()> generator);
        static TextureLoadResult loadGenerated(const std::string& id);

        static void requestShutdown();
        // re-allows async loads after a recreated view
        static void cancelShutdown();

        static void remove(const std::string& id);
        static void clear();
        // removes only entries no one else references (safe while other scenes are alive)
        static void clearUnused();
    };
}

#endif /* TEXTUREDATAPOOL_H */
