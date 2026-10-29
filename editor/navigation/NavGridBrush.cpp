// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavGridBrush.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::editor;
using nav::GridCell;
using nav::NavGridMap;

std::vector<int> NavGridBrush::stamp(const NavGridMap& map, GridCell center, int size, Shape shape) {
    std::vector<int> cells;
    size = std::max(1, size);
    const float radius = (float)size * 0.5f;
    const int reach = (size + 1) / 2 + 1;
    const Vector2 c = map.cellCenter(center);
    const float cellSize = std::max(map.getCellSize(), 1e-6f);
    for (int y = center.y - reach; y <= center.y + reach; y++) {
        for (int x = center.x - reach; x <= center.x + reach; x++) {
            const GridCell cell{x, y};
            if (!map.contains(cell)) continue;
            bool inside;
            if (map.getLayout() == nav::GridLayout::Hex) {
                // hex: by cube distance (size 1: the cell, 3: its ring...)
                inside = map.hexDistance(center, cell) <= (size - 1) / 2;
            } else if (shape == Shape::Square) {
                // an even size grows to the +x / +y side
                const int low = (size - 1) / 2, high = size / 2;
                inside = x >= center.x - low && x <= center.x + high && y >= center.y - low && y <= center.y + high;
            } else {
                const Vector2 p = map.cellCenter(cell);
                const float dx = (p.x - c.x) / cellSize, dy = (p.y - c.y) / cellSize;
                inside = std::sqrt(dx * dx + dy * dy) <= std::max(radius - 0.5f, 0.0f) + 1e-4f;
            }
            if (inside) cells.push_back(map.index(cell));
        }
    }
    return cells;
}

std::vector<GridCell> NavGridBrush::line(const NavGridMap& map, GridCell a, GridCell b) {
    std::vector<GridCell> cells;
    if (map.getLayout() == nav::GridLayout::Hex) {
        const int steps = map.hexDistance(a, b);
        const Vector2 pa = map.cellCenter(a), pb = map.cellCenter(b);
        for (int i = 0; i <= steps; i++) {
            const float t = steps == 0 ? 0.0f : (float)i / (float)steps;
            const GridCell cell = map.cellAt(pa + (pb - pa) * t);
            if (cells.empty() || cells.back() != cell) cells.push_back(cell);
        }
        return cells;
    }
    int x0 = a.x, y0 = a.y;
    const int dx = std::abs(b.x - a.x), dy = -std::abs(b.y - a.y);
    const int sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
    int error = dx + dy;
    while (true) {
        cells.push_back(GridCell{x0, y0});
        if (x0 == b.x && y0 == b.y) break;
        const int e2 = 2 * error;
        if (e2 >= dy) {
            error += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            error += dx;
            y0 += sy;
        }
    }
    return cells;
}

std::vector<int> NavGridBrush::flood(const NavGridMap& map, GridCell start) {
    std::vector<int> cells;
    if (!map.contains(start)) return cells;
    const uint8_t area = map.getArea(start);
    std::vector<uint8_t> seen((size_t)map.getCellCount(), 0);
    std::vector<int> open = {map.index(start)};
    seen[(size_t)map.index(start)] = 1;
    GridCell next[NavGridMap::MAX_NEIGHBOURS];
    float steps[NavGridMap::MAX_NEIGHBOURS];
    while (!open.empty()) {
        const int index = open.back();
        open.pop_back();
        cells.push_back(index);
        const int count = map.neighbours(map.cellOf(index), next, steps);
        for (int n = 0; n < count; n++) {
            // a fill stays inside what a wall of its own encloses: no diagonal leaks
            if (map.getLayout() == nav::GridLayout::Square && steps[n] > 1.0f) continue;
            const int j = map.index(next[n]);
            if (seen[(size_t)j] || map.getAreaAt(j) != area) continue;
            seen[(size_t)j] = 1;
            open.push_back(j);
        }
    }
    std::sort(cells.begin(), cells.end());
    return cells;
}

int NavGridBrush::Stroke::apply(NavGridMap& map, const std::vector<int>& cells, uint8_t area, bool overrideBit) {
    int changed = 0;
    for (int index : cells) {
        if (index < 0 || index >= map.getCellCount()) continue;
        const GridCell cell = map.cellOf(index);
        const uint8_t oldArea = map.getAreaAt(index);
        const bool oldOverride = map.getOverride(cell);
        if (oldArea == area && oldOverride == overrideBit) continue;
        auto found = changes.find(index);
        if (found == changes.end()) {
            NavGridCellsCmd::Cell entry;
            entry.index = index;
            entry.oldArea = oldArea;
            entry.oldOverride = oldOverride;
            found = changes.emplace(index, entry).first;
        }
        found->second.newArea = area;
        found->second.newOverride = overrideBit;
        map.setArea(cell, area);
        map.setOverride(cell, overrideBit);
        changed++;
    }
    return changed;
}

bool NavGridBrush::Stroke::empty() const {
    for (const auto& entry : changes) {
        if (entry.second.oldArea != entry.second.newArea || entry.second.oldOverride != entry.second.newOverride) return false;
    }
    return true;
}

std::vector<NavGridCellsCmd::Cell> NavGridBrush::Stroke::cells() const {
    std::vector<NavGridCellsCmd::Cell> out;
    for (const auto& entry : changes) {
        if (entry.second.oldArea != entry.second.newArea || entry.second.oldOverride != entry.second.newOverride)
            out.push_back(entry.second);
    }
    return out;
}

void NavGridBrush::Stroke::revert(NavGridMap& map) const {
    for (const auto& entry : changes) {
        const GridCell cell = map.cellOf(entry.first);
        map.setArea(cell, entry.second.oldArea);
        map.setOverride(cell, entry.second.oldOverride);
    }
}
