// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WORLD_CHUNK_SAMPLES_H
#define WORLD_CHUNK_SAMPLES_H

// T4 R20, R22, R23: the samples of one collision chunk of a Terrain World, straight from its tile
// with no resampling, and those of a coarse tile from the overview. Holes follow T3 R22: a sample has
// no collision when every square around it is open, and a square of a tile that is not resident
// counts as solid, so the collider never opens under drawn ground. No Jolt here: the caller hands
// the floats to a height field (NO_COLLISION is Jolt's cNoCollisionValue).

#include "Export.h"
#include "terrain/world/TerrainTileFormat.h"
#include "terrain/world/TerrainWorldData.h"
#include "terrain/world/WorldLattice.h"

#include <cfloat>
#include <vector>

namespace jasperengine::terrain {

    inline constexpr float WORLD_NO_COLLISION = FLT_MAX;

    // The tile and its eight neighbours, [dz + 1][dx + 1]; null where absent or not in memory
    struct TileNeighbourhood {
        const WorldTile* tiles[3][3] = {};
        const WorldTile* centre() const { return tiles[1][1]; }
    };

    // Whether the lattice square whose low corner is the tile's local sample (sx, sz) is open: every
    // hole texel it overlaps is a hole. Local samples may be -1 or N, in the neighbours.
    JASPERENGINE_API bool worldSquareOpen(const TileNeighbourhood& near, const WorldLattice& lattice, int sx, int sz);

    // (chunkQuads + 1)^2 heights in metres, row by row along +x then +z, from the chunk's low
    // corner; returns how many are WORLD_NO_COLLISION
    JASPERENGINE_API int worldChunkSamples(const TileNeighbourhood& near, const WorldLattice& lattice, float maxHeight, int chunkQuads,
                                           int chunkX, int chunkZ, std::vector<float>& out);

    // R22: a whole tile's coarse samples, (A + 1)^2, from the overview; holes ignored as drawn
    JASPERENGINE_API bool worldCoarseSamples(const WorldOverview& overview, float maxHeight, TileCoord tile, std::vector<float>& out);

}

#endif // WORLD_CHUNK_SAMPLES_H
