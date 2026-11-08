// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TILE_STREAMER_H
#define TILE_STREAMER_H

// T4 R8, R13-R16, R34: which tiles of a Terrain World are wanted, loading, in memory and on the GPU.
// Pure: the clock comes in through update(now), reading and decoding through a TileIO, and uploads
// are planned here and done by the renderer, which reports back. A cancelled tile goes back to Cold
// and is queued again when wanted (the godot_voxel hole: AC15); a failed read retries three times.

#include "Export.h"
#include "terrain/world/TerrainTileFormat.h"
#include "terrain/world/TerrainWorldSelect.h"
#include "terrain/world/WorldLattice.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::terrain {

    enum class TileState : uint8_t { Absent, Cold, Queued, Loading, CpuReady, Uploading, Resident, Error };
    JASPERENGINE_API const char* tileStateName(TileState state);

    // R16: mapped onto AssetIOService's priorities by the caller
    enum class TilePriority : uint8_t { Normal = 0, High = 1, Critical = 2 };

    enum class TileLoadStatus : uint8_t { Pending, Done, Failed };

    // Reads and decodes one tile; polled, never blocking (R17)
    class TileIO {
    public:
        virtual ~TileIO() = default;
        virtual uint64_t begin(TileCoord tile, TilePriority priority) = 0;
        // Done fills data; Failed fills error
        virtual TileLoadStatus poll(uint64_t handle, std::shared_ptr<WorldTile>& data, std::string& error) = 0;
        virtual void cancel(uint64_t handle) = 0;
    };

    struct StreamBudgets {
        int poolSize = 64;
        uint64_t gpuCapBytes = 128ull << 20;
        uint64_t cpuCapBytes = 96ull << 20;
        uint64_t uploadBytesPerFrame = 8ull << 20;
        int maxInFlight = 4;
        float lodDistance = 1.0f;
        // a headless player: tiles stop at CpuReady (R15)
        bool gpu = true;
    };

    // R15
    struct StreamFocus {
        int id = 0;
        float x = 0.0f, z = 0.0f;
        float radiusScale = 1.0f;
        float velocityX = 0.0f, velocityZ = 0.0f;
    };

    struct TileStreamStats {
        int states[8] = {};
        int inFlight = 0;
        int queued = 0;
        uint64_t cpuBytes = 0, gpuBytes = 0;
        int residentPages = 0;
        uint64_t uploadBytesLastFrame = 0, uploadBytesTotal = 0;
        int uploadRetries = 0;
        int poolExhausted = 0;      // tiles that wanted a page the pool could not give, last plan
        int errors = 0;
        int loaded = 0, evictedPages = 0, evictedCpu = 0, cancelled = 0;
    };

    struct TileStreamEvent {
        enum class Kind : uint8_t { CpuReady, PageEvicted, CpuEvicted, Error, Cancelled };
        Kind kind = Kind::CpuReady;
        TileCoord tile;
        std::string message;
    };

    class JASPERENGINE_API TileStreamer {
    public:
        static constexpr double RESORT_SECONDS = 0.1;
        static constexpr double PREDICT_SECONDS = 1.5;
        static constexpr int MAX_RETRIES = 3;

        TileStreamer(TileIO& io, const WorldOccupancy& occupancy);

        void setLattice(const WorldLattice& lattice) { worldLattice = lattice; }
        const WorldLattice& lattice() const { return worldLattice; }
        StreamBudgets& budgets() { return limits; }
        const StreamBudgets& budgets() const { return limits; }

        void setFocuses(const std::vector<StreamFocus>& focuses);
        const std::vector<StreamFocus>& focuses() const { return focusList; }
        // Wanted until the next re-sort: collision subjects, pending areas (R18, R22)
        void demand(TileCoord tile, TilePriority priority);
        // R8, R31: an edited tile keeps its data and page
        void pin(TileCoord tile, bool pinned);
        bool pinned(TileCoord tile) const;
        // R45: no load starts, nothing is evicted and nothing uploads while frozen
        void setFrozen(bool frozen) { isFrozen = frozen; }
        bool frozen() const { return isFrozen; }

        // Polls loads, re-sorts every RESORT_SECONDS, starts and cancels loads, evicts (D9's pump)
        void update(double now);
        // This frame's uploads, priority first, within the byte budget but always one (R13)
        std::vector<TileCoord> planUploads();
        void uploaded(TileCoord tile, uint64_t gpuBytes);
        // R13: a refused upload keeps its data and goes again next frame
        void uploadRefused(TileCoord tile);

        TileState state(TileCoord tile) const;
        std::shared_ptr<const WorldTile> data(TileCoord tile) const;
        bool wanted(TileCoord tile) const;
        bool pageStale(TileCoord tile) const;
        // An edit replaced the tile's data: it stays drawable on its old page until the new one is up
        void replace(TileCoord tile, std::shared_ptr<const WorldTile> data);
        // The file changed (R14, R31): back to Cold, an Error cleared
        void invalidate(TileCoord tile);
        // The tile was removed from the world
        void forget(TileCoord tile);

        std::vector<TileStreamEvent> takeEvents();
        const TileStreamStats& stats() const { return counters; }
        uint64_t pageBytes(const WorldTile& tile) const;
        // Tiles with a record, in key order: for the panel and sample_terrain
        std::vector<TileCoord> knownTiles() const;

    private:
        struct Record {
            TileState state = TileState::Cold;
            std::shared_ptr<const WorldTile> data;
            uint64_t handle = 0;
            TilePriority priority = TilePriority::Normal;
            float distance = 0.0f;
            double lastWanted = -1.0;
            double retryAt = 0.0;
            int failures = 0;
            uint64_t cpuBytes = 0, gpuBytes = 0;
            bool page = false;
            bool stale = false;
            bool pinned = false;
            bool wanted = false;
            bool focusTile = false;
            bool errorLogged = false;
        };

        void resort(double now);
        void pollLoads(double now);
        void startLoads(double now);
        void cancelFarLoads();
        void evict();
        void recount();
        float bandRadius(const StreamFocus& focus) const;
        float distanceToTile(float x, float z, TileCoord tile) const;
        void want(TileCoord tile, TilePriority priority, float distance, bool focusTile, double now);
        Record* find(TileCoord tile);
        const Record* find(TileCoord tile) const;

        TileIO& io;
        const WorldOccupancy& occupancy;
        WorldLattice worldLattice;
        StreamBudgets limits;
        std::vector<StreamFocus> focusList;
        std::vector<std::pair<TileCoord, TilePriority>> demands;
        std::map<uint32_t, Record> records;
        std::vector<TileStreamEvent> events;
        TileStreamStats counters;
        double lastResort = -1.0e9;
        bool isFrozen = false;
        bool focusChanged = true;
    };

}

#endif // TILE_STREAMER_H
