// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_WORLD_RUNTIME_H
#define TERRAIN_WORLD_RUNTIME_H

// T4: one Terrain World while its scene runs. It reads its tiles through a TerrainWorldSource (the
// project folder in the editor, the pack in a player), streams them round its focuses (TileStreamer),
// keeps edited tiles pinned in memory, answers surface queries (R24), and holds what render and
// collision read. Main thread; jobs get copies, never this.

#include "Export.h"
#include "ecs/Entity.h"
#include "jobs/JobSystem.h"
#include "math/Vector3.h"
#include "terrain/TerrainSurface.h"
#include "terrain/world/TerrainTileFormat.h"
#include "terrain/world/TerrainWorldData.h"
#include "terrain/world/TerrainWorldSelect.h"
#include "terrain/world/TileStreamer.h"
#include "terrain/world/TiledMapStore.h"
#include "terrain/world/WorldLattice.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace jasperengine {

    class Scene;
    class TerrainWorldCollision;
    class TerrainWorldRender;

    // R53: where a world's data comes from
    class JASPERENGINE_API TerrainWorldSource {
    public:
        virtual ~TerrainWorldSource() = default;
        // The manifest and the tiles present; main thread, once at start
        virtual bool manifest(terrain::WorldManifest& out, std::string& error) = 0;
        // A path AssetIOService and FileData read (a loose file or a pack entry)
        virtual std::string tilePath(terrain::TileCoord tile) const = 0;
        // overview.jtile, or empty: the runtime assembles it from the tiles (the editor's stale cache)
        virtual std::string overviewPath() const = 0;
        // R27: T5's tiles/x_z.jetrees beside the tile (the manifest says whether it is there)
        virtual std::string treePath(terrain::TileCoord tile) const;
        // The project folder, which the editor writes: Play never writes (R31)
        virtual bool editable() const { return false; }
    };

    // R53: a player's world, from the pack: <folder>/world.bin, overview.jtile, tiles/x_z.jtile
    class JASPERENGINE_API CookedWorldSource final : public TerrainWorldSource {
    public:
        explicit CookedWorldSource(std::string folder): folder(std::move(folder)) {}
        bool manifest(terrain::WorldManifest& out, std::string& error) override;
        std::string tilePath(terrain::TileCoord tile) const override;
        std::string overviewPath() const override;

    private:
        std::string folder;
    };

    // tiles/x_z.jtile under a world's folder
    JASPERENGINE_API std::string terrainWorldTileName(terrain::TileCoord tile);
    // R27: tiles/x_z.jetrees, T5's trees of that tile
    JASPERENGINE_API std::string terrainWorldTreeName(terrain::TileCoord tile);
    // zstd's decompressor: every player has it (D7)
    JASPERENGINE_API terrain::TileCodec terrainWorldDecodeCodec();
    // R34's defaults for the platform this build runs on
    JASPERENGINE_API terrain::WorldPlatformBudget terrainWorldPlatformBudget();

    enum class TerrainWorldReadyLevel : uint8_t { Render, Collision };
    // R19
    enum class TerrainWorldPointState : uint8_t { Absent, Overview, Full };
    JASPERENGINE_API const char* terrainWorldPointStateName(TerrainWorldPointState state);

    // R45's numbers, for sample_terrain {world}, the panel and Lua's getStats
    struct TerrainWorldStats {
        int tiles = 0;
        int states[8] = {};
        uint64_t gpuBytes = 0, cpuBytes = 0;
        uint64_t gpuCap = 0, cpuCap = 0;
        int poolSize = 0;
        int queue = 0, inFlight = 0;
        uint64_t uploadBytesLastFrame = 0, uploadBytesTotal = 0;
        int uploadRetries = 0;
        double mainThreadMs = 0.0, worstMainThreadMs = 0.0;
        int draws[4][3] = {};       // per view: colour, depth, G-buffer
        int nodesVisited = 0, nodesSelected = 0;
        int collisionFine = 0, collisionCoarse = 0, frozenBodies = 0;
        int seamErrors = 0;
        int poolExhausted = 0;
        int latePages = 0;
        std::vector<std::string> errors;
        bool ready = false;
        bool frozen = false;
    };

    struct TerrainWorldAreaWait {
        int id = 0;
        float x = 0.0f, z = 0.0f, radius = 0.0f;
        TerrainWorldReadyLevel level = TerrainWorldReadyLevel::Render;
    };

    class JASPERENGINE_API TerrainWorldRuntime final : public terrain::TileAccess {
    public:
        // R31: tiles a game may hold edited at once
        static constexpr int MAX_RUNTIME_EDITED_TILES = 16;
        // R52: the overview follows edits at most this often
        static constexpr double OVERVIEW_REFRESH_EDITOR = 0.5;
        static constexpr double OVERVIEW_REFRESH_PLAY = 1.0;

        TerrainWorldRuntime(Scene* scene, Entity entity, std::shared_ptr<TerrainWorldSource> source, bool playing);
        ~TerrainWorldRuntime() override;

        TerrainWorldRuntime(const TerrainWorldRuntime&) = delete;
        TerrainWorldRuntime& operator=(const TerrainWorldRuntime&) = delete;

        // Reads the manifest, makes the streamer and store, starts the overview's load (D10)
        bool start(std::string& error);
        bool started() const { return streamer != nullptr; }
        // R10: the overview exists, so every present tile draws
        bool isReady() const { return overviewDone; }

        Scene* getScene() const { return scene; }
        Entity getEntity() const { return entity; }
        const terrain::WorldManifest& manifest() const { return worldManifest; }
        terrain::WorldManifest& manifest() { return worldManifest; }
        const terrain::WorldLattice& lattice() const { return worldManifest.lattice; }
        float maxHeight() const { return worldManifest.maps.maxHeight; }
        const terrain::WorldOccupancy& occupancy() const { return worldOccupancy; }
        terrain::TileStreamer& tiles() { return *streamer; }
        const terrain::TileStreamer& tiles() const { return *streamer; }
        terrain::TiledMapStore& store() { return *mapStore; }
        const terrain::WorldOverview& getOverview() const { return overview; }
        // bumped when the overview's samples or colours change: render makes its textures again
        uint32_t overviewRevision() const { return overviewRev; }
        bool playing() const { return isPlaying; }
        TerrainWorldSource& source() { return *worldSource; }

        // D9: the pump, once a frame, paused or not
        void pump(double now);

        // R15, R16: focuses. The system sets the automatic ones (camera, the component's) each frame.
        void setAutoFocuses(const std::vector<terrain::StreamFocus>& focuses);
        int addFocus(float x, float z, float radiusScale = 1.0f, float velocityX = 0.0f, float velocityZ = 0.0f);
        bool setFocus(int id, float x, float z, float velocityX = 0.0f, float velocityZ = 0.0f);
        bool removeFocus(int id);
        int focusCount() const { return static_cast<int>(autoFocuses.size() + scriptFocuses.size()); }

        // R18
        bool isAreaReady(float x, float z, float radius, TerrainWorldReadyLevel level) const;
        // A pending area is a focus until it is ready or dropped
        int waitForArea(float x, float z, float radius, TerrainWorldReadyLevel level);
        void dropAreaWait(int id);
        // the tiles of an area that are not ready yet
        std::vector<terrain::TileCoord> missingTiles(float x, float z, float radius, TerrainWorldReadyLevel level) const;

        // R19, R24. mayWait: a job or the editor may read a cold tile now; the game's main thread
        // answers from the overview with exact false
        terrain::SurfaceSample query(float localX, float localZ, bool mayWait);
        bool raycast(const Vector3& origin, const Vector3& direction, float maxT, bool skipHoles, bool mayWait, Vector3& point, float& t);
        TerrainWorldPointState pointState(float localX, float localZ) const;
        terrain::TileState tileState(terrain::TileCoord tile) const;
        // The data of a tile in memory, edited or streamed; null otherwise
        std::shared_ptr<const terrain::WorldTile> resident(terrain::TileCoord tile) const;
        // Reads and decodes a present tile now (editor, jobs, ensureCollisionAt)
        std::shared_ptr<const terrain::WorldTile> loadNow(terrain::TileCoord tile);

        // The derived sections (min/max, overview samples and colour, skirt) of edited tiles now, not at
        // the next pump: before a flush writes them
        void settleEdits();
        // R31: what the editor flushes: tiles changed or made since the last take, and removed ones
        std::vector<terrain::TileCoord> takeDirtyFiles();
        std::vector<terrain::TileCoord> takeRemovedFiles();
        bool hasDirtyFiles() const { return !dirtyFiles.empty() || !removedFiles.empty(); }
        // A flushed tile no longer needs its pin (the editor keeps its cache under the caps)
        void releaseFlushed(terrain::TileCoord tile);
        // R31: an external change (git pull): reload it
        void fileChanged(terrain::TileCoord tile, bool exists);
        // R51: the seams of a tile against its resident neighbours; repair favours the lower (x, z)
        int checkSeams(terrain::TileCoord tile, bool repair);
        // R52: the overview from every tile again (Rebuild Overview); a job unless wait
        void rebuildOverview(bool wait);
        // Whether the overview changed since it was last saved, for the editor's cache
        bool overviewNeedsSave() const { return overviewUnsaved; }
        void overviewSaved() { overviewUnsaved = false; }
        // R52: the layers' colours for the macro colour (base first, RGBA floats)
        void setLayerColours(std::vector<float> colours, int layerCount);

        // The budgets from the component, with R34's defaults
        void applySettings(int poolSize, float lodDistance, float uploadBudgetMiB, int maxNearTiles);
        void setFrozen(bool frozen);
        bool frozen() const { return streamer && streamer->frozen(); }

        TerrainWorldStats stats() const;
        TerrainWorldStats& mutableStats() { return statsCache; }
        // R10's late pages: a tile the selection wanted near that had no page (render reports them)
        void noteLatePages(int count) { statsCache.latePages += count; }

        // TileAccess, for the store: present tiles load now when they must
        bool present(terrain::TileCoord tile) const override;
        std::shared_ptr<const terrain::WorldTile> tile(terrain::TileCoord tile) const override;
        std::shared_ptr<terrain::WorldTile> writable(terrain::TileCoord tile) override;
        void add(terrain::TileCoord tile, std::shared_ptr<terrain::WorldTile> data) override;
        void restore(terrain::TileCoord tile, std::shared_ptr<terrain::WorldTile> data) override;
        void remove(terrain::TileCoord tile) override;
        void changed(terrain::TileCoord tile) override;
        bool bounds(terrain::TileCoord& low, terrain::TileCoord& high) const override;
        bool mayWrite(terrain::TileCoord tile, std::string* why) const override;

        // What render and collision hang on the runtime (their own files)
        std::unique_ptr<TerrainWorldRender> render;
        std::unique_ptr<TerrainWorldCollision> collision;

        // Goes up each time a tile's data changes in memory (an edit, a reload): collision rebuilds
        // what it built from an older one
        uint32_t tileRevision(terrain::TileCoord tile) const;

    private:
        struct ScriptFocus {
            int id = 0;
            terrain::StreamFocus focus;
        };

        void startOverviewLoad();
        void pollOverview();
        void refreshOverview(double now);
        void rebuildDerived(terrain::TileCoord tile, terrain::WorldTile& data);
        std::shared_ptr<terrain::WorldTile> decodeFile(terrain::TileCoord tile, std::string& error) const;

        Scene* scene;
        Entity entity;
        std::shared_ptr<TerrainWorldSource> worldSource;
        bool isPlaying;

        terrain::WorldManifest worldManifest;
        terrain::WorldOccupancy worldOccupancy;
        std::unique_ptr<terrain::TileIO> io;
        std::unique_ptr<terrain::TileStreamer> streamer;
        std::unique_ptr<terrain::TiledMapStore> mapStore;

        // R31: edited tiles, the writer's copies
        std::map<uint32_t, std::shared_ptr<terrain::WorldTile>> pinned;
        std::set<uint32_t> dirtyFiles, removedFiles, derivedDirty;
        std::map<uint32_t, uint32_t> revisions;

        terrain::WorldOverview overview;
        uint32_t overviewRev = 0;
        // R10: the overview arrived (or the world has no tile to draw)
        bool overviewDone = false;
        bool overviewUnsaved = false;
        bool overviewPatchPending = false;
        double overviewPatchAt = 0.0;
        jobs::Task<std::shared_ptr<terrain::WorldOverview>> overviewTask;
        bool overviewLoading = false;

        std::vector<float> layerColours;
        int layerCount = 0;

        std::vector<terrain::StreamFocus> autoFocuses;
        std::vector<ScriptFocus> scriptFocuses;
        std::vector<TerrainWorldAreaWait> areaWaits;
        int nextFocusId = 1;
        int nextWaitId = 1;

        // queries come from jobs too (particles, spline conforms): the tile tables change only under it
        mutable std::recursive_mutex lock;
        std::set<uint32_t> seamWarned;
        TerrainWorldStats statsCache;
        double lastNow = 0.0;
    };

}

#endif // TERRAIN_WORLD_RUNTIME_H
