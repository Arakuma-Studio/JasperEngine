// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Flat per-cell arrays and the hex neighbour tables follow OpenEngine's GridMap and
// HexGridHelpers (Engine/Modules/Pathfinding/Source/GridMap.cpp, HexGridHelpers.cpp), Copyright
// (c) 2026 Gamecrafters Guild Limited and contributors, MIT License; the hex point lookup is exact
// cube rounding instead of their rectangular approximation. See NOTICE.md.

#ifndef NAV_GRID_MAP_H
#define NAV_GRID_MAP_H

// A navigation grid (spec R15-R17): width x height cells of one area each (0-15; 1 Not Walkable
// blocks), costs from the project's area table, optional heights (slope and step limits), a
// per-cell override bit (cells the brush painted over a rebuilt source) and a reachability index
// (union-find components over walkable neighbours), rebuilt on the next query after an edit.
//
// Layouts: Square (4 or 8 neighbours) and Hex, pointy-top odd-r (odd rows shifted right by half
// a cell, 6 neighbours). Positions are grid-local 2D (the grid's own plane, origin at the corner
// of cell (0, 0), x along columns, y along rows); NavigationSystem maps them to the world.

#include "Export.h"
#include "math/Vector2.h"
#include "navigation/NavTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::nav {

    enum class GridLayout {
        Square,
        // pointy-top, odd-r
        Hex
    };

    struct GridCell {
        int x = 0;
        int y = 0;
        bool operator==(const GridCell& other) const { return x == other.x && y == other.y; }
        bool operator!=(const GridCell& other) const { return !(*this == other); }
    };

    class JASPERENGINE_API NavGridMap {
    public:
        static constexpr int MAX_NEIGHBOURS = 8;

        NavGridMap();

        // Resizes to width x height cells of `area`, no heights, no overrides
        void reset(int width, int height, uint8_t area = AREA_WALKABLE);
        void setLayout(GridLayout layout, int neighbours);
        void setCellSize(float size);
        void setSlopeLimits(float maxSlopeDegrees, float maxStep);

        int getWidth() const { return width; }
        int getHeight() const { return height; }
        int getCellCount() const { return width * height; }
        GridLayout getLayout() const { return layout; }
        // 4 or 8 (square), 6 (hex)
        int getNeighbourMode() const { return layout == GridLayout::Hex ? 6 : neighbourMode; }
        float getCellSize() const { return cellSize; }
        float getMaxSlope() const { return maxSlope; }
        float getMaxStep() const { return maxStep; }

        bool contains(GridCell cell) const { return cell.x >= 0 && cell.y >= 0 && cell.x < width && cell.y < height; }
        int index(GridCell cell) const { return cell.y * width + cell.x; }
        GridCell cellOf(int index) const { return GridCell{index % width, index / width}; }

        uint8_t getArea(GridCell cell) const { return areas[(size_t)index(cell)]; }
        uint8_t getAreaAt(int index) const { return areas[(size_t)index]; }
        // the cells' areas, row by row (the pathfinder's inner loop)
        const uint8_t* areaData() const { return areas.data(); }
        // false outside the grid or for an area > 15
        bool setArea(GridCell cell, uint8_t area);
        // the cells of [a, b] (corners in any order) clipped to the grid; returns how many changed
        int fillRect(GridCell a, GridCell b, uint8_t area);
        bool isWalkable(int index) const { return areas[(size_t)index] != AREA_NOT_WALKABLE; }

        bool getOverride(GridCell cell) const;
        void setOverride(GridCell cell, bool value);
        void clearOverrides();
        int countOverrides() const;

        bool hasHeights() const { return !heights.empty(); }
        void setHasHeights(bool value);
        float getHeight(GridCell cell) const { return heights.empty() ? 0.0f : heights[(size_t)index(cell)]; }
        void setHeight(GridCell cell, float value);

        // ---- geometry (grid-local 2D)
        Vector2 cellCenter(GridCell cell) const;
        // the cell whose area holds the point (exact for hex: cube rounding); may be outside
        GridCell cellAt(const Vector2& point) const;
        // the grid's extent from (0, 0)
        Vector2 getSize() const;
        // the corners of a cell (4 square, 6 hex), counter-clockwise
        int cellCorners(GridCell cell, Vector2* out) const;

        // The cells next to `cell` a path may step to (the layout's offsets, clipped to the grid;
        // walkability and limits are the pathfinder's), with the step length in cells
        int neighbours(GridCell cell, GridCell* out, float* stepLengths) const;
        // whether a step between two neighbouring cells respects the slope and step limits
        bool stepAllowed(int from, int to, float stepLength) const;
        // cells between centres (hex: cube distance, square: by the neighbour mode)
        float heuristic(GridCell a, GridCell b) const;
        int hexDistance(GridCell a, GridCell b) const;

        // ---- reachability (spec R17): the component of every walkable cell, rebuilt on demand
        bool isReachable(GridCell a, GridCell b) const;
        uint32_t getComponent(GridCell cell) const;
        void ensureComponents() const;
        bool componentsDirty() const { return componentsStale; }

        // changes with every edit (agents repath across edited cells, overlays redraw)
        uint64_t getRevision() const { return revision; }
        // the cells edited since the last takeEditedCells
        std::vector<int> takeEditedCells();

        size_t memoryBytes() const;

        // ---- the .navgrid format: "JNGR", version, layout, neighbours, dimensions, cell size,
        // limits, cell areas, override bits, optional heights, checksum (little-endian)
        static constexpr uint32_t FILE_VERSION = 1;
        std::vector<uint8_t> write() const;
        bool read(const uint8_t* bytes, size_t size, std::string& error);

    private:
        void touched(int index);

        int width = 0;
        int height = 0;
        GridLayout layout = GridLayout::Square;
        int neighbourMode = 8;
        float cellSize = 1.0f;
        float maxSlope = 45.0f;
        float maxStep = 0.5f;
        std::vector<uint8_t> areas;
        std::vector<uint8_t> overrideBits;
        std::vector<float> heights;
        uint64_t revision = 1;
        std::vector<int> edited;
        bool editedAll = false;

        mutable std::vector<uint32_t> components;
        mutable bool componentsStale = true;
    };

}

#endif // NAV_GRID_MAP_H
