// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T4 R41, R44, R51: one undo step of a Terrain World's tile set or tile bytes made outside the map
// store's own undo (Add Tile, Remove Tile, Repair Seams). It keeps each listed tile as it was before
// the change and after it, and puts back one side through TiledMapStore::restoreTile, which re-reads
// the neighbours' gutters, so an undo leaves the files as they were. The world's files are written at
// once: the panel and MCP read them back.

#include "command/Command.h"
#include "ecs/Entity.h"
#include "terrain/world/WorldLattice.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class TerrainWorldRuntime;
}

namespace jasperengine::terrain {
    struct WorldTile;
}

namespace jasperengine::editor{

    class Project;

    class TerrainWorldTilesCmd: public Command{
    public:
        // The first execute snapshots `tiles`, runs change on the world's runtime and snapshots them
        // again; redo and undo put one snapshot back. verb: "Add Tiles", "Remove Tiles", "Repair Seams"
        using Change = std::function<bool(TerrainWorldRuntime& runtime, std::string& error)>;
        TerrainWorldTilesCmd(Project* project, uint32_t sceneId, Entity entity, std::string verb, std::vector<terrain::TileCoord> tiles,
                             Change change);

        bool execute() override;
        void undo() override;

        std::vector<Touched> touchedPaths(bool undo) const override;
        bool affectsStructure() const override { return false; }
        bool isNoOp() const override { return noOp; }
        std::string describe() const override;
        const char* typeName() const override { return "TerrainWorldTilesCmd"; }
        size_t memoryBytes() const override;
        std::string failure() const override { return lastError; }

    private:
        struct Snapshot{
            terrain::TileCoord tile;
            std::shared_ptr<const terrain::WorldTile> before;
            std::shared_ptr<const terrain::WorldTile> after;
        };

        TerrainWorldRuntime* runtime(std::string& error) const;
        void put(TerrainWorldRuntime& world, bool after);

        Project* project;
        uint32_t sceneId;
        Entity entity;
        std::string verb;
        std::string name;
        std::vector<terrain::TileCoord> tiles;
        Change change;
        std::vector<Snapshot> snapshots;
        std::string folder;
        bool done = false;
        bool noOp = false;
        std::string lastError;
    };

    // What an Add Tiles or Remove Tiles of these tiles would change, without changing anything: the tiles
    // added (absent ones) or removed (present ones), in the order given, duplicates once. False, with
    // why and its fix, when one is refused (R6's limits, the box, a tile that is not there) or none changes.
    bool checkTerrainWorldTiles(TerrainWorldRuntime& runtime, const std::vector<terrain::TileCoord>& tiles, bool remove,
                                std::vector<terrain::TileCoord>& changed, std::string& error);

    // R41, R44: Add Tile or Remove Tile as one undo step "Add Tiles (Name)" / "Remove Tiles (Name)" on the
    // scene's history, after checkTerrainWorldTiles. changed (when given) gets the tiles that changed.
    bool editTerrainWorldTiles(Project* project, uint32_t sceneId, Entity world, const std::vector<terrain::TileCoord>& tiles, bool remove,
                               std::string& error, std::vector<terrain::TileCoord>* changed = nullptr);

}
