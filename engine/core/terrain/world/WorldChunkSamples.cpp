// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/world/WorldChunkSamples.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::terraincore;

bool jasperengine::terraincore::worldSquareOpen(const TileNeighbourhood& near, const WorldLattice& lattice, int sx, int sz) {
    const int n = lattice.tileQuads;
    const int dx = sx < 0 ? -1 : (sx >= n ? 1 : 0);
    const int dz = sz < 0 ? -1 : (sz >= n ? 1 : 0);
    const WorldTile* tile = near.tiles[dz + 1][dx + 1];
    // not in memory, absent, or without holes: solid
    if (!tile || tile->holes.empty()) return false;
    const int lx = sx - dx * n, lz = sz - dz * n;
    const int side = tile->holes.width;
    // the hole texels the square [lx, lx + 1] x [lz, lz + 1] overlaps (T3 R22), area texels over the tile
    const int x0 = (lx * side) / n, x1 = std::max(x0 + 1, ((lx + 1) * side + n - 1) / n);
    const int z0 = (lz * side) / n, z1 = std::max(z0 + 1, ((lz + 1) * side + n - 1) / n);
    for (int z = z0; z < z1 && z < side; z++) {
        for (int x = x0; x < x1 && x < side; x++) {
            if (tile->holes.texel(x, z)[0] >= 128) return false;
        }
    }
    return true;
}

int jasperengine::terraincore::worldChunkSamples(const TileNeighbourhood& near, const WorldLattice& lattice, float maxHeight, int chunkQuads, int chunkX,
                                             int chunkZ, std::vector<float>& out) {
    const WorldTile* tile = near.centre();
    const int side = chunkQuads + 1;
    out.assign(static_cast<size_t>(side) * side, 0.0f);
    if (!tile) return 0;
    const bool holes = !tile->holes.empty() || [&] {
        for (const auto& row : near.tiles) {
            for (const WorldTile* other : row) {
                if (other && !other->holes.empty()) return true;
            }
        }
        return false;
    }();
    int open = 0;
    for (int j = 0; j < side; j++) {
        for (int i = 0; i < side; i++) {
            const int sx = chunkX * chunkQuads + i, sz = chunkZ * chunkQuads + j;
            float value = heightOfSample(tile->sample(sx + 1, sz + 1), maxHeight);
            if (holes && worldSquareOpen(near, lattice, sx - 1, sz - 1) && worldSquareOpen(near, lattice, sx, sz - 1) &&
                worldSquareOpen(near, lattice, sx - 1, sz) && worldSquareOpen(near, lattice, sx, sz)) {
                value = WORLD_NO_COLLISION;
                open++;
            }
            out[static_cast<size_t>(j) * side + i] = value;
        }
    }
    return open;
}

bool jasperengine::terraincore::worldCoarseSamples(const WorldOverview& overview, float maxHeight, TileCoord tile, std::vector<float>& out) {
    if (!overview.covers(tile) || overview.empty()) return false;
    const int side = overview.stride + 1;
    const int originX = (tile.x - overview.low.x) * overview.stride, originZ = (tile.z - overview.low.z) * overview.stride;
    out.resize(static_cast<size_t>(side) * side);
    for (int j = 0; j < side; j++) {
        for (int i = 0; i < side; i++) {
            out[static_cast<size_t>(j) * side + i] =
                heightOfSample(overview.heights[static_cast<size_t>(originZ + j) * overview.width() + originX + i], maxHeight);
        }
    }
    return true;
}
