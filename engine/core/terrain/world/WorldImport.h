// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WORLD_IMPORT_H
#define WORLD_IMPORT_H

// T4 R32: a heightmap sliced into a Terrain World's tiles by reading bands of rows, so memory stays
// within a couple of tile rows whatever the image's size. Sample (i, j) of the image lands on the
// world's sample (origin.x * N + i, origin.z * N + j); past its last row or column the edge repeats.

#include "Export.h"
#include "terrain/world/TerrainTileFormat.h"
#include "terrain/world/WorldLattice.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::terrain {

    struct HeightmapRows {
        int width = 0, height = 0;
        // one row of 16-bit samples, top first; false stops the import
        std::function<bool(int row, std::vector<uint16_t>& out)> read;
    };

    struct HeightmapImport {
        WorldLattice lattice;
        TileCoord origin;
        // the image's first row is the world's highest z (the usual top-down picture)
        bool flipZ = false;
    };

    // The tiles a width x height image makes at this lattice
    JASPERENGINE_API int importTilesAcross(int samples, int tileQuads);

    // Calls emit with each tile, row of tiles by row of tiles; cancelled() true stops between tiles.
    // False with error on a read failure, a refused emit or a cancel.
    JASPERENGINE_API bool sliceHeightmap(const HeightmapRows& source, const HeightmapImport& plan, const std::function<bool(WorldTile& tile)>& emit,
                                         const std::function<bool()>& cancelled, std::string& error);

}

#endif // WORLD_IMPORT_H
