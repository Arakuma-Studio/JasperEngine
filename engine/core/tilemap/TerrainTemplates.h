// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_TERRAIN_TEMPLATES_H
#define TILEMAP_TERRAIN_TEMPLATES_H

// Terrain templates and the coverage check (docs/specs/S50-grid-tilemap.md R19, R20).
//
// Layouts (the block's top-left tile is the origin, rows run down the image):
//   blob_47: cr31's 7x7 "minimum packing" sheet, cells named by cr31's index (N=1 NE=2 E=4 SE=8
//     S=16 SW=32 W=64 NW=128); tile 0 appears three times, as variations.
//   blob_47_godot: Godot 3's 12x4 "3x3 minimal" template; column 10 of row 1 is blank.
//   sides_16: 4x4, cell i (reading order) has the sides of i (N=1 E=2 S=4 W=8).
//   dual_grid_4x4: SpriteCook's sheet, frameByMask = [-1,15,8,9,0,11,14,7,13,4,1,10,3,2,5,6] with
//     mask TL=1 TR=2 BL=4 BR=8. Frame 12 is the all-second-terrain tile of a two_surface set, and
//     is left alone on any other set.
// Names: blob_<cr31 index> (variations blob_0_1, blob_0_2), sides_<i>, dual_<mask> (dual_surface
// for frame 12).

#include "Export.h"
#include "tilemap/Tileset.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace jasperengine{

    enum class TerrainTemplate : uint8_t { Blob47, Blob47Godot, Sides16, DualGrid4x4 };

    struct TerrainTemplateOptions{
        int terrain = 0;                 // the set-local terrain the tiles draw
        int secondTerrain = -1;          // two_surface sets: the other surface; -1: the other of the set's first two
        bool assignNames = true;
    };

    // a mask no tile matches: the centre terrain (dual grid: -1) and one value per peering slot
    // (PEERING_ANY in the slots the mode does not use, as on a tile that would fill it)
    struct TerrainMissingMask{
        int center = -1;
        std::array<int16_t, MAX_PEERING_SLOTS> peering;
        bool operator==(const TerrainMissingMask& o) const { return center == o.center && peering == o.peering; }
    };

    struct TerrainCoverage{
        size_t required = 0;             // the distinct masks the mode can produce
        std::vector<TerrainMissingMask> missing;   // deterministic order
    };

    namespace terrain{

        JASPERENGINE_API const char* templateName(TerrainTemplate value);
        JASPERENGINE_API bool parseTemplate(std::string_view text, TerrainTemplate& out);
        // the template's block size in tiles
        JASPERENGINE_API void templateSize(TerrainTemplate value, int& columns, int& rows);

        // Assigns names and peering values to the block of tiles whose top-left tile is
        // originTile. Texture sources find each tile by its grid position; other sources take the
        // tiles after the origin in id order, in reading order. All or nothing: false with
        // problems, the tileset unchanged, when the set's mode or the tile shape does not fit or a
        // tile of the block is missing.
        JASPERENGINE_API bool applyTemplate(TilesetData& data, int set, TerrainTemplate value, int32_t originTile, const TerrainTemplateOptions& options,
                                            std::vector<TilesetProblem>& problems);

        // R20: the masks the set's mode can produce that no tile matches (a tile's "any" matches
        // every value): blob the 47 reduced masks (hexagons 64), sides 16 (hexagons 64), dual
        // grid 15 per terrain against none and 16 per terrain pair, for each centre terrain and
        // each other value (none or another terrain); a two_surface dual-grid set only its two
        // surfaces. allowTransforms also counts the flipped and transposed variants of square
        // tiles.
        JASPERENGINE_API TerrainCoverage coverage(const TilesetData& data, int set, bool allowTransforms);
    }

}

#endif // TILEMAP_TERRAIN_TEMPLATES_H
