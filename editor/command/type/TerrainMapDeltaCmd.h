// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "ecs/Entity.h"
#include "terrain/TerrainMapStore.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // T1 R24's undo step, in the smallest form generation needs until T1's lands on it (T2 D, see the
    // spec's Deviations): one rectangle per changed map, the XOR of its stored bytes before and after, so
    // undo and redo are the same operation and the step never holds two copies. The first execute runs
    // the producer, which writes through a store and returns the deltas; the maps' files are rewritten
    // in full, because PNG has no partial write.
    class TerrainMapDeltaCmd : public Command {
    public:
        struct MapDelta {
            terrain::MapId map;
            terrain::TexelRect rect;
            std::vector<uint8_t> xorBytes;
        };
        using Producer = std::function<bool(std::vector<MapDelta>& deltas)>;

        TerrainMapDeltaCmd(Project* project, uint32_t sceneId, Entity entity, std::string label, Producer producer);

        bool execute() override;
        void undo() override;
        std::vector<Touched> touchedPaths(bool undo) const override;
        std::string describe() const override { return label; }
        const char* typeName() const override { return "TerrainMapDeltaCmd"; }
        size_t memoryBytes() const override;
        bool releaseOffThread() const override { return true; }

    private:
        void applyDeltas(bool restoreModified);
        void persist(const MapDelta& delta);

        Project* project;
        uint32_t sceneId;
        Entity entity;
        std::string label;
        Producer producer;
        std::vector<MapDelta> deltas;
        bool wasModified = false;
    };

}
