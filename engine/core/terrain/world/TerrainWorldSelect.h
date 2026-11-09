// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_WORLD_SELECT_H
#define TERRAIN_WORLD_SELECT_H

// T4 R7-R10, R36, D4: CDLOD selection over a whole Terrain World as one implicit quadtree whose level
// T node is exactly a tile. Nothing is built per world, and the walk visits only nodes in range or on
// the frustum, never every tile. Near tiles draw from their own page, at most D of them, nearest
// first; everything else, and any present tile whose page is missing, draws from the one overview.

#include "Export.h"
#include "terrain/world/TerrainTileFormat.h"
#include "terrain/world/WorldLattice.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace jasperengine::terraincore {

    // Which tiles are present, with their min and max, as a dense pyramid over the addressable grid
    // (D4's world min/max pyramid above T). Level 0 is a tile.
    class JASPERENGINE_API WorldOccupancy {
    public:
        static constexpr int LEVELS = 10;   // 512 tiles a side down to 1

        void setTile(TileCoord tile, bool present, uint16_t minHeight = 0, uint16_t maxHeight = 0xFFFF);
        void clear();
        bool present(TileCoord tile) const;
        // present tiles under the node of this level whose minimum tile is (tileX, tileZ)
        uint32_t count(int level, int tileX, int tileZ) const;
        TileMinMax minMax(int level, int tileX, int tileZ) const;
        int tileCount() const { return tiles; }
        bool bounds(TileCoord& low, TileCoord& high) const;
        std::vector<TileCoord> presentTiles() const;

    private:
        size_t index(int level, int tileX, int tileZ) const;
        void refresh(int level, int tileX, int tileZ);

        std::vector<uint32_t> counts;
        std::vector<uint16_t> mins, maxs;
        int tiles = 0;
    };

    struct WorldSelectNode {
        float x = 0.0f, z = 0.0f;   // centre, entity-local
        float size = 0.0f;
        float range = 0.0f;         // WORLD_INFINITE_RANGE: no morph
        float resolution = 0.0f;    // R for a full grid, R / 2 for a half
        int level = 0;
    };

    inline constexpr float WORLD_INFINITE_RANGE = 1.0e30f;

    struct WorldTileDraws {
        TileCoord tile;
        std::vector<WorldSelectNode> full, half;
    };

    struct WorldSelection {
        std::vector<WorldTileDraws> near;               // at most D, nearest first
        std::vector<WorldSelectNode> overviewFull, overviewHalf;
        int visited = 0;
        int fellBack = 0;           // tiles that wanted a page and drew the overview (R10)
        int missingPages = 0;       // of those, the ones whose page was not drawable
        bool overflow = false;      // a grid reached maxNodesPerGrid
        int drawItems() const;
        int selectedNodes() const;
        void clear();
    };

    struct WorldSelectParams {
        WorldLattice lattice;
        float maxHeight = 100.0f;
        float lodDistance = 1.0f;
        float farClip = 1000.0f;
        float eyeX = 0.0f, eyeY = 0.0f, eyeZ = 0.0f;   // entity-local
        // inside when a x + b y + c z + d >= 0; an all-zero plane is skipped (no far plane)
        float planes[6][4] = {};
        int maxNearTiles = 16;      // D
        int maxNodesPerGrid = 4096;
        // the tile's resident data when its page can draw, else null (R10)
        std::function<const WorldTile*(TileCoord)> page;
    };

    // r_L = 2 size_L lodDistance; the root's range reaches the far clip
    JASPERENGINE_API float worldLevelRange(const WorldSelectParams& params, int level);
    JASPERENGINE_API void selectWorld(const WorldSelectParams& params, const WorldOccupancy& occupancy, WorldSelection& out);

}

#endif // TERRAIN_WORLD_SELECT_H
