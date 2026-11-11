// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// What a tile map importer found (docs/specs/S50-grid-tilemap.md R44-R48, D11): the pure result of
// parsing a map file, with nothing written and no engine state touched. Apply turns it into
// .tileset files and GameObjects; the preview (R46) reports its counts.
//
// Cells are in the engine's space: y up, row 0 the bottom row, so a layer's anchors from
// tilegrid::cellToLocal land on the source map's drawn positions (up to one translation per map).
// Values are D3's: tile id + 1 and the flip bits, terrain 0 for none.

#ifndef TILEMAP_IMPORT_PLAN_H
#define TILEMAP_IMPORT_PLAN_H

#include "tilemap/TileGridLayout.h"
#include "tilemap/TileTypes.h"
#include "tilemap/Tileset.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor::tilemapimport {

    // Reads a file next to the map, by a path relative to the map's directory; false when it
    // cannot be read. The caller decides what a map may reach (it holds the project's root)
    using FileReader = std::function<bool(const std::string& path, std::string& out)>;

    // Why a parse failed. Typed as the MCP errors (R79): kind "parse" carries a JSON path in
    // `field` or a line number in `line` (with the `field` "line N"), kind "tileset_missing" names
    // the external file it could not read
    struct ImportProblem {
        std::string kind = "parse";
        std::string field;
        std::string message;
        std::string fix;
        std::string file;
        int line = 0;
    };

    struct ImportCell {
        int32_t x = 0;
        int32_t y = 0;
        uint32_t value = 0;        // D3's cell value; 0: no tile (a terrain-only cell)
        uint8_t terrain = 0;       // D3's terrain plane value: 0 none, else global terrain index + 1
        bool operator==(const ImportCell& o) const { return x == o.x && y == o.y && value == o.value && terrain == o.terrain; }
    };

    struct ImportLayer {
        std::string name;          // "Group/Layer"
        int tileset = -1;          // index in ImportPlan::tilesets; -1: an empty layer of no tileset
        std::vector<ImportCell> cells;   // sorted by (y, x)
        bool visible = true;
        float opacity = 1.0f;
        double offsetX = 0.0;      // pixels, y up
        double offsetY = 0.0;
        int chunks = 0;            // the source file's chunks (infinite maps); 0 when finite
    };

    // A Grid and its layers, back to front
    struct ImportGrid {
        std::string name;
        TileGridSpec spec;
        double originX = 0.0;      // where the Grid sits, pixels, y up (LDtk's world position)
        double originY = 0.0;
        std::vector<ImportLayer> layers;
    };

    struct ImportImage {
        int source = 0;            // TilesetSource::id
        std::string path;          // relative to the map's directory, normalized
        int width = 0;             // from the file; 0 when it does not say
        int height = 0;
    };

    struct ImportTileset {
        TilesetData data;          // sources' assets hold the images' paths (ImportImage::path)
        std::string externalFile;  // the .tsj/.tsx it came from, relative to the map's directory; "" when embedded
        std::vector<ImportImage> images;
    };

    // What the map holds and the importer does not make (R46)
    struct ImportSkipped {
        std::string kind;          // "object_layer", "image_layer", "parallax", "entities", "hex_rotation", "tint", "collision", "wang_set"
        std::string name;          // the layer, level or tile it is on
        int count = 1;
    };

    // The counts of the R46 report
    struct ImportCounts {
        int grids = 0;
        int layers = 0;
        int tilesets = 0;
        int images = 0;
        int64_t tiles = 0;         // cells holding a tile
        int64_t terrainCells = 0;  // cells holding a terrain
        int skipped = 0;
        int warnings = 0;
    };

    struct ImportPlan {
        std::string sourceFile;
        std::string format;        // "tiled", "ldtk", "aseprite"
        std::vector<ImportGrid> grids;
        std::vector<ImportTileset> tilesets;
        std::vector<std::string> warnings;
        std::vector<ImportSkipped> skipped;

        ImportCounts counts() const {
            ImportCounts c;
            c.grids = static_cast<int>(grids.size());
            c.tilesets = static_cast<int>(tilesets.size());
            c.skipped = static_cast<int>(skipped.size());
            c.warnings = static_cast<int>(warnings.size());
            for (const ImportTileset& t : tilesets) c.images += static_cast<int>(t.images.size());
            for (const ImportGrid& g : grids) {
                c.layers += static_cast<int>(g.layers.size());
                for (const ImportLayer& l : g.layers) {
                    for (const ImportCell& cell : l.cells) {
                        if (cellValueTile(cell.value) != NO_TILE) c.tiles++;
                        if (cell.terrain != TERRAIN_NONE_VALUE) c.terrainCells++;
                    }
                }
            }
            return c;
        }
    };

}

#endif // TILEMAP_IMPORT_PLAN_H
