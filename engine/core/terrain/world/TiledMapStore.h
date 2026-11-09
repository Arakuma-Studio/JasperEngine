// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TILED_MAP_STORE_H
#define TILED_MAP_STORE_H

// T4 R28-R30, D11, D12: T1's TerrainMapStore over the tiles of a Terrain World, so every op, brush,
// bake and generator edits a world as one big map. Texels are global and never negative: texel 0 is
// the addressable minimum corner (D12). A write updates every tile holding each texel in the one call
// (height samples on a border are in up to four tiles, plus gutters), so a seam is never half written.
// Tiles come from a TileAccess: the editor's cache, the runtime's streamer, or memory in tests.

#include "Export.h"
#include "terrain/TerrainMapStore.h"
#include "terrain/world/TerrainTileFormat.h"
#include "terrain/world/WorldLattice.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::terrain {

    // Where a store finds its tiles. Main thread, as the store is.
    class TileAccess {
    public:
        virtual ~TileAccess() = default;
        virtual bool present(TileCoord tile) const = 0;
        // The CPU data, loaded now if it must be; null when absent or unreadable
        virtual std::shared_ptr<const WorldTile> tile(TileCoord tile) const = 0;
        // The copy the store may change: a reader holding the old one (a job) keeps it as it was
        virtual std::shared_ptr<WorldTile> writable(TileCoord tile) = 0;
        virtual void add(TileCoord tile, std::shared_ptr<WorldTile> data) = 0;
        // An undo's snapshot back as it was, derived sections included: rebuilding them could change
        // the bytes the tile had (an undo leaves the files as they were)
        virtual void restore(TileCoord tile, std::shared_ptr<WorldTile> data) { add(tile, std::move(data)); }
        virtual void remove(TileCoord tile) = 0;
        // The tile's bytes changed (samples, gutters, maps): its derived sections, page and file follow
        virtual void changed(TileCoord tile) = 0;
        // The box of present tiles, inclusive; false when there are none
        virtual bool bounds(TileCoord& low, TileCoord& high) const = 0;
        // Whether this tile may change now (R31's cap on a game's edited tiles); asked by canWrite
        virtual bool mayWrite(TileCoord tile, std::string* why) const {
            (void)tile;
            (void)why;
            return true;
        }
    };

    // Tiles in memory: tests, the import's slicing and the runtime's edit pins all start from it
    class JASPERENGINE_API MemoryTileAccess : public TileAccess {
    public:
        bool present(TileCoord tile) const override;
        std::shared_ptr<const WorldTile> tile(TileCoord tile) const override;
        std::shared_ptr<WorldTile> writable(TileCoord tile) override;
        void add(TileCoord tile, std::shared_ptr<WorldTile> data) override;
        void remove(TileCoord tile) override;
        void changed(TileCoord tile) override;
        bool bounds(TileCoord& low, TileCoord& high) const override;

        size_t count() const { return tiles.size(); }
        std::vector<TileCoord> coords() const;
        // changed() since the last take, in key order
        std::vector<TileCoord> takeChanged();

    private:
        std::map<uint32_t, std::shared_ptr<WorldTile>> tiles;
        std::map<uint32_t, bool> changedTiles;
    };

    // What the world's maps are, beyond what a tile holds (R48's manifest)
    struct WorldMaps {
        float maxHeight = 100.0f;
        uint16_t baseHeight = 0;    // normalized 16-bit: what an absent tile reads, and a new one holds
        int blendMaps = 0;
        // per blend map, its RGBA where a tile has none
        float blendFill[8][4] = {};
        bool hasHoles = false;
        int densityMaps = 0;
    };

    struct TiledStoreOptions {
        // R29: a write over an absent tile makes it (brush, edit_terrain, generation; not Lua)
        bool autoAddTiles = false;
        // R6
        int maxBoxTiles = WORLD_MAX_BOX_DESKTOP;
        // D11: past this many texels an op runs straight on the tiles, with a snapshot for undo
        int64_t largeEditTexels = 2048ll * 2048ll;
        // R30
        uint64_t snapshotCapBytes = 2ull << 30;
    };

    // R30, D11: what a large edit would cost, for dry_run and the allow_no_undo check
    struct LargeEditPrediction {
        bool large = false;
        uint64_t snapshotBytes = 0;
        bool undoable = true;
        int tiles = 0;
    };

    class JASPERENGINE_API TiledMapStore final : public TerrainMapStore {
    public:
        TiledMapStore(const WorldLattice& lattice, const WorldMaps& maps, TileAccess& access, TiledStoreOptions options = {});

        const WorldLattice& lattice() const { return worldLattice; }
        const WorldMaps& maps() const { return worldMaps; }
        WorldMaps& maps() { return worldMaps; }
        TiledStoreOptions& options() { return storeOptions; }
        TileAccess& tiles() { return *access; }

        // The global texel of a tile's first shared sample (height), or first texel (area maps)
        int tileTexelOrigin(MapId map, int tileCoord) const;
        // The tiles a global rectangle of a map reaches, holders of shared texels included
        std::vector<TileCoord> tilesOf(MapId map, TexelRect rect) const;
        // A tile's own rectangle in a map's global texels: shared samples included, gutters not
        TexelRect tileRect(MapId map, TileCoord tile) const;

        // Add Tile (R41, R44): base height, borders from present neighbours. False past R6's limits.
        bool addTile(TileCoord tile, std::string* why = nullptr);
        // Remove Tile; neighbours' gutters go back to repeating their borders
        bool removeTile(TileCoord tile);
        // Re-reads one tile's gutter ring from its neighbours, or repeats its border where none is
        void syncGutters(TileCoord tile);
        // R51: shared samples of a tile that differ from a present neighbour's; repair resolves in
        // favour of the tile with the lower (x, z)
        int seamErrors(TileCoord tile, bool repair);

        // Whether a would-be tile fits R6: addressable, within 16,384 m and the bounding box
        bool canCreate(TileCoord tile, std::string* why) const;
        LargeEditPrediction predictLarge(MapId map, TexelRect rect) const;
        // Tiles made by writes since the last takeUndoHook
        const std::vector<TileCoord>& createdTiles() const { return created; }

        TerrainFrame frame() const override;
        MapFormat format(MapId map) const override;
        bool read(MapId map, TexelRect rect, std::span<float> out) const override;
        WriteResult write(MapId map, TexelRect rect, std::span<const float> in) override;
        bool readBytes(MapId map, TexelRect rect, std::span<uint8_t> out) const override;
        WriteResult writeBytes(MapId map, TexelRect rect, std::span<const uint8_t> in) override;
        bool canWrite(MapId map, TexelRect rect, std::string* why) const override;
        WriteResult createMap(MapId map, MapFormat format, float fill) override;
        bool canCreateMap(MapId map, std::string* why) const override;
        void takeDirty(std::vector<std::pair<MapId, TexelRect>>& out) override;
        Vector2 worldOfTexel(MapId map, int x, int y) const override;
        Vector2 texelOfWorld(MapId map, float localX, float localZ) const override;
        TexelRect preferredBlock(MapId map) const override;
        std::unique_ptr<TerrainLargeEdit> beginLarge(MapId map, TexelRect rect) override;
        std::unique_ptr<TerrainStoreUndoHook> takeUndoHook() override;

        // Used by the undo hook and the large edit's restore
        void restoreTile(TileCoord tile, std::shared_ptr<WorldTile> data);
        void dropTile(TileCoord tile);

    private:
        struct MapGeometry {
            int origin = 0;     // global texel of tile 0's first texel
            int period = 0;     // texels from one tile to the next
            int side = 0;       // texels a tile holds a side (gutter excluded)
            int lattice = 0;    // global texels a side
            bool shared = false;
            int bytesPerTexel = 1;
        };

        MapGeometry geometry(MapId map) const;
        void defaultTexel(MapId map, uint8_t* out) const;
        // The map of a tile a texel goes to; allocated with the default when allocate, else null
        TileMap* tileMap(WorldTile& tile, MapId map, bool allocate);
        const TileMap* tileMap(const WorldTile& tile, MapId map) const;
        void copyBordersFromNeighbours(WorldTile& tile, MapId map);
        std::shared_ptr<WorldTile> makeTile(TileCoord tile);
        WriteResult writeStored(MapId map, TexelRect rect, std::span<const uint8_t> in, bool allowCreate);
        void markDirty(MapId map, TexelRect rect);

        WorldLattice worldLattice;
        WorldMaps worldMaps;
        TileAccess* access;
        TiledStoreOptions storeOptions;
        std::vector<std::pair<MapId, TexelRect>> dirty;
        std::vector<TileCoord> created;
    };

}

#endif // TILED_MAP_STORE_H
