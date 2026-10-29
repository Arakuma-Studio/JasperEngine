// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // One undo step over a NavGrid's cells (spec R35): a brush stroke ("Paint Nav Grid") keeps
    // the cells it changed (area and override bit, before and after); a Rebuild or Reset
    // Overrides keeps the whole map before and after (.navgrid bytes). The grid's .navgrid is
    // written with the next scene save.
    class NavGridCellsCmd : public Command {
    public:
        struct Cell {
            int index = 0;
            uint8_t oldArea = 0;
            uint8_t newArea = 0;
            bool oldOverride = false;
            bool newOverride = false;
        };

        // cells already applied (the stroke painted them as it went): execute() reapplies
        NavGridCellsCmd(Project* project, uint32_t sceneId, Entity grid, std::vector<Cell> cells, std::string name = "Paint Nav Grid");
        // whole maps (NavGridMap::write)
        NavGridCellsCmd(Project* project, uint32_t sceneId, Entity grid, std::vector<uint8_t> before, std::vector<uint8_t> after,
                        std::string name);

        bool execute() override;
        void undo() override;
        bool affectsStructure() const override { return false; }
        bool isNoOp() const override { return cells.empty() && before == after; }
        size_t memoryBytes() const override { return sizeof(*this) + cells.size() * sizeof(Cell) + before.size() + after.size(); }

        std::string describe() const override { return name; }
        const char* typeName() const override { return "NavGridCellsCmd"; }

    private:
        Project* project;
        uint32_t sceneId;
        Entity grid;
        std::vector<Cell> cells;
        std::vector<uint8_t> before;
        std::vector<uint8_t> after;
        std::string name;

        bool apply(bool forward);
    };

}
