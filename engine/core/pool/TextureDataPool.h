// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXTUREDATAPOOL_H
#define TEXTUREDATAPOOL_H

#include "Engine.h"
#include "render/TextureRender.h"
#include "texture/TextureData.h"
#include "texture/CookedTextures.h"
#include "texture/TexturePrepare.h"
#include "jobs/JobSystem.h"

#include <map>
#include <memory>
#include <array>
#include <functional>

#include <mutex>
#include <unordered_map>
#include <atomic>
#include <string>
#include <vector>

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

    // What a file-backed pool entry was decoded from, so a hot reload decodes it again with
    // the same settings (S05 R11)
    struct TextureSource {
        std::array<std::string, 6> paths;   // as loadFromFile got them (an SVG with its ?svgScale=)
        size_t numFaces = 1;
        bool fixAlphaBorder = false;
    };

    // A resident texture, as the VRAM report and the budget see it (spec S14 R12, S15)
    struct TextureResidentInfo {
        std::string id;
        bool cooked = false;
        bool streamable = false;
        GpuTextureFormat format = GpuTextureFormat::RGBA8;   // uploaded
        int logicalWidth = 0, logicalHeight = 0;             // Texture::getWidth
        int storedWidth = 0, storedHeight = 0;               // the GPU image's level 0
        int levels = 1;
        int droppedLevels = 0;                               // quality + budget
        uint64_t bytes = 0;
    };

    class JASPERENGINE_API TextureDataPool{
    public:
        // The raw path's import processing of a file (the editor's import settings; spec S14 R4)
        using ProcessingProvider = std::function<bool(const std::string& path, TextureProcessing& out)>;

    private:
        struct Resident;
        struct BasisJob;

        // One cooked file's first bytes, kept from firstLevel down: the levels that fit, or the
        // progressive tail (the levels <= 64 px) and where the rest ends
        struct CookedPiece {
            TextureData data;
            bool complete = true;
            bool hasTail = false;
            uint64_t end = 0;
            uint32_t firstLevel = 0;
            std::shared_ptr<BasisJob> basis;
        };
        static bool loadCookedBytes(const std::string& id, const CookedTextureEntry& entry, const std::vector<uint8_t>& bytes,
                                    uint32_t firstLevel, bool allowTail, bool deferBasis, const TextureFormatSupport& support,
                                    bool lowMemory, CookedPiece& out, std::string& error);

        // One load stage's outcome: the levels, and (a progressive or ranged cooked load) what
        // is still to read
        struct LoadResult {
            std::array<TextureData,6> data;
            bool cooked = false;
            bool complete = true;
            // a cooked file read short of the levels the load keeps
            struct Rest {
                size_t face = 0;
                CookedTextureEntry entry;
                std::shared_ptr<std::vector<uint8_t>> prefix;
                uint64_t end = 0;
                uint32_t firstLevel = 0;
                bool deferBasis = false;
            };
            std::vector<Rest> rest;
            // a Basis transcode left to the main thread (web without threads)
            std::shared_ptr<BasisJob> basis;
        };

        // an async load: the combined result, and the token its reads and decodes share
        struct PendingTexture {
            jobs::Task<LoadResult> task;
            jobs::CancelToken cancel;
        };

        static std::unordered_map<std::string, Resident>& getResidents();
        static texturesdata_t& getMap();
        static std::unordered_map<std::string, uint32_t>& getGenerations();
        static std::unordered_map<std::string, TextureSource>& getSources();
        static std::atomic<uint32_t> replaceEpoch;

        static std::unordered_map<std::string, PendingTexture> pendingBuilds;
        // loads that finish in update(): the rest of a progressive load, budget and quality
        // reloads (their result replaces the entry, never shown half-loaded)
        static std::unordered_map<std::string, PendingTexture> backgroundBuilds;
        static std::vector<std::shared_ptr<BasisJob>> basisJobs;
        static std::unordered_map<std::string, std::function<TextureData()>> generators;
        static std::mutex cacheMutex;
        static std::atomic<bool> shutdownRequested;

        static bool hasTexturePixels(const std::shared_ptr<std::array<TextureData,6>>& data, size_t numFaces);
        static size_t getTextureFaces(std::array<TextureData,6>& data);

        static std::array<TextureData,6> loadTextureInternal(const std::string& id, const std::array<std::string, 6>& paths, size_t numFaces, bool fixAlphaBorder, bool trackProgress);
        // the raw path: decoded faces through the import processing into levels
        static void prepareLevels(std::array<TextureData,6>& data, size_t numFaces, const std::string& path, bool fixAlphaBorder);
        // the drop (levels skipped at the top) a cooked texture loads with now
        static int levelDropFor(const std::string& id, const CookedTextureEntry& entry);
        // the cooked files of a load, when every face has one
        static bool findCooked(const std::array<std::string, 6>& paths, size_t numFaces, std::vector<CookedTextureEntry>& entries);
        static std::string validateTextureFaces(std::array<TextureData,6>& data, size_t numFaces);
        // the async load: one asset read per file, decoded as Heavy Background jobs; a cooked
        // file reads its first bytes (metadata and the mip tail) first
        static PendingTexture startAsyncLoad(const std::string& id, const std::array<std::string, 6>& paths, size_t numFaces, bool fixAlphaBorder);
        // the second read of the cooked files a first read left short
        static PendingTexture startRestLoad(const std::string& id, LoadResult&& partial);
        // a finished load: in the map and the resident set (main thread)
        static void adopt(const std::string& id, LoadResult& result, bool replacing);
        static void noteResident(const std::string& id, const std::array<TextureData,6>& data, bool cooked);
        static void forgetResident(const std::string& id);
        static void rebalanceBudget();
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
        // Drops the entry even while others hold it, so the next load decodes again
        static void invalidate(const std::string& id);

        // Hot reload (S05 R13): the entry becomes `data` and its generation goes up; every
        // Texture of the id sees the new pixels on its next use, without a destroy(). Main thread.
        static void replace(const std::string& id, std::shared_ptr<std::array<TextureData,6>> data);
        static uint32_t generation(const std::string& id);
        // Goes up with every replace(): a Texture skips the generation lookup while it stays
        static uint32_t getReplaceEpoch() { return replaceEpoch.load(std::memory_order_acquire); }
        static bool getSource(const std::string& id, TextureSource& out);
        // The file-backed ids reading `systemPath` (absolute; FileData::getSystemPath of theirs)
        static std::vector<std::string> findIdsUsingFile(const std::string& systemPath);
        // Decodes `source` as loadFromFile would; any thread. Throws with the decoder's message.
        static std::array<TextureData,6> decode(const std::string& id, const TextureSource& source);
        // Faces the pool decoded from files, shared: when the last holder lets go, their pixels
        // are freed (an upload with releaseDataAfterLoad frees them earlier; a texture kept on the
        // CPU, or replaced by a hot reload before it was drawn, had nobody to free them)
        static std::shared_ptr<std::array<TextureData,6>> shareDecoded(std::array<TextureData,6> data);

        static void clear();
        // removes only entries no one else references (safe while other scenes are alive)
        static void clearUnused();

        // ---- cooked textures (spec S14)

        static void setProcessingProvider(ProcessingProvider provider);
        // Once a frame, main thread (Engine::systemDraw): swaps finished progressive loads in,
        // runs the main thread's Basis transcodes (a 4 ms slice of each frame once the scenes are
        // loaded, web builds without threads) and reloads the textures the budget re-planned
        static void update();
        // textureQuality: top mips dropped from streamable textures (0 full ... 3 an eighth)
        static void setTextureQuality(int quality);
        static int getTextureQuality();
        // bytes; 0 = unlimited. Over it, the largest streamable texture drops one more mip until
        // the resident bytes fit (a texture keeps at least 64 px); re-planned on load and unload
        static void setMemoryBudget(uint64_t bytes);
        static uint64_t getMemoryBudget();
        static uint64_t getResidentBytes();
        // RGB565 / RGBA4 as the last resort on GL-family backends (R11)
        static void setLowMemoryFallback(bool enabled);
        static bool getLowMemoryFallback();
        static std::vector<TextureResidentInfo> getResidentTextures();
        // loads waiting on a progressive swap, a transcode or a budget reload
        static size_t pendingWorkCount();
        // The texture loads still running: a frame drawn now shows a stand-in for each (an empty
        // texture while it reads and decodes, a progressive load's small tail while the rest
        // reads, a transcode waiting for the main thread). Readiness and screenshots wait for
        // none to be left; a finished load the next draw adopts is not counted.
        static size_t getLoadingCount();
        // their ids (the texture paths), at most `max`
        static std::vector<std::string> getLoadingIds(size_t max);
    };
}

#endif /* TEXTUREDATAPOOL_H */
