// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/PrefabEditUndo.h"
#include "util/TerrainMapUtils.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // A structural step on a map: creating, deleting, rebinding, resizing or converting it, or a
    // first stroke that moved it to an editable file. It keeps whole-map snapshots, compressed
    // (T1 R28): a map edit is a TerrainMapDeltaCmd.
    class TerrainTextureEditCmd: public Command{
    public:
        // The pixels of both snapshots are moved into their compressed form
        TerrainTextureEditCmd(Project* project, uint32_t sceneId, Entity entity, const TerrainMapRef& ref,
                              TerrainMapSnapshot before, TerrainMapSnapshot after, std::string verb = {});

        bool execute() override;
        void undo() override;

        // "Sculpt Terrain (Island)", "Paint Terrain (Island)", "Paint Details (Island)"
        std::string describe() const override;
        const char* typeName() const override { return "TerrainTextureEditCmd"; }

        // Off-thread release: the destructor only frees map snapshots and override records; no editor/engine/GPU state (S24 R8).
        bool releaseOffThread() const override { return true; }
        size_t memoryBytes() const override;
        // the map file each apply writes (the write itself: TerrainMapUtils::writeFile, beneath the
        // step's write scope)
        std::vector<Touched> touchedPaths(bool undo) const override;

    private:
        struct PackedSnapshot{
            // everything but the pixels
            TerrainMapSnapshot head;
            std::vector<uint8_t> packed;
            size_t rawBytes = 0;
        };

        static PackedSnapshot pack(TerrainMapSnapshot snapshot);
        static TerrainMapSnapshot unpack(const PackedSnapshot& snapshot);
        void apply(const PackedSnapshot& snapshot, bool restoreModifiedState);

        Project* project;
        uint32_t sceneId;
        Entity entity;
        TerrainMapRef ref;
        PackedSnapshot beforePacked;
        PackedSnapshot afterPacked;
        bool wasModified = false;
        PrefabEditUndo prefabUndo;
        // the step's verb when it is not a sculpt or a paint ("Convert Heightmap to 16-bit")
        std::string verbOverride;
    };

}
