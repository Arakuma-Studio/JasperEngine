// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/world/WorldImport.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::terrain;

int jasperengine::terrain::importTilesAcross(int samples, int tileQuads) {
    if (samples < 2 || tileQuads <= 0) return 0;
    // a tile needs its last shared sample: (samples - 1) quads, a partial tile repeating the edge
    return (samples - 1 + tileQuads - 1) / tileQuads;
}

bool jasperengine::terrain::sliceHeightmap(const HeightmapRows& source, const HeightmapImport& plan, const std::function<bool(WorldTile& tile)>& emit,
                                           const std::function<bool()>& cancelled, std::string& error) {
    const WorldLattice& lattice = plan.lattice;
    if (!validateLattice(lattice, &error)) return false;
    if (source.width < 2 || source.height < 2 || !source.read) {
        error = "the heightmap is empty";
        return false;
    }
    const int n = lattice.tileQuads;
    const int across = importTilesAcross(source.width, n), down = importTilesAcross(source.height, n);
    for (int corner = 0; corner < 4; corner++) {
        const TileCoord tile(plan.origin.x + (corner % 2) * (across - 1), plan.origin.z + (corner / 2) * (down - 1));
        if (!tileWithinLimit(lattice, tile)) {
            error = "tile (" + std::to_string(tile.x) + ", " + std::to_string(tile.z) + ") is past the world's limit of +-16384 m";
            return false;
        }
    }

    // the band a row of tiles reads: its N + 1 shared rows and a gutter row either side, so at most
    // N + 3 image rows are held at once
    std::vector<std::vector<uint16_t>> band(static_cast<size_t>(n) + 3);
    std::vector<int> bandRow(band.size(), -1);
    auto imageRow = [&](int worldRow) {
        // world sample row (within the import) to image row, clamped: the edge repeats
        const int clamped = std::min(std::max(worldRow, 0), source.height - 1);
        return plan.flipZ ? source.height - 1 - clamped : clamped;
    };
    auto row = [&](int worldRow) -> const std::vector<uint16_t>* {
        const int wanted = imageRow(worldRow);
        const size_t slot = static_cast<size_t>(((worldRow % static_cast<int>(band.size())) + static_cast<int>(band.size())) % static_cast<int>(band.size()));
        if (bandRow[slot] != wanted) {
            band[slot].assign(static_cast<size_t>(source.width), 0);
            if (!source.read(wanted, band[slot]) || band[slot].size() != static_cast<size_t>(source.width)) {
                error = "the heightmap's row " + std::to_string(wanted) + " could not be read";
                return nullptr;
            }
            bandRow[slot] = wanted;
        }
        return &band[slot];
    };
    for (int tz = 0; tz < down; tz++) {
        for (int tx = 0; tx < across; tx++) {
            if (cancelled && cancelled()) {
                error = "cancelled";
                return false;
            }
            WorldTile tile;
            initTile(tile, lattice, TileCoord(plan.origin.x + tx, plan.origin.z + tz), 0);
            for (int j = 0; j < n + 3; j++) {
                const std::vector<uint16_t>* samples = row(tz * n + j - 1);
                if (!samples) return false;
                for (int i = 0; i < n + 3; i++) {
                    const int column = std::min(std::max(tx * n + i - 1, 0), source.width - 1);
                    tile.setSample(i, j, (*samples)[static_cast<size_t>(column)]);
                }
            }
            buildMinMax(tile, lattice);
            buildOverviewHeight(tile, lattice);
            buildSkirtDepth(tile, lattice);
            if (!emit(tile)) {
                if (error.empty()) error = "a tile could not be written";
                return false;
            }
        }
    }
    return true;
}
