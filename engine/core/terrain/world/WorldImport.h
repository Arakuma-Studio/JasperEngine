// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WORLD_IMPORT_H
#define WORLD_IMPORT_H

// T4 R32: a heightmap imported into a Terrain World's tiles by reading bands of rows, so memory stays
// within one band of N + 3 image rows and a few tiles whatever the image's size. Sample (i, j) of the
// image lands on the world's sample (origin.x * N + i, origin.z * N + j); past its last row or column
// the edge repeats over the tiles it covers. Every tile the import changes is written whole: the tiles
// it covers (made at the base height when absent) and their present neighbours, whose shared samples
// and gutters change too, as T4 R28's store would leave them.

#include "Export.h"
#include "terrain/world/TerrainTileFormat.h"
#include "terrain/world/WorldLattice.h"

#include <cstdint>
#include <functional>
#include <memory>
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

    // What an import composes with: the world as it is (read from the import's job: the editor reads
    // the tile files) and where each changed tile goes
    struct ImportTarget {
        std::function<bool(TileCoord tile)> present;
        // the tile now; null when it cannot be read (the import fails)
        std::function<std::shared_ptr<WorldTile>(TileCoord tile)> read;
        // the tile as the import leaves it, derived sections built; false stops the import
        std::function<bool(const WorldTile& tile)> write;
        // what a tile the import makes holds before the image's samples
        uint16_t baseHeight = 0;
        // the macro colour's layer colours (buildOverviewColour); empty: no macro colour
        std::vector<float> layerColours;
        int layerCount = 0;
    };

    // The tiles a width x height image makes at this lattice
    JASPERENGINE_API int importTilesAcross(int samples, int tileQuads);

    // The tiles an import covers, row by row
    JASPERENGINE_API std::vector<TileCoord> importCoveredTiles(const HeightmapImport& plan, int width, int height);
    // Every tile it changes, in the order importHeightmap writes them: the covered ones and the present
    // tiles round them
    JASPERENGINE_API std::vector<TileCoord> importChangedTiles(const HeightmapImport& plan, int width, int height,
                                                               const std::function<bool(TileCoord tile)>& present);

    // Writes each changed tile, tile row by tile row; cancelled() true stops between tiles. progress
    // gets (tiles written, tiles to write). False with error on a read failure, a refused write, a
    // tile past the world's limit or a cancel ("cancelled").
    JASPERENGINE_API bool importHeightmap(const HeightmapRows& source, const HeightmapImport& plan, const ImportTarget& target,
                                          const std::function<bool()>& cancelled, const std::function<void(int done, int total)>& progress,
                                          std::string& error);

    // A 16-bit RAW or R16 file, read a row at a time
    struct RawHeightmapFile {
        std::string path;
        int width = 0, height = 0;
        bool bigEndian = false;
        // the stored values mapped to 0..65535 (T1's Auto range is the data's own minimum and maximum)
        double low = 0.0, high = 65535.0;
    };

    // One pass over a RAW file, a row at a time: its range in each byte order and how rough each
    // order reads (T1's guess: real terrain is smooth, the wrong order is noise)
    struct RawHeightmapScan {
        uint16_t minLittle = 0, maxLittle = 0, minBig = 0, maxBig = 0;
        double roughLittle = 0.0, roughBig = 0.0;
        bool bigEndian() const { return roughBig < roughLittle; }
    };
    JASPERENGINE_API bool scanRawHeightmap(const std::string& path, int width, int height, const std::function<bool()>& cancelled,
                                           RawHeightmapScan& out, std::string& error);
    // The rows of a RAW file; the file stays open while the returned rows live
    JASPERENGINE_API HeightmapRows rawHeightmapRows(const RawHeightmapFile& file);

}

#endif // WORLD_IMPORT_H
