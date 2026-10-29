// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The nav-grid brush's cell operations (spec S19 R35), shared by the Scene view brush and the
// paint_nav_grid tool: stamps (circle or square, a size in cells), straight lines between
// stamps (Shift-drag), flood fills, and the edit that records what changed for one undo step.

#include "navigation/NavGridCellsCmd.h"
#include "navigation/NavGridMap.h"

#include <map>
#include <vector>

namespace jasperengine::editor::NavGridBrush {

    enum class Shape { Circle, Square };

    // the cells of a stamp of `size` cells across centred on `center` (clipped to the grid)
    std::vector<int> stamp(const nav::NavGridMap& map, nav::GridCell center, int size, Shape shape);
    // the cells from a to b, one per step (Bresenham on square grids, cube lines on hex grids)
    std::vector<nav::GridCell> line(const nav::NavGridMap& map, nav::GridCell a, nav::GridCell b);
    // the cells of start's area connected to it (the layout's neighbours; square grids: 4)
    std::vector<int> flood(const nav::NavGridMap& map, nav::GridCell start);

    // A stroke's edits: each cell's first old state and its last new one
    class Stroke {
    public:
        // sets the cells' area and override bit; returns how many changed now
        int apply(nav::NavGridMap& map, const std::vector<int>& cells, uint8_t area, bool overrideBit = true);
        bool empty() const;
        size_t size() const { return changes.size(); }
        // the cells whose state differs from before the stroke
        std::vector<NavGridCellsCmd::Cell> cells() const;
        // puts every cell back as it was before the stroke (a cancelled stroke)
        void revert(nav::NavGridMap& map) const;

    private:
        std::map<int, NavGridCellsCmd::Cell> changes;
    };

}
