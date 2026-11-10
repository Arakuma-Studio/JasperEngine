// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T4 R27: T5's trees on a Terrain World, one store per tile (tiles/x_z.jetrees), for the editor's tree
// tools (the Paint Trees brush, Mass Place Trees, edit_terrain's foliage ops, a spline's Clear Trees).
// An edit routes each tree to the tile under it, in that tile's frame: a square centred on the tile, as
// its foliage region is. A resident tile is edited in its region's store, so its cells follow each
// change; a tile out of memory is read from its file there and then (the editor may wait, R24). A tile
// with no file starts empty and gets one when the edit ends. Files are written when the edit ends and at
// each undo and redo, through the atomic writer, and the step keeps each file's bytes before and after,
// so undo and redo give the bytes back exactly.

#include "command/Command.h"
#include "ecs/Entity.h"
#include "math/Vector2.h"
#include "terrain/TreeStore.h"
#include "terrain/world/WorldLattice.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace jasperengine {
    class TerrainWorldRuntime;
}

namespace jasperengine::editor {

    class Project;

    // A World's tree as edit_terrain names it (place_trees' ids, remove_trees): the tile and the record id
    // in one integer below 2^53, which JSON carries exactly
    uint64_t terrainWorldTreeId(terraincore::TileCoord tile, uint32_t record);
    bool splitTerrainWorldTreeId(uint64_t id, terraincore::TileCoord& tile, uint32_t& record);

    class TerrainWorldTreeEdit {
    public:
        TerrainWorldTreeEdit(Project* project, uint32_t sceneId, Entity world);

        // null, with why in error(), when the GameObject is not a running World out of Play
        TerrainWorldRuntime* runtime() const { return world; }
        const std::string& error() const { return problem; }

        float tileSize() const;
        float maxHeight() const;
        // The tile under an entity-local point (a point on a border is the higher tile's)
        terraincore::TileCoord tileAt(float localX, float localZ) const;
        bool present(terraincore::TileCoord tile) const;
        // The tile's centre, entity-local: the origin of its frame
        Vector2 centre(terraincore::TileCoord tile) const;
        // The present tiles a box of entity-local points reaches, in (z, x) order
        std::vector<terraincore::TileCoord> tilesIn(float minX, float minZ, float maxX, float maxZ) const;
        // Every present tile, in (z, x) order
        std::vector<terraincore::TileCoord> presentTiles() const;

        // The tile's trees as this edit has them; null, with why, over an absent tile or a file that does
        // not read (it is never overwritten)
        const terraincore::TreeStore* trees(terraincore::TileCoord tile, std::string* why = nullptr);
        // One change to a tile, records in its frame (id 0 takes the next id); false when the tile is refused
        bool apply(terraincore::TileCoord tile, std::span<const uint32_t> removed, std::span<const terraincore::TreeRecord> added, terraincore::TreeDelta* delta = nullptr);
        // A delta made against what trees() gave for the tile (a job's working copy)
        bool applyDelta(terraincore::TileCoord tile, const terraincore::TreeDelta& delta);
        bool touched() const;

        // Writes the changed tiles' files and adds one step named name, already done, to the scene's
        // history; alongside runs with each application of the step (what a tool changes with the trees,
        // undone: true on undo). Nothing changed: no step, true. A write that fails puts back the files it
        // wrote and the stores, and returns false.
        bool commit(const std::string& name, std::string* error = nullptr, std::function<void(bool undone)> alongside = nullptr);
        // Every touched tile as it was (a cancelled stroke)
        void revert();

    private:
        struct Tile {
            terraincore::TileCoord coord;
            std::shared_ptr<terraincore::TreeStore> store;
            bool region = false;    // the resident region's store: its cells follow each change
            std::optional<std::string> before;
            bool changed = false;
            std::string refused;
        };
        Tile* tile(terraincore::TileCoord coord, std::string* why);

        Project* project;
        uint32_t sceneId;
        Entity entity;
        TerrainWorldRuntime* world = nullptr;
        std::filesystem::path folder;
        std::string problem;
        std::map<uint32_t, Tile> tiles;
    };

    // One undo step of a World's tree files: each tile's .jetrees bytes before and after (none: no file).
    // An undo or redo writes one side and hands it to the tile's region when the tile is resident.
    class TerrainWorldTreesCmd : public Command {
    public:
        struct TileFiles {
            terraincore::TileCoord tile;
            std::optional<std::string> before;
            std::optional<std::string> after;
        };
        // done: the files already hold the after side (the edit wrote them); the first execute only runs alongside
        TerrainWorldTreesCmd(Project* project, uint32_t sceneId, Entity world, std::string name, std::vector<TileFiles> tiles, bool done);

        bool execute() override;
        void undo() override;
        std::vector<Touched> touchedPaths(bool undo) const override;
        bool affectsStructure() const override { return false; }
        bool isNoOp() const override { return tiles.empty(); }
        std::string describe() const override { return name; }
        const char* typeName() const override { return "TerrainWorldTreesCmd"; }
        size_t memoryBytes() const override;
        std::string failure() const override { return lastError; }

        std::function<void(bool undone)> alongside;

    private:
        bool put(bool after, std::string& error);

        Project* project;
        uint32_t sceneId;
        Entity entity;
        std::string name;
        std::vector<TileFiles> tiles;
        std::string folder;
        bool done;
        std::string lastError;
    };

}
