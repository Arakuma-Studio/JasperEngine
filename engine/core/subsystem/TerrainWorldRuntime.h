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
        virtual bool manifest(terraincore::WorldManifest& out, std::string& error) = 0;
        // A path AssetIOService and FileData read (a loose file or a pack entry)
        virtual std::string tilePath(terraincore::TileCoord tile) const = 0;
        // overview.jtile, or empty: the runtime assembles it from the tiles (the editor's stale cache)
        virtual std::string overviewPath() const = 0;
        // R27: T5's tiles/x_z.jetrees beside the tile (the manifest says whether it is there)
        virtual std::string treePath(terraincore::TileCoord tile) const;
        // The project folder, which the editor writes: Play never writes (R31)
        virtual bool editable() const { return false; }
    };

    // R53: a player's world, from the pack: <folder>/world.bin, overview.jtile, tiles/x_z.jtile
    class JASPERENGINE_API CookedWorldSource final : public TerrainWorldSource {
    public:
        explicit CookedWorldSource(std::string folder): folder(std::move(folder)) {}
        bool manifest(terraincore::WorldManifest& out, std::string& error) override;
        std::string tilePath(terraincore::TileCoord tile) const override;
        std::string overviewPath() const override;

    private:
        std::string folder;
    };

    // tiles/x_z.jtile under a world's folder
    JASPERENGINE_API std::string terrainWorldTileName(terraincore::TileCoord tile);
    // R27: tiles/x_z.jetrees, T5's trees of that tile
    JASPERENGINE_API std::string terrainWorldTreeName(terraincore::TileCoord tile);
    // zstd's decompressor: every player has it (D7)
    JASPERENGINE_API terraincore::TileCodec terrainWorldDecodeCodec();
    // R34's defaults for the platform this build runs on
    JASPERENGINE_API terraincore::WorldPlatformBudget terrainWorldPlatformBudget();

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

    // R42's LOD Level overlay: a node the main view drew last, entity-local; overview: drawn from the
    // overview rather than the tile's page
    struct TerrainWorldDrawnNode {
        float x = 0.0f, z = 0.0f, size = 0.0f;
        int level = 0;
        bool overview = false;
    };

    // R26: a world's heights for a job, apart from the runtime (which may go while the job runs): the
    // tiles in memory when it was made, the rest read from the source when the job asks (it may wait)
    class JASPERENGINE_API TerrainWorldHeightReader {
    public:
        // Local heights in metres of the world samples [x0, x0 + countX) x [z0, z0 + countZ) (sample s
        // sits at s * spacing); an absent tile, or one that cannot be read, reads the base height
        void read(int x0, int z0, int countX, int countZ, std::vector<float>& out);
        const terraincore::WorldLattice& lattice() const { return worldLattice; }

    private:
        friend class TerrainWorldRuntime;
        std::shared_ptr<const terraincore::WorldTile> tile(terraincore::TileCoord coord);

        terraincore::WorldLattice worldLattice;
        float maxHeight = 0.0f;
        uint16_t baseHeight = 0;
        std::shared_ptr<TerrainWorldSource> source;
        std::set<uint32_t> present;
        std::map<uint32_t, std::shared_ptr<const terraincore::WorldTile>> tiles;
    };

    struct TerrainWorldAreaWait {
        int id = 0;
        float x = 0.0f, z = 0.0f, radius = 0.0f;
        TerrainWorldReadyLevel level = TerrainWorldReadyLevel::Render;
    };

    class JASPERENGINE_API TerrainWorldRuntime final : public terraincore::TileAccess {
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
        const terraincore::WorldManifest& manifest() const { return worldManifest; }
        terraincore::WorldManifest& manifest() { return worldManifest; }
        const terraincore::WorldLattice& lattice() const { return worldManifest.lattice; }
        float maxHeight() const { return worldManifest.maps.maxHeight; }
        const terraincore::WorldOccupancy& occupancy() const { return worldOccupancy; }
        terraincore::TileStreamer& tiles() { return *streamer; }
        const terraincore::TileStreamer& tiles() const { return *streamer; }
        terraincore::TiledMapStore& store() { return *mapStore; }
        const terraincore::WorldOverview& getOverview() const { return overview; }
        // bumped when the overview's samples or colours change: render makes its textures again
        uint32_t overviewRevision() const { return overviewRev; }
        bool playing() const { return isPlaying; }
        TerrainWorldSource& source() { return *worldSource; }

        // D9: the pump, once a frame, paused or not
        void pump(double now);

        // R15, R16: focuses. The system sets the automatic ones (camera, the component's) each frame.
        void setAutoFocuses(const std::vector<terraincore::StreamFocus>& focuses);
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
        std::vector<terraincore::TileCoord> missingTiles(float x, float z, float radius, TerrainWorldReadyLevel level) const;

        // R19, R24. mayWait: a job or the editor may read a cold tile now; the game's main thread
        // answers from the overview with exact false
        terraincore::SurfaceSample query(float localX, float localZ, bool mayWait);
        bool raycast(const Vector3& origin, const Vector3& direction, float maxT, bool skipHoles, bool mayWait, Vector3& point, float& t);
        TerrainWorldPointState pointState(float localX, float localZ) const;
        terraincore::TileState tileState(terraincore::TileCoord tile) const;
        // The data of a tile in memory, edited or streamed; null otherwise
        std::shared_ptr<const terraincore::WorldTile> resident(terraincore::TileCoord tile) const;
        // Reads and decodes a present tile now (editor, jobs, ensureCollisionAt)
        std::shared_ptr<const terraincore::WorldTile> loadNow(terraincore::TileCoord tile);

        // The derived sections (min/max, overview samples and colour, skirt) of edited tiles now, not at
        // the next pump: before a flush writes them
        void settleEdits();
        // R31: what the editor flushes: tiles changed or made since the last take, and removed ones
        std::vector<terraincore::TileCoord> takeDirtyFiles();
        std::vector<terraincore::TileCoord> takeRemovedFiles();
        bool hasDirtyFiles() const { return !dirtyFiles.empty() || !removedFiles.empty(); }
        // A flushed tile no longer needs its pin (the editor keeps its cache under the caps)
        void releaseFlushed(terraincore::TileCoord tile);
        // R31: an external change (git pull): reload it
        void fileChanged(terraincore::TileCoord tile, bool exists);
        // R51: the seams of a tile against its resident neighbours; repair favours the lower (x, z)
        int checkSeams(terraincore::TileCoord tile, bool repair);
        // R52: the overview from every tile again (Rebuild Overview); a job unless wait
        void rebuildOverview(bool wait);
        // Whether the overview changed since it was last saved, for the editor's cache
        bool overviewNeedsSave() const { return overviewUnsaved; }
        void overviewSaved() { overviewUnsaved = false; }
        // R52: the layers' colours for the macro colour (base first, RGBA floats)
        void setLayerColours(std::vector<float> colours, int layerCount);
        const std::vector<float>& getLayerColours() const { return layerColours; }
        int getLayerCount() const { return layerCount; }
        // R42: the tiles under the editor's brush stay in memory while it hovers them (the brush sets
        // them each frame; a pump past HOVER_SECONDS without one lets them go); an edited tile keeps
        // its own pin
        static constexpr double HOVER_SECONDS = 0.5;
        void setHoverTiles(const std::vector<terraincore::TileCoord>& tiles);
        bool hovered(terraincore::TileCoord tile) const { return hoverPins.count(tile.key()) > 0; }
        // R42's LOD Level overlay: render keeps the main view's nodes only while this is on
        void recordDrawnNodes(bool on);
        bool recordingDrawnNodes() const { return recordNodes; }
        void setDrawnNodes(std::vector<TerrainWorldDrawnNode> nodes) { drawn = std::move(nodes); }
        const std::vector<TerrainWorldDrawnNode>& drawnNodes() const { return drawn; }
        // R26: the heights of tiles low..high for a job
        std::shared_ptr<TerrainWorldHeightReader> heightReader(terraincore::TileCoord low, terraincore::TileCoord high) const;

        // The budgets from the component, with R34's defaults
        void applySettings(int poolSize, float lodDistance, float uploadBudgetMiB, int maxNearTiles);
        void setFrozen(bool frozen);
        bool frozen() const { return streamer && streamer->frozen(); }

        TerrainWorldStats stats() const;
        TerrainWorldStats& mutableStats() { return statsCache; }
        // R10's late pages: a tile the selection wanted near that had no page (render reports them)
        void noteLatePages(int count) { statsCache.latePages += count; }

        // TileAccess, for the store: present tiles load now when they must
        bool present(terraincore::TileCoord tile) const override;
        std::shared_ptr<const terraincore::WorldTile> tile(terraincore::TileCoord tile) const override;
        std::shared_ptr<terraincore::WorldTile> writable(terraincore::TileCoord tile) override;
        void add(terraincore::TileCoord tile, std::shared_ptr<terraincore::WorldTile> data) override;
        void restore(terraincore::TileCoord tile, std::shared_ptr<terraincore::WorldTile> data) override;
        void remove(terraincore::TileCoord tile) override;
        void changed(terraincore::TileCoord tile) override;
        bool bounds(terraincore::TileCoord& low, terraincore::TileCoord& high) const override;
        bool mayWrite(terraincore::TileCoord tile, std::string* why) const override;

        // What render and collision hang on the runtime (their own files)
        std::unique_ptr<TerrainWorldRender> render;
        std::unique_ptr<TerrainWorldCollision> collision;

        // Goes up each time a tile's data changes in memory (an edit, a reload): collision rebuilds
        // what it built from an older one
        uint32_t tileRevision(terraincore::TileCoord tile) const;

    private:
        struct ScriptFocus {
            int id = 0;
            terraincore::StreamFocus focus;
        };

        void startOverviewLoad();
        void pollOverview();
        void refreshOverview(double now);
        void rebuildDerived(terraincore::TileCoord tile, terraincore::WorldTile& data);
        std::shared_ptr<terraincore::WorldTile> decodeFile(terraincore::TileCoord tile, std::string& error) const;

        Scene* scene;
        Entity entity;
        std::shared_ptr<TerrainWorldSource> worldSource;
        bool isPlaying;

        terraincore::WorldManifest worldManifest;
        terraincore::WorldOccupancy worldOccupancy;
        std::unique_ptr<terraincore::TileIO> io;
        std::unique_ptr<terraincore::TileStreamer> streamer;
        std::unique_ptr<terraincore::TiledMapStore> mapStore;

        // R31: edited tiles, the writer's copies
        std::map<uint32_t, std::shared_ptr<terraincore::WorldTile>> pinned;
        std::set<uint32_t> dirtyFiles, removedFiles, derivedDirty;
        std::map<uint32_t, uint32_t> revisions;

        terraincore::WorldOverview overview;
        uint32_t overviewRev = 0;
        // R10: the overview arrived (or the world has no tile to draw)
        bool overviewDone = false;
        bool overviewUnsaved = false;
        bool overviewPatchPending = false;
        double overviewPatchAt = 0.0;
        jobs::Task<std::shared_ptr<terraincore::WorldOverview>> overviewTask;
        bool overviewLoading = false;

        std::vector<float> layerColours;
        int layerCount = 0;
        std::set<uint32_t> hoverPins;
        double hoverAt = 0.0;
        bool recordNodes = false;
        std::vector<TerrainWorldDrawnNode> drawn;

        std::vector<terraincore::StreamFocus> autoFocuses;
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
