// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// One changed rectangle of a detail layer's density map, as it was and is (T5 R23, R52): what
// scatter_by_rules and clear_foliage leave in the undo history, through T1's store, so foliage's
// partial rebuild hears of it.

#include "command/Command.h"
#include "ecs/Entity.h"
#include "terrain/TerrainMapStore.h"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class FoliageMapPatchCmd : public Command {
    public:
        FoliageMapPatchCmd(Project* project, uint32_t sceneId, Entity terrain, terrain::MapId map, terrain::TexelRect rect,
                           std::vector<uint8_t> before, std::vector<uint8_t> after, std::string name);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }
        std::string describe() const override { return name; }
        const char* typeName() const override { return "FoliageMapPatchCmd"; }
        size_t memoryBytes() const override { return sizeof(*this) + name.capacity() + before.capacity() + after.capacity(); }

        // convert_placed_to_trees' other half: the hosts' instances out on apply, back on undo (R28)
        static std::function<void(bool undone)> emptyHosts(Project* project, uint32_t sceneId, std::vector<std::string> hosts);

    private:
        void write(const std::vector<uint8_t>& bytes);

        Project* project;
        uint32_t sceneId;
        Entity terrain;
        terrain::MapId map;
        terrain::TexelRect rect;
        std::vector<uint8_t> before;
        std::vector<uint8_t> after;
        std::string name;
    };

}
