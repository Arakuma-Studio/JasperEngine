// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T4 R32: Import Heightmap's swap, one undo step. The import's job wrote every tile it changes, whole,
// into a stash folder under .jasperengine/trash (after/x_z.jtile); the first execute checks that no
// tile file changed since the job read it, keeps each replaced file in the stash (before/, a hard link
// where the disk allows, else a copy) and writes the new ones over the world's tiles. Undo and redo
// write one side back, so the files come back byte for byte, and no tile is held in memory for it.

#include "command/Command.h"
#include "ecs/Entity.h"
#include "terrain/world/WorldLattice.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class TerrainWorldRuntime;
}

namespace jasperengine::editor{

    class Project;

    class TerrainWorldImportCmd: public Command{
    public:
        // A tile file as the import's job read it: whether it was there, its size and its time
        struct Stamp{
            bool exists = false;
            uintmax_t size = 0;
            std::filesystem::file_time_type time;
            bool operator==(const Stamp& other) const { return exists == other.exists && size == other.size && time == other.time; }
        };
        static Stamp stampOf(const std::filesystem::path& file);

        // reason, when given, gets why the first execute failed (the history frees a command that fails)
        TerrainWorldImportCmd(Project* project, uint32_t sceneId, Entity entity, std::filesystem::path stash, std::vector<terraincore::TileCoord> tiles,
                              std::vector<Stamp> stamps, std::shared_ptr<std::string> reason = nullptr);
        ~TerrainWorldImportCmd() override;

        bool execute() override;
        void undo() override;

        std::vector<Touched> touchedPaths(bool undo) const override;
        bool affectsStructure() const override { return false; }
        std::string describe() const override;
        const char* typeName() const override { return "TerrainWorldImportCmd"; }
        std::string failure() const override { return lastError; }

    private:
        TerrainWorldRuntime* runtime(std::string& error) const;
        bool apply();
        // writes one side's files over the world's tiles and reloads them; false (with the tiles put
        // back as they were) when a write fails
        bool put(TerrainWorldRuntime& world, bool after, std::string& error);

        Project* project;
        uint32_t sceneId;
        Entity entity;
        std::string name;
        std::filesystem::path stash;
        std::filesystem::path folder;
        std::vector<terraincore::TileCoord> tiles;
        std::vector<Stamp> stamps;
        std::vector<char> existed;
        bool done = false;
        std::string lastError;
        std::shared_ptr<std::string> reason;
    };

}
