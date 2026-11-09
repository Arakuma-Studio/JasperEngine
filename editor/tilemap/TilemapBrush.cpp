// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TilemapBrush.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace jasperengine::editor::tilemap {

    namespace {

        constexpr uint32_t kBrushFloodMask = TILE_ID_MASK | TILE_TRANSFORM_MASK;

        PixelProblem brushProblem(const char* kind, std::string message, std::string fix, std::string field = {}) {
            PixelProblem p;
            p.kind = kind;
            p.field = std::move(field);
            p.message = std::move(message);
            p.fix = std::move(fix);
            return p;
        }

        PixelProblem brushRegionTooLarge(int64_t cells) {
            return brushProblem("region_too_large",
                                "the operation covers " + std::to_string(cells) + " cells, more than the limit of " + std::to_string(MAX_TILE_REGION_CELLS),
                                "work on a smaller region, or fill inside a closed area");
        }

        bool brushHasTileset(const BrushTarget& target, BrushEdit& edit) {
            if (target.tileset) return true;
            edit.error = brushProblem("tileset_missing", "the layer has no loaded tileset", "assign a Tileset to the Tilemap Layer", "tileset");
            return false;
        }

        TerrainContext brushTerrainContext(const BrushTarget& target) {
            TerrainContext context;
            context.tileset = target.tileset;
            context.grid = target.grid;
            context.dualGrid = target.dualGrid;
            context.seed = target.seed;
            return context;
        }

        bool brushCheckTile(const BrushTarget& target, int32_t tile, uint32_t flags, const char* field, BrushEdit& edit) {
            if (tile < 0 || tile > MAX_TILE_ID || !target.tileset->findTile(tile)) {
                edit.error = brushProblem("unknown_tile", "the tileset has no tile " + std::to_string(tile),
                                          "use a tile id or name the tileset has", field);
                return false;
            }
            if ((flags & TILE_TRANSPOSE) && target.grid.layout == TileGridLayout::Hexagon) {
                edit.error = brushProblem("layout_mismatch", "Hexagon layouts accept flips only (R5), not transpose",
                                          "use flip X and flip Y only", field);
                return false;
            }
            return true;
        }

        bool brushCheckStamp(const BrushTarget& target, const BrushStamp& stamp, BrushEdit& edit) {
            if (!brushHasTileset(target, edit)) return false;
            for (size_t i = 0; i < stamp.cells.size(); i++) {
                const std::string field = "stamp.cells[" + std::to_string(i) + "]";
                if (!brushCheckTile(target, stamp.cells[i].tile, stamp.cells[i].flags, field.c_str(), edit)) return false;
            }
            return true;
        }

        // a cell of a manual tile keeps a terrain under it: the cell itself, or on a dual-grid
        // layer any of the four logical cells around the display cell
        bool brushCellHasTerrain(const BrushTarget& target, CellCoord cell) {
            if (!target.dualGrid) return target.cells.getTerrainValue(cell) != TERRAIN_NONE_VALUE;
            const CellCoord corners[4] = {{cell.x - 1, cell.y}, {cell.x, cell.y}, {cell.x - 1, cell.y - 1}, {cell.x, cell.y - 1}};
            for (const CellCoord& corner : corners) {
                if (target.cells.getTerrainValue(corner) != TERRAIN_NONE_VALUE) return true;
            }
            return false;
        }

        using BrushWrite = std::pair<CellCoord, uint32_t>;

        // R26's rules for a set of manual tiles: terrain cells lose their terrain first (so the
        // neighbours see the finished state), then the tiles land, so a stamp over terrain ends
        // with the stamped tiles whatever the solver wrote
        bool brushWriteTiles(const BrushTarget& target, std::span<const BrushWrite> writes, const PaintOptions& options, BrushEdit& edit) {
            if (!target.dualGrid && !options.keepTerrain) {
                std::vector<CellCoord> cleared;
                for (const BrushWrite& write : writes) {
                    if (target.cells.getTerrainValue(write.first) != TERRAIN_NONE_VALUE) cleared.push_back(write.first);
                }
                if (!cleared.empty()) {
                    std::string solverError;
                    if (!terrain::paint(target.cells, brushTerrainContext(target), cleared, NO_TERRAIN, edit.patch, edit.terrain, solverError)) {
                        edit.error = brushProblem("layout_mismatch", solverError, "give the layer a tileset that fits its Grid");
                        return false;
                    }
                }
            }
            for (const BrushWrite& write : writes) {
                uint32_t value = write.second;
                if (options.keepTerrain && brushCellHasTerrain(target, write.first)) value |= TILE_LOCK;
                edit.patch.setValue(target.cells, write.first, value);
            }
            return true;
        }

        // the stamp's value at any cell, repeating from `origin`: box fills, lines and floods
        class BrushPattern {
        public:
            BrushPattern(const BrushStamp& stamp, CellCoord origin): origin(origin) {
                int32_t minX = 0, minY = 0, maxX = 0, maxY = 0;
                bool any = false;
                for (const BrushCell& cell : stamp.cells) {
                    if (!any) {
                        minX = maxX = cell.x;
                        minY = maxY = cell.y;
                        any = true;
                    } else {
                        minX = std::min(minX, cell.x);
                        maxX = std::max(maxX, cell.x);
                        minY = std::min(minY, cell.y);
                        maxY = std::max(maxY, cell.y);
                    }
                }
                width = stamp.width > 0 ? stamp.width : (any ? (int64_t)maxX - minX + 1 : 1);
                height = stamp.height > 0 ? stamp.height : (any ? (int64_t)maxY - minY + 1 : 1);
                // a stamp without a size starts at its own bottom-left cell, one with a size at (0, 0)
                const int32_t baseX = stamp.width > 0 ? 0 : minX;
                const int32_t baseY = stamp.height > 0 ? 0 : minY;
                single = stamp.cells.size() == 1 && width == 1 && height == 1;
                if (single) {
                    singleValue = makeCellValue(stamp.cells[0].tile, stamp.cells[0].flags & TILE_TRANSFORM_MASK);
                } else {
                    for (const BrushCell& cell : stamp.cells) {
                        const CellCoord local{(int32_t)(cell.x - baseX), (int32_t)(cell.y - baseY)};
                        values[local] = makeCellValue(cell.tile, cell.flags & TILE_TRANSFORM_MASK);
                    }
                }
            }

            // the stamp's value at `cell`; false in a hole of the stamp
            bool at(CellCoord cell, uint32_t& value) const {
                if (single) {
                    value = singleValue;
                    return true;
                }
                const CellCoord local{(int32_t)floorMod((int64_t)cell.x - origin.x, width),
                                      (int32_t)floorMod((int64_t)cell.y - origin.y, height)};
                const auto it = values.find(local);
                if (it == values.end()) return false;
                value = it->second;
                return true;
            }

        private:
            CellCoord origin;
            int64_t width = 1;
            int64_t height = 1;
            bool single = false;
            uint32_t singleValue = 0;
            std::unordered_map<CellCoord, uint32_t, CellCoordHash> values;

            static int64_t floorMod(int64_t a, int64_t b) { return ((a % b) + b) % b; }
        };

        // the stamp written at each of `cells`, a hole of the stamp leaving a cell untouched
        bool brushFillCells(const BrushTarget& target, const BrushStamp& stamp, CellCoord origin, std::span<const CellCoord> cells, const PaintOptions& options, BrushEdit& edit) {
            const BrushPattern pattern(stamp, origin);
            std::vector<BrushWrite> writes;
            writes.reserve(cells.size());
            for (const CellCoord& cell : cells) {
                uint32_t value = 0;
                if (pattern.at(cell, value)) writes.emplace_back(cell, value);
            }
            return brushWriteTiles(target, writes, options, edit);
        }

        bool brushErase(const BrushTarget& target, std::span<const CellCoord> cells, BrushEdit& edit) {
            for (const CellCoord& cell : cells) edit.patch.setValue(target.cells, cell, 0);
            return true;
        }

    }

    BrushStamp BrushStamp::single(int32_t tile, uint32_t flags) {
        BrushStamp stamp;
        stamp.cells.push_back(BrushCell{0, 0, tile, flags});
        return stamp;
    }

    PixelProblem lockedLinkedError() {
        return brushProblem("locked_linked", "this layer's Grid is linked to an imported map and is read-only",
                            "unlink the Grid or edit the source map");
    }

    bool boxCells(const CellRect& rect, std::vector<CellCoord>& out, PixelProblem& error) {
        out.clear();
        if (rect.isEmpty()) return true;
        if (rect.area() > MAX_TILE_REGION_CELLS) {
            error = brushRegionTooLarge(rect.area());
            return false;
        }
        out.reserve((size_t)rect.area());
        for (int64_t y = rect.minY; y <= rect.maxY; y++) {
            for (int64_t x = rect.minX; x <= rect.maxX; x++) out.push_back(CellCoord{(int32_t)x, (int32_t)y});
        }
        return true;
    }

    bool floodCells(const BrushTarget& target, CellCoord start, FloodScope scope, std::vector<CellCoord>& out, PixelProblem& error) {
        out.clear();
        const uint32_t wanted = target.cells.get(start) & kBrushFloodMask;
        if (scope == FloodScope::EveryEqual) {
            const CellRect rect = target.cells.usedRect();
            if (rect.area() > MAX_TILE_REGION_CELLS) {
                error = brushRegionTooLarge(rect.area());
                return false;
            }
            target.cells.forEachCell(rect, [&](CellCoord cell, uint32_t value, uint8_t) {
                if ((value & kBrushFloodMask) == wanted) out.push_back(cell);
            });
            return true;
        }
        std::unordered_set<CellCoord, CellCoordHash> visited;
        visited.insert(start);
        out.push_back(start);
        std::vector<CellCoord> around;
        for (size_t head = 0; head < out.size(); head++) {
            around.clear();
            tilegrid::sideNeighbours(target.grid, out[head], around);
            for (const CellCoord& next : around) {
                if (visited.count(next) != 0 || (target.cells.get(next) & kBrushFloodMask) != wanted) continue;
                if ((int64_t)out.size() >= MAX_TILE_REGION_CELLS) {
                    error = brushRegionTooLarge(MAX_TILE_REGION_CELLS + 1);
                    out.clear();
                    return false;
                }
                visited.insert(next);
                out.push_back(next);
            }
        }
        return true;
    }

    bool paintStamp(const BrushTarget& target, const BrushStamp& stamp, CellCoord anchor, const PaintOptions& options, BrushEdit& edit) {
        edit.error = PixelProblem();
        if (!brushCheckStamp(target, stamp, edit)) return false;
        std::vector<BrushWrite> writes;
        writes.reserve(stamp.cells.size());
        for (const BrushCell& cell : stamp.cells) {
            writes.emplace_back(CellCoord{(int32_t)(anchor.x + cell.x), (int32_t)(anchor.y + cell.y)},
                                makeCellValue(cell.tile, cell.flags & TILE_TRANSFORM_MASK));
        }
        return brushWriteTiles(target, writes, options, edit);
    }

    bool boxFill(const BrushTarget& target, const BrushStamp& stamp, const CellRect& rect, const PaintOptions& options, BrushEdit& edit) {
        edit.error = PixelProblem();
        if (!brushCheckStamp(target, stamp, edit)) return false;
        std::vector<CellCoord> cells;
        if (!boxCells(rect, cells, edit.error)) return false;
        return brushFillCells(target, stamp, CellCoord{rect.minX, rect.minY}, cells, options, edit);
    }

    bool paintLine(const BrushTarget& target, const BrushStamp& stamp, CellCoord from, CellCoord to, const PaintOptions& options, BrushEdit& edit) {
        edit.error = PixelProblem();
        if (!brushCheckStamp(target, stamp, edit)) return false;
        if (tilegrid::distance(target.grid, from, to) >= MAX_TILE_REGION_CELLS) {
            edit.error = brushRegionTooLarge(tilegrid::distance(target.grid, from, to));
            return false;
        }
        const std::vector<CellCoord> cells = tilegrid::line(target.grid, from, to);
        return brushFillCells(target, stamp, from, cells, options, edit);
    }

    bool floodFill(const BrushTarget& target, const BrushStamp& stamp, CellCoord start, FloodScope scope, const PaintOptions& options, BrushEdit& edit) {
        edit.error = PixelProblem();
        if (!brushCheckStamp(target, stamp, edit)) return false;
        std::vector<CellCoord> cells;
        if (!floodCells(target, start, scope, cells, edit.error)) return false;
        return brushFillCells(target, stamp, start, cells, options, edit);
    }

    bool paintRandom(const BrushTarget& target, std::span<const RandomEntry> entries, uint32_t strokeSeed, std::span<const CellCoord> cells,
                     const PaintOptions& options, BrushEdit& edit) {
        edit.error = PixelProblem();
        if (!brushHasTileset(target, edit)) return false;
        std::vector<float> weights;
        weights.reserve(entries.size());
        for (size_t i = 0; i < entries.size(); i++) {
            const RandomEntry& entry = entries[i];
            const std::string field = "entries[" + std::to_string(i) + "]";
            if (!brushCheckTile(target, entry.tile, entry.flags, field.c_str(), edit)) return false;
            weights.push_back(entry.weight >= 0.0f ? entry.weight : target.tileset->findTile(entry.tile)->probability);
        }
        if ((int64_t)cells.size() > MAX_TILE_REGION_CELLS) {
            edit.error = brushRegionTooLarge((int64_t)cells.size());
            return false;
        }
        const bool anyPositive = std::any_of(weights.begin(), weights.end(), [](float w) { return w > 0.0f; });
        if (!anyPositive) {
            edit.error = brushProblem("unknown_tile", "the random brush has no tile with a weight above 0",
                                      "give at least one tile a positive weight or probability", "entries");
            return false;
        }
        std::vector<BrushWrite> writes;
        writes.reserve(cells.size());
        for (const CellCoord& cell : cells) {
            const int pick = tileset::weightedPick(weights.data(), weights.size(), tileset::cellHash(target.seed, strokeSeed, cell.x, cell.y));
            if (pick < 0) continue;      // no positive weight: refused above
            const RandomEntry& entry = entries[(size_t)pick];
            writes.emplace_back(cell, makeCellValue(entry.tile, entry.flags & TILE_TRANSFORM_MASK));
        }
        return brushWriteTiles(target, writes, options, edit);
    }

    bool eraseCell(const BrushTarget& target, CellCoord cell, BrushEdit& edit) {
        edit.error = PixelProblem();
        return brushErase(target, std::span<const CellCoord>(&cell, 1), edit);
    }

    bool eraseBox(const BrushTarget& target, const CellRect& rect, BrushEdit& edit) {
        edit.error = PixelProblem();
        std::vector<CellCoord> cells;
        if (!boxCells(rect, cells, edit.error)) return false;
        return brushErase(target, cells, edit);
    }

    bool eraseLine(const BrushTarget& target, CellCoord from, CellCoord to, BrushEdit& edit) {
        edit.error = PixelProblem();
        if (tilegrid::distance(target.grid, from, to) >= MAX_TILE_REGION_CELLS) {
            edit.error = brushRegionTooLarge(tilegrid::distance(target.grid, from, to));
            return false;
        }
        return brushErase(target, tilegrid::line(target.grid, from, to), edit);
    }

    bool eraseFlood(const BrushTarget& target, CellCoord start, FloodScope scope, BrushEdit& edit) {
        edit.error = PixelProblem();
        std::vector<CellCoord> cells;
        if (!floodCells(target, start, scope, cells, edit.error)) return false;
        return brushErase(target, cells, edit);
    }

    bool paintTerrain(const BrushTarget& target, std::span<const CellCoord> cells, int terrainIndex, BrushEdit& edit) {
        edit.error = PixelProblem();
        if (!brushHasTileset(target, edit)) return false;
        if (terrainIndex != NO_TERRAIN && !target.tileset->terrain(terrainIndex)) {
            edit.error = brushProblem("unknown_terrain", "the tileset has no terrain " + std::to_string(terrainIndex),
                                      "use a terrain index or name the tileset has", "terrain");
            return false;
        }
        if ((int64_t)cells.size() > MAX_TILE_REGION_CELLS) {
            edit.error = brushRegionTooLarge((int64_t)cells.size());
            return false;
        }
        std::string solverError;
        if (!terrain::paint(target.cells, brushTerrainContext(target), cells, terrainIndex, edit.patch, edit.terrain, solverError)) {
            edit.error = brushProblem("layout_mismatch", solverError, "use a terrain set the layer's Grid and Dual Grid setting accept");
            return false;
        }
        return true;
    }

    bool refreshTerrain(const BrushTarget& target, const CellRect& rect, BrushEdit& edit) {
        edit.error = PixelProblem();
        if (!brushHasTileset(target, edit)) return false;
        const bool hexShape = target.tileset->shape == TileShape::HexPointy || target.tileset->shape == TileShape::HexFlat;
        if ((target.grid.layout == TileGridLayout::Hexagon) != hexShape ||
            (target.dualGrid && target.grid.layout != TileGridLayout::Rectangle && target.grid.layout != TileGridLayout::Isometric)) {
            edit.error = brushProblem("layout_mismatch", "the tileset's tile shape or the layer's Dual Grid setting does not fit the Grid",
                                      "use a tileset made for this layout");
            return false;
        }
        terrain::refresh(target.cells, brushTerrainContext(target), rect, edit.patch, edit.terrain);
        return true;
    }

    BrushStamp pick(const BrushTarget& target, CellCoord cell) {
        BrushStamp stamp;
        const uint32_t value = target.cells.get(cell);
        if (!cellValueEmpty(value)) stamp.cells.push_back(BrushCell{0, 0, cellValueTile(value), cellValueFlags(value) & TILE_TRANSFORM_MASK});
        return stamp;
    }

    bool copyRect(const BrushTarget& target, const CellRect& rect, BrushStamp& out, PixelProblem& error) {
        out = BrushStamp();
        if (rect.area() > MAX_TILE_REGION_CELLS) {
            error = brushRegionTooLarge(rect.area());
            return false;
        }
        if (rect.isEmpty()) return true;
        out.width = (int)rect.width();
        out.height = (int)rect.height();
        target.cells.forEachCell(rect, [&](CellCoord cell, uint32_t value, uint8_t) {
            if (!cellValueEmpty(value)) {
                out.cells.push_back(BrushCell{cell.x - rect.minX, cell.y - rect.minY, cellValueTile(value), cellValueFlags(value) & TILE_TRANSFORM_MASK});
            }
        });
        return true;
    }

    bool deleteRect(const BrushTarget& target, const CellRect& rect, BrushEdit& edit) {
        edit.error = PixelProblem();
        std::vector<CellCoord> cells;
        if (!boxCells(rect, cells, edit.error)) return false;
        // the logical plane of a dual-grid layer is not what the selection shows
        if (!target.dualGrid) {
            std::vector<CellCoord> withTerrain;
            for (const CellCoord& cell : cells) {
                if (target.cells.getTerrainValue(cell) != TERRAIN_NONE_VALUE) withTerrain.push_back(cell);
            }
            if (!withTerrain.empty() && target.tileset) {
                std::string solverError;
                if (!terrain::paint(target.cells, brushTerrainContext(target), withTerrain, NO_TERRAIN, edit.patch, edit.terrain, solverError)) {
                    edit.error = brushProblem("layout_mismatch", solverError, "use a tileset made for this layout");
                    return false;
                }
            } else {
                for (const CellCoord& cell : withTerrain) edit.patch.setTerrainValue(target.cells, cell, TERRAIN_NONE_VALUE);
            }
        }
        return brushErase(target, cells, edit);
    }

    bool moveRect(const BrushTarget& target, const CellRect& rect, CellCoord delta, BrushEdit& edit) {
        edit.error = PixelProblem();
        if (rect.area() > MAX_TILE_REGION_CELLS) {
            edit.error = brushRegionTooLarge(rect.area());
            return false;
        }
        if (rect.isEmpty() || (delta.x == 0 && delta.y == 0)) return true;
        const bool moveTerrain = !target.dualGrid;
        struct Moved {
            CellCoord from;
            CellCoord to;
            uint32_t value;
            uint8_t terrain;
        };
        std::vector<Moved> moved;
        target.cells.forEachCell(rect, [&](CellCoord cell, uint32_t value, uint8_t terrain) {
            if (!cellValueEmpty(value) || (moveTerrain && terrain != TERRAIN_NONE_VALUE)) {
                moved.push_back(Moved{cell, CellCoord{(int32_t)(cell.x + delta.x), (int32_t)(cell.y + delta.y)}, value, terrain});
            }
        });
        if (moved.empty()) return true;
        std::unordered_set<CellCoord, CellCoordHash> landing;
        landing.reserve(moved.size());
        for (const Moved& cell : moved) landing.insert(cell.to);
        bool terrainChanged = false;
        for (const Moved& cell : moved) {
            if (landing.count(cell.from) != 0) continue;
            edit.patch.setValue(target.cells, cell.from, 0);
            if (moveTerrain) {
                terrainChanged = terrainChanged || cell.terrain != TERRAIN_NONE_VALUE;
                edit.patch.setTerrainValue(target.cells, cell.from, TERRAIN_NONE_VALUE);
            }
        }
        for (const Moved& cell : moved) {
            edit.patch.setValue(target.cells, cell.to, cell.value);
            if (moveTerrain) {
                terrainChanged = terrainChanged || target.cells.getTerrainValue(cell.to) != TERRAIN_NONE_VALUE || cell.terrain != TERRAIN_NONE_VALUE;
                edit.patch.setTerrainValue(target.cells, cell.to, cell.terrain);
            }
        }
        // the autotile around both places sees the terrain that came and went (D6: tiles derive
        // from the plane); without a tileset there is nothing to choose from
        if (terrainChanged && target.tileset) {
            const auto refreshAround = [&](const CellRect& area) {
                CellRect grown = area;
                grown.minX -= 1; grown.minY -= 1; grown.maxX += 1; grown.maxY += 1;
                terrain::refresh(target.cells, brushTerrainContext(target), grown, edit.patch, edit.terrain);
            };
            refreshAround(rect);
            CellRect landed = rect;
            landed.minX += delta.x; landed.maxX += delta.x;
            landed.minY += delta.y; landed.maxY += delta.y;
            refreshAround(landed);
        }
        return true;
    }

}
