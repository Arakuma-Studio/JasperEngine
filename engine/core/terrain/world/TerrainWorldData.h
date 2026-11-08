// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T4 R9, R48, R52, R53: what a Terrain World is beyond its tiles. The manifest (manifest.yaml in the
// editor, world.bin in a pack, because a player parses no YAML) and the overview: the exact tile
// samples at the overview's stride over the box of present tiles, with their macro colour, drawn
// past the near band and answering queries over tiles not in memory.

#ifndef TERRAIN_WORLD_DATA_H
#define TERRAIN_WORLD_DATA_H

#include "Export.h"
#include "terrain/TerrainSurface.h"
#include "terrain/world/TerrainTileFormat.h"
#include "terrain/world/TiledMapStore.h"
#include "terrain/world/WorldLattice.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace jasperengine::terrain {

    inline constexpr uint32_t WORLD_FORMAT_VERSION = 1;
    // the header flag of an overview container
    inline constexpr uint32_t TILE_FLAG_OVERVIEW = 1u;
    inline constexpr uint32_t TILE_SECTION_OVERVIEW_INFO = tileSectionId('O', 'V', 'R', 'I');

    struct WorldTileInfo {
        TileCoord coord;
        uint16_t minHeight = 0, maxHeight = 0xFFFF;
        // R27: T5's tiles/x_z.jetrees is beside it
        bool trees = false;
    };

    struct WorldManifest {
        uint32_t formatVersion = WORLD_FORMAT_VERSION;
        std::string guid;
        WorldLattice lattice;
        WorldMaps maps;
        uint64_t layerHash = 0;
        // D5: the largest of the tiles', normalized
        float skirtDepth = 0.0f;
        // R48: the files present; world.bin carries them, the editor scans them
        std::vector<WorldTileInfo> tiles;
    };

    // world.bin (R53): little-endian, versioned, checked
    JASPERENGINE_API void encodeWorldBin(const WorldManifest& manifest, std::vector<uint8_t>& out);
    JASPERENGINE_API bool decodeWorldBin(std::span<const uint8_t> bytes, WorldManifest& out, std::string* error);

    struct WorldOverview {
        TileCoord low;
        int tilesX = 0, tilesZ = 0;
        int stride = 16;        // A
        float skirtDepth = 0.0f;
        std::vector<uint16_t> heights;  // width() x height()
        std::vector<uint8_t> colour;    // RGBA8, the same lattice

        int width() const { return stride * tilesX + 1; }
        int height() const { return stride * tilesZ + 1; }
        bool empty() const { return heights.empty(); }
        bool covers(TileCoord tile) const {
            return tile.x >= low.x && tile.z >= low.z && tile.x < low.x + tilesX && tile.z < low.z + tilesZ;
        }
        size_t bytes() const { return heights.size() * 2 + colour.size(); }
    };

    // R34: the largest overview side a platform takes
    inline constexpr int WORLD_OVERVIEW_MAX_DESKTOP = 4097;
    inline constexpr int WORLD_OVERVIEW_MAX_MOBILE = 2049;

    // A box from low to high inclusive, its samples at the base height and its colour mid grey
    JASPERENGINE_API void initOverview(WorldOverview& overview, const WorldLattice& lattice, TileCoord low, TileCoord high, uint16_t baseHeight);
    // One tile's patch from its OVRH and OVRC; false when the tile is outside the box or has none
    JASPERENGINE_API bool setOverviewPatch(WorldOverview& overview, const WorldTile& tile);
    // Height in metres at an entity-local position over a present tile, by the overview's bilinear
    // (what an overview draw shows), and its normal
    JASPERENGINE_API SurfaceSample sampleOverview(const WorldOverview& overview, const WorldLattice& lattice, float maxHeight, float localX, float localZ);

    JASPERENGINE_API bool encodeOverview(const WorldOverview& overview, const WorldLattice& lattice, const TileCodec& codec, int level,
                                         std::vector<uint8_t>& out, std::string* error);
    JASPERENGINE_API TileError decodeOverview(std::span<const uint8_t> file, const TileCodec& codec, WorldOverview& out);

    // R47, R52: a manifest's layer hash over the layers' colours (what the macro colour depends on)
    JASPERENGINE_API uint64_t layerColourHash(std::span<const float> colours);

    // R34's defaults by platform
    struct WorldPlatformBudget {
        int maxTileQuads = 1024;
        int poolSize = 64;
        uint64_t gpuCapBytes = 128ull << 20;
        uint64_t cpuCapBytes = 96ull << 20;
        int nearTiles = 16;
        uint64_t uploadBytes = 8ull << 20;
        int inFlight = 4;
        float lodDistance = 1.0f;
        float collisionRadius = 128.0f;
        int overviewMaxSide = WORLD_OVERVIEW_MAX_DESKTOP;
        int maxBoxTiles = WORLD_MAX_BOX_DESKTOP;
    };
    enum class WorldPlatform : uint8_t { Desktop, Mobile, Web, WebSingleThread };
    JASPERENGINE_API WorldPlatformBudget worldPlatformBudget(WorldPlatform platform);

}

#endif // TERRAIN_WORLD_DATA_H
