// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_TILESET_H
#define TILEMAP_TILESET_H

// The Tileset asset (docs/specs/S50-grid-tilemap.md R11-R20, §6.2): a .tileset JSON document
// (version 1; its GUID is in the .meta sidecar, S04), parsed and written here for the runtime and
// the editor alike. Pure: no file access, no textures (TilesetPool resolves pages for drawing).
//
// Ids (R13): every tile has an id unique in its tileset and never reused; `nextId` (written as
// "next_id") is one past the largest id ever given, so a deleted tile's id is not handed out again.
// The file lists every tile (at least id, source, at), sorted by id.
//
// Tile pixels: `at` is the tile's position in its source: texture sources the grid column and row
// (top row 0, as the image), aseprite sources the tile index (x, 0) where our tile i is the
// Aseprite tileset's tile i + 1, sheet sources the frame name. Collision polygons are in tile
// pixels with the origin at the tile image's bottom-left corner and y up (the cell's space), on the
// pixel grid (integers).
//
// Terrains (R18): terrain sets hold named terrains; the terrain plane stores the tileset-global
// terrain index (set 0's terrains first, then set 1's, ...), at most MAX_TILESET_TERRAINS. A tile's
// terrain data names its set and the set-local centre terrain, and one peering value per direction
// of the set's mode: PEERING_ANY (no constraint), PEERING_NONE (no terrain of this set), or a
// set-local terrain index.
//   Peering slots (TileTerrain::peering): square and isometric tiles use slots 0-7 for the
//   directions n ne e se s sw w nw (TileGridLayout's order); blob uses all 8, sides n e s w
//   (slots 0, 2, 4, 6), dual_grid the corners ne se sw nw (slots 1, 3, 5, 7: a dual-grid tile's
//   corners TR BR BL TL). Hexagon tiles use slots 0-5 for the six sides (counter-clockwise from
//   east) and 6-11 for the six corners (counter-clockwise, the first after side 0): keys
//   hex_pointy "e ne nw w sw se" + "c_ne c_n c_nw c_sw c_s c_se", hex_flat "ne n nw sw s se" +
//   "c_e c_ne c_nw c_w c_sw c_se". Sides mode uses only the 6 sides on hexagons.

#include "Export.h"
#include "asset/AssetRef.h"
#include "math/Vector4.h"
#include "tilemap/TileTypes.h"

#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine{

    enum class TilesetSourceKind : uint8_t { Texture, Sheet, Aseprite };
    enum class TileShape : uint8_t { Square, Isometric, HexPointy, HexFlat };
    enum class TileCollisionMode : uint8_t { None, Full, Alpha, Custom };
    enum class TileCollisionKind : uint8_t { Solid, OneWay, Sensor };
    enum class TileOneWayDirection : uint8_t { Up, Down, Left, Right };
    enum class TerrainMode : uint8_t { Blob, Sides, DualGrid };
    enum class TilePropertyType : uint8_t { Bool, Int, Float, String, Color };

    constexpr int16_t PEERING_ANY = -2;
    constexpr int16_t PEERING_NONE = -1;
    constexpr int MAX_PEERING_SLOTS = 12;

    struct JASPERENGINE_API TilesetSource{
        int id = 0;
        TilesetSourceKind kind = TilesetSourceKind::Texture;
        // the texture, the sheet (anything SpriteSheetPool::get loads) or the .aseprite file
        AssetRef asset;
        // texture: the slicing (Tiled's names); 0 size: the tileset's tile size
        int tileWidth = 0;
        int tileHeight = 0;
        int margin = 0;
        int spacing = 0;
        bool skipEmpty = true;
        // sheet: the frames it takes, by name (empty: every frame of the sheet)
        std::vector<std::string> frames;
        // aseprite: the file's tileset index
        int asepriteTileset = 0;
    };

    struct TilePolygon{
        std::vector<std::array<int32_t, 2>> points;
        bool operator==(const TilePolygon& o) const { return points == o.points; }
    };

    struct TileCollision{
        TileCollisionMode mode = TileCollisionMode::None;
        TileCollisionKind kind = TileCollisionKind::Solid;
        TileOneWayDirection oneWay = TileOneWayDirection::Up;
        int threshold = 128;             // alpha: the alpha at or above which a pixel is solid
        // custom: the authored polygons; alpha: the traced outlines (written by the editor, so
        // the runtime never reads pixels); full: unused (the cell rect)
        std::vector<TilePolygon> polygons;
        bool operator==(const TileCollision& o) const {
            return mode == o.mode && kind == o.kind && oneWay == o.oneWay && threshold == o.threshold && polygons == o.polygons;
        }
    };

    struct TileAnimationFrame{
        int32_t tile = 0;
        int32_t ms = 100;
        bool operator==(const TileAnimationFrame& o) const { return tile == o.tile && ms == o.ms; }
    };

    struct TileTerrain{
        int set = -1;                    // -1: the tile has no terrain data
        int center = -1;                 // set-local terrain index; -1: none
        std::array<int16_t, MAX_PEERING_SLOTS> peering;
        TileTerrain(){ peering.fill(PEERING_ANY); }
        bool operator==(const TileTerrain& o) const { return set == o.set && center == o.center && peering == o.peering; }
    };

    struct JASPERENGINE_API TilePropertyValue{
        TilePropertyType type = TilePropertyType::Int;
        bool b = false;
        int64_t i = 0;
        double f = 0.0;
        std::string s;
        Vector4 color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);   // sRGB 0-1 (written "#rrggbbaa")
        bool operator==(const TilePropertyValue& o) const;
    };

    struct JASPERENGINE_API TileDef{
        int32_t id = 0;
        int source = 0;                  // TilesetSource::id
        int atX = 0;
        int atY = 0;
        std::string frame;               // sheet sources
        std::string name;                // "" in memory: the default <source>_<index> (defaultTileName)
        int offsetX = 0;                 // pixels, y up (Tiled's tileoffset with y flipped)
        int offsetY = 0;
        float probability = 1.0f;
        TileCollision collision;
        std::vector<TileAnimationFrame> animation;
        bool randomStart = false;
        TileTerrain terrain;
        std::map<std::string, TilePropertyValue> properties;
        int navArea = -1;                // S19 area; -1: none
        bool operator==(const TileDef& o) const;
    };

    struct TerrainDef{
        std::string name;
        std::string color = "#ffffff";   // "#rrggbb"
        bool operator==(const TerrainDef& o) const { return name == o.name && color == o.color; }
    };

    struct TerrainSetDef{
        std::string name;
        TerrainMode mode = TerrainMode::Blob;
        bool twoSurface = false;         // dual_grid: the second terrain fills frame 12 (D6)
        std::vector<TerrainDef> terrains;
        bool operator==(const TerrainSetDef& o) const {
            return name == o.name && mode == o.mode && twoSurface == o.twoSurface && terrains == o.terrains;
        }
    };

    struct PropertySchemaEntry{
        std::string name;
        TilePropertyType type = TilePropertyType::Int;
        TilePropertyValue defaultValue;
        bool operator==(const PropertySchemaEntry& o) const { return name == o.name && type == o.type && defaultValue == o.defaultValue; }
    };

    struct TileStampCell{
        int32_t x = 0;
        int32_t y = 0;
        int32_t tile = 0;
        uint32_t flags = 0;              // TILE_FLIP_X | TILE_FLIP_Y | TILE_TRANSPOSE
        bool operator==(const TileStampCell& o) const { return x == o.x && y == o.y && tile == o.tile && flags == o.flags; }
    };

    struct TileStamp{
        std::string name;
        int width = 1;
        int height = 1;
        std::vector<TileStampCell> cells;
        bool operator==(const TileStamp& o) const { return name == o.name && width == o.width && height == o.height && cells == o.cells; }
    };

    // a problem of a document or an edit, typed as the MCP errors (R79): kind, message, fix, field
    struct TilesetProblem{
        std::string kind;                // "parse", "unknown_tile", "invalid_peering", "unknown_property", ...
        std::string field;               // JSON path ("tiles[3].terrain.peering.ne")
        std::string message;
        std::string fix;
    };

    struct JASPERENGINE_API TilesetData{
        int version = 1;
        std::string name;
        int tileWidth = 16;
        int tileHeight = 16;
        TileShape shape = TileShape::Square;
        std::vector<TilesetSource> sources;
        std::vector<PropertySchemaEntry> propertySchema;
        std::vector<TerrainSetDef> terrainSets;
        std::vector<TileDef> tiles;      // sorted by id
        std::vector<TileStamp> stamps;
        int32_t nextId = 0;

        const TileDef* findTile(int32_t id) const;
        TileDef* findTile(int32_t id);
        // by name (a default name included); NO_TILE when none
        int32_t findTileByName(std::string_view name) const;
        // the displayed name: the tile's, else "<source>_<index>" (index: its position among the
        // source's tiles in id order)
        std::string tileName(const TileDef& tile) const;
        const TilesetSource* findSource(int id) const;
        // adds a tile with the next id (kept sorted); returns it
        TileDef& addTile(int source, int atX, int atY);
        bool removeTile(int32_t id);

        // terrains: the global index of a set's terrain, and back; NO_TERRAIN when out of range
        int terrainCount() const;
        int terrainIndex(int set, int local) const;
        bool terrainOf(int global, int& set, int& local) const;
        const TerrainDef* terrain(int global) const;
        // by name (global index), NO_TERRAIN when none; a name shared by two sets gives the first
        int findTerrain(std::string_view name) const;
        const PropertySchemaEntry* findProperty(std::string_view name) const;

        bool operator==(const TilesetData& o) const;
    };

    namespace tileset{
        // "texture" / "sheet" / "aseprite", "square" / "isometric" / "hex_pointy" / "hex_flat", ...
        JASPERENGINE_API const char* sourceKindName(TilesetSourceKind kind);
        JASPERENGINE_API const char* shapeName(TileShape shape);
        JASPERENGINE_API const char* terrainModeName(TerrainMode mode);
        JASPERENGINE_API const char* propertyTypeName(TilePropertyType type);
        JASPERENGINE_API const char* collisionModeName(TileCollisionMode mode);
        JASPERENGINE_API const char* collisionKindName(TileCollisionKind kind);
        JASPERENGINE_API bool parseSourceKind(std::string_view text, TilesetSourceKind& out);
        JASPERENGINE_API bool parseShape(std::string_view text, TileShape& out);
        JASPERENGINE_API bool parseTerrainMode(std::string_view text, TerrainMode& out);
        JASPERENGINE_API bool parsePropertyType(std::string_view text, TilePropertyType& out);

        // the peering slots a mode uses on a shape, in key order, and the key of a slot
        JASPERENGINE_API std::vector<int> peeringSlots(TileShape shape, TerrainMode mode);
        JASPERENGINE_API const char* peeringKey(TileShape shape, int slot);
        JASPERENGINE_API int peeringSlotOfKey(TileShape shape, std::string_view key);

        // parses a document: false on a fatal problem (kind "parse", with its JSON path or line);
        // non-fatal problems (an unknown key, a dangling animation frame) are added and skipped
        JASPERENGINE_API bool parse(std::string_view text, TilesetData& out, std::vector<TilesetProblem>& problems);
        // the document: keys in §6.2's order, tiles by id, unset per-tile fields left out (R14),
        // two-space indent, one tile per line; parse(write(d)) == d and write(parse(t)) == t for
        // what write produced
        JASPERENGINE_API std::string write(const TilesetData& data);
        // the edits' checks: undeclared properties, peering values outside the set, animation
        // frames naming no tile, terrain counts past MAX_TILESET_TERRAINS, duplicate ids/names...
        JASPERENGINE_API void validate(const TilesetData& data, std::vector<TilesetProblem>& problems);

        // the R22 hash: a deterministic value of (seed, stroke, x, y) in [0, 2^32)
        JASPERENGINE_API uint32_t cellHash(uint32_t seed, uint32_t stroke, int32_t x, int32_t y);
        // a weighted choice among `weights` (> 0) with a cellHash value; -1 when none is positive
        JASPERENGINE_API int weightedPick(const float* weights, size_t count, uint32_t hash);
    }

}

#endif // TILEMAP_TILESET_H
