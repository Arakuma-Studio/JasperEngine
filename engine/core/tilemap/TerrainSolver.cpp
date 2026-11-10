// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT
//
// The scoring and the blob corner filter port Pixelorama's autotiling
// (src/Classes/Cels/CelTileMap.gd: autotile_find_best_tile, autotile_score_tile,
// autotile_filter_corners). Copyright (c) 2019-present Orama Interactive and contributors, MIT
// License. See NOTICE.md.

#include "tilemap/TerrainSolver.h"

#include <algorithm>
#include <unordered_set>

using namespace jasperengine;

namespace {

    bool solverHexShape(TileShape shape){
        return shape == TileShape::HexPointy || shape == TileShape::HexFlat;
    }

    bool solverContextValid(const TerrainContext& context){
        return context.tileset && (context.grid.layout == TileGridLayout::Hexagon) == solverHexShape(context.tileset->shape);
    }

    bool solverDualLayoutValid(const TileGridSpec& grid){
        return grid.layout == TileGridLayout::Rectangle || grid.layout == TileGridLayout::Isometric;
    }

    bool solverTransformsOn(const TerrainContext& context){
        return context.allowTransforms && context.tileset->shape == TileShape::Square && context.grid.layout == TileGridLayout::Rectangle;
    }

    struct SolverCandidate{
        const TileDef* tile = nullptr;
        std::array<int16_t, MAX_PEERING_SLOTS> peering{};
        uint32_t flags = 0;
    };

    // the tiles of one set, gathered once per paint so a cell does not walk the whole tileset
    struct SolverSet{
        bool built = false;
        std::vector<SolverCandidate> candidates;
        std::vector<int> slots;
    };

    class SolverCache{
    public:
        explicit SolverCache(const TerrainContext& ctx): context(ctx), sets(ctx.tileset->terrainSets.size()){}

        const SolverSet& get(int set){
            SolverSet& entry = sets[(size_t)set];
            if (!entry.built){
                entry.built = true;
                const TilesetData& data = *context.tileset;
                entry.slots = tileset::peeringSlots(data.shape, data.terrainSets[(size_t)set].mode);
                const bool transforms = solverTransformsOn(context);
                for (const TileDef& tile : data.tiles){
                    if (tile.terrain.set != set)
                        continue;
                    addVariants(entry, tile, transforms);
                }
            }
            return entry;
        }

    private:
        const TerrainContext& context;
        std::vector<SolverSet> sets;

        static void addVariants(SolverSet& entry, const TileDef& tile, bool transforms){
            static constexpr uint32_t VARIANTS[8] = {0u, TILE_FLIP_X, TILE_FLIP_Y, TILE_FLIP_X | TILE_FLIP_Y, TILE_TRANSPOSE, TILE_TRANSPOSE | TILE_FLIP_X,
                                                     TILE_TRANSPOSE | TILE_FLIP_Y, TILE_TRANSPOSE | TILE_FLIP_X | TILE_FLIP_Y};
            const size_t first = entry.candidates.size();
            const int count = transforms ? 8 : 1;
            for (int v = 0; v < count; v++){
                SolverCandidate candidate;
                candidate.tile = &tile;
                candidate.flags = VARIANTS[v];
                candidate.peering = terrain::transformPeering(tile.terrain.peering, VARIANTS[v]);
                // a symmetric tile must not get extra lottery tickets or a pointless flip
                bool duplicate = false;
                for (size_t c = first; c < entry.candidates.size() && !duplicate; c++)
                    duplicate = entry.candidates[c].peering == candidate.peering;
                if (!duplicate)
                    entry.candidates.push_back(candidate);
            }
        }
    };

    int solverLocalAt(const TileCells& store, const TilesetData& data, int set, CellCoord cell){
        int cellSet = 0, local = 0;
        return data.terrainOf(store.getTerrain(cell), cellSet, local) && cellSet == set ? local : PEERING_NONE;
    }

    uint32_t solverChoose(const TileCells& store, const TerrainContext& context, SolverCache& cache, CellCoord displayCell, bool& matched){
        const TilesetData& data = *context.tileset;
        matched = true;
        int set = -1, local = -1;
        if (context.dualGrid){
            // the corners decide the set: the first one that holds a dual-grid terrain
            const CellCoord corners[4] = {{displayCell.x - 1, displayCell.y}, {displayCell.x, displayCell.y}, {displayCell.x - 1, displayCell.y - 1}, {displayCell.x, displayCell.y - 1}};
            for (const CellCoord& corner : corners){
                int cornerSet = 0, cornerLocal = 0;
                if (data.terrainOf(store.getTerrain(corner), cornerSet, cornerLocal) && data.terrainSets[(size_t)cornerSet].mode == TerrainMode::DualGrid){
                    set = cornerSet;
                    break;
                }
            }
            if (set < 0)
                return 0;
        }else{
            const int global = store.getTerrain(displayCell);
            if (global == NO_TERRAIN)
                return 0;
            // a terrain the tileset no longer has: the cell keeps it and is reported
            if (!data.terrainOf(global, set, local)){
                matched = false;
                return 0;
            }
        }

        std::array<int16_t, MAX_PEERING_SLOTS> values;
        terrain::mask(store, context, set, displayCell, values);
        const SolverSet& entry = cache.get(set);
        const std::span<const int16_t> maskView(values.data(), values.size());

        int best = INT32_MIN;
        std::vector<const SolverCandidate*> ties;
        for (const SolverCandidate& candidate : entry.candidates){
            if (!context.dualGrid && candidate.tile->terrain.center != local)
                continue;
            TileTerrain shaped;
            shaped.peering = candidate.peering;
            const int value = terrain::score(shaped, maskView, entry.slots);
            if (value > best){
                best = value;
                ties.clear();
            }
            if (value == best)
                ties.push_back(&candidate);
        }
        if (ties.empty()){
            matched = false;
            return 0;
        }
        const SolverCandidate* chosen = ties[0];
        if (ties.size() > 1){
            std::vector<float> weights(ties.size());
            for (size_t i = 0; i < ties.size(); i++)
                weights[i] = ties[i]->tile->probability;
            const int pick = tileset::weightedPick(weights.data(), weights.size(), tileset::cellHash(context.seed, 0, displayCell.x, displayCell.y));
            if (pick >= 0)
                chosen = ties[(size_t)pick];
        }
        return makeCellValue(chosen->tile->id, chosen->flags);
    }

    // a display cell's tile from the plane, recorded in the patch
    void solverRechoose(TileCells& store, const TerrainContext& context, SolverCache& cache, CellCoord cell, TileCellPatch& patch, TerrainReport& report){
        const uint32_t current = store.get(cell);
        if (current & TILE_LOCK)
            return;
        if (!context.dualGrid && store.getTerrain(cell) == NO_TERRAIN)
            return;
        bool matched = true;
        const uint32_t chosen = solverChoose(store, context, cache, cell, matched);
        report.rechosen++;
        if (!matched)
            report.noTile.push_back(cell);
        if (chosen != current)
            patch.setValue(store, cell, chosen);
    }

    class SolverAffected{
    public:
        void add(CellCoord cell){
            if (seen.insert(cell).second)
                order.push_back(cell);
        }
        const std::vector<CellCoord>& cells() const { return order; }

    private:
        // the set only answers "seen?"; the order, and so the patch, comes from the input
        std::unordered_set<CellCoord, CellCoordHash> seen;
        std::vector<CellCoord> order;
    };

    // R25: the display cells whose mask can depend on the terrain of `cell` under `mode`
    void solverAddNeighbourhood(const TileGridSpec& grid, TerrainMode mode, bool dualGrid, CellCoord cell, SolverAffected& affected){
        if (dualGrid){
            affected.add(CellCoord{cell.x, cell.y});
            affected.add(CellCoord{cell.x + 1, cell.y});
            affected.add(CellCoord{cell.x, cell.y + 1});
            affected.add(CellCoord{cell.x + 1, cell.y + 1});
            return;
        }
        affected.add(cell);
        if (grid.layout == TileGridLayout::Hexagon){
            for (int direction = 0; direction < 6; direction++)
                affected.add(tilegrid::neighbour(grid, cell, direction));
            return;
        }
        const int step = mode == TerrainMode::Sides ? 2 : 1;
        for (int direction = 0; direction < 8; direction += step)
            affected.add(tilegrid::neighbour(grid, cell, direction));
    }

}

TileCellPatch::Entry& TileCellPatch::entry(const TileCells& cells, CellCoord cell){
    auto it = entries.find(cell);
    if (it != entries.end())
        return it->second;
    const uint32_t value = cells.get(cell);
    const uint8_t terrain = cells.getTerrainValue(cell);
    order.push_back(cell);
    return entries.emplace(cell, Entry{value, value, terrain, terrain}).first->second;
}

void TileCellPatch::setValue(TileCells& cells, CellCoord cell, uint32_t value){
    Entry& e = entry(cells, cell);
    cells.set(cell, value);
    e.newValue = cells.get(cell);
}

void TileCellPatch::setTerrainValue(TileCells& cells, CellCoord cell, uint8_t terrain){
    Entry& e = entry(cells, cell);
    cells.setTerrainValue(cell, terrain);
    e.newTerrain = cells.getTerrainValue(cell);
}

std::vector<TileCellChange> TileCellPatch::changes() const{
    std::vector<TileCellChange> out;
    out.reserve(order.size());
    for (const CellCoord& cell : order){
        const Entry& e = entries.at(cell);
        if (e.oldValue != e.newValue || e.oldTerrain != e.newTerrain)
            out.push_back(TileCellChange{cell, e.oldValue, e.newValue, e.oldTerrain, e.newTerrain});
    }
    return out;
}

void TileCellPatch::revert(TileCells& cells) const{
    for (const CellCoord& cell : order){
        const Entry& e = entries.at(cell);
        cells.set(cell, e.oldValue);
        cells.setTerrainValue(cell, e.oldTerrain);
    }
}

void TileCellPatch::clear(){
    entries.clear();
    order.clear();
}

bool TileCellPatch::contains(CellCoord cell) const{
    return entries.find(cell) != entries.end();
}

std::array<int16_t, MAX_PEERING_SLOTS> terrain::transformPeering(const std::array<int16_t, MAX_PEERING_SLOTS>& peering, uint32_t flags){
    // the directions as image steps (x right, y down), in slot order n ne e se s sw w nw
    static constexpr int STEPS[8][2] = {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};
    if ((flags & TILE_TRANSFORM_MASK) == 0)
        return peering;
    std::array<int16_t, MAX_PEERING_SLOTS> out = peering;
    for (int slot = 0; slot < 8; slot++){
        int x = STEPS[slot][0], y = STEPS[slot][1];
        // Tiled's order: the transpose applies first, then the flips
        if (flags & TILE_TRANSPOSE)
            std::swap(x, y);
        if (flags & TILE_FLIP_X)
            x = -x;
        if (flags & TILE_FLIP_Y)
            y = -y;
        for (int target = 0; target < 8; target++){
            if (STEPS[target][0] == x && STEPS[target][1] == y){
                out[(size_t)target] = peering[(size_t)slot];
                break;
            }
        }
    }
    return out;
}

int terrain::score(const TileTerrain& tile, std::span<const int16_t> mask, std::span<const int> slots){
    int total = 0;
    for (int slot : slots){
        const int16_t wanted = tile.peering[(size_t)slot];
        if (wanted == PEERING_ANY)
            continue;
        total += wanted == mask[(size_t)slot] ? 10 : -2;
        total += 1;
    }
    return total;
}

void terrain::mask(const TileCells& store, const TerrainContext& context, int set, CellCoord displayCell, std::array<int16_t, MAX_PEERING_SLOTS>& out){
    out.fill(PEERING_NONE);
    if (!solverContextValid(context) || set < 0 || (size_t)set >= context.tileset->terrainSets.size())
        return;
    const TilesetData& data = *context.tileset;
    const TerrainMode mode = data.terrainSets[(size_t)set].mode;
    const auto at = [&](CellCoord cell){
        return (int16_t)solverLocalAt(store, data, set, cell);
    };
    if (mode == TerrainMode::DualGrid){
        out[1] = at(CellCoord{displayCell.x, displayCell.y});
        out[3] = at(CellCoord{displayCell.x, displayCell.y - 1});
        out[5] = at(CellCoord{displayCell.x - 1, displayCell.y - 1});
        out[7] = at(CellCoord{displayCell.x - 1, displayCell.y});
        return;
    }
    if (context.grid.layout == TileGridLayout::Hexagon){
        for (int side = 0; side < 6; side++)
            out[(size_t)side] = at(tilegrid::neighbour(context.grid, displayCell, side));
        if (mode == TerrainMode::Blob){
            // pointy-top sides start at east, on a corner's clockwise side; flat-top sides start
            // at north-east, just past the first corner (c_e)
            const int firstSide = data.shape == TileShape::HexFlat ? 5 : 0;
            for (int corner = 0; corner < 6; corner++){
                const int16_t a = out[(size_t)((corner + firstSide) % 6)];
                const int16_t b = out[(size_t)((corner + firstSide + 1) % 6)];
                out[(size_t)(6 + corner)] = a == b ? a : PEERING_NONE;
            }
        }
        return;
    }
    for (int slot = 0; slot < 8; slot += mode == TerrainMode::Sides ? 2 : 1)
        out[(size_t)slot] = at(tilegrid::neighbour(context.grid, displayCell, slot));
    if (mode == TerrainMode::Blob){
        const int16_t own = at(displayCell);
        for (int corner = 1; corner < 8; corner += 2){
            if (out[(size_t)(corner - 1)] != own || out[(size_t)((corner + 1) % 8)] != own)
                out[(size_t)corner] = PEERING_NONE;
        }
    }
}

uint32_t terrain::chooseTile(const TileCells& store, const TerrainContext& context, CellCoord displayCell, bool& matched){
    matched = false;
    if (!solverContextValid(context))
        return 0;
    SolverCache cache(context);
    return solverChoose(store, context, cache, displayCell, matched);
}

bool terrain::paint(TileCells& store, const TerrainContext& context, std::span<const CellCoord> cells, int terrainIndex, TileCellPatch& patch, TerrainReport& report, std::string& error){
    if (!context.tileset){
        error = "no tileset";
        return false;
    }
    if (!solverContextValid(context)){
        error = "the tileset's tile shape does not match the Grid's layout";
        return false;
    }
    if (context.dualGrid && !solverDualLayoutValid(context.grid)){
        error = "a dual-grid layer needs a Rectangle or Isometric Grid";
        return false;
    }
    const TilesetData& data = *context.tileset;
    int newSet = -1, newLocal = -1;
    TerrainMode newMode = TerrainMode::Blob;
    if (terrainIndex != NO_TERRAIN){
        if (terrainIndex >= MAX_TILESET_TERRAINS || !data.terrainOf(terrainIndex, newSet, newLocal)){
            error = "this tileset has no terrain " + std::to_string(terrainIndex);
            return false;
        }
        newMode = data.terrainSets[(size_t)newSet].mode;
        if (context.dualGrid != (newMode == TerrainMode::DualGrid)){
            error = context.dualGrid ? "a dual-grid layer takes dual_grid terrain sets only" : "a dual_grid terrain set needs a layer with Dual Grid on";
            return false;
        }
    }
    const uint8_t newValue = terrainIndex == NO_TERRAIN ? TERRAIN_NONE_VALUE : (uint8_t)(terrainIndex + 1);

    // the plane first, entirely: every mask below must see the finished stroke
    SolverAffected affected;
    for (const CellCoord& cell : cells){
        const uint8_t oldValue = store.getTerrainValue(cell);
        if (oldValue == newValue)
            continue;
        int oldSet = -1, oldLocal = -1;
        const bool hadOld = oldValue != TERRAIN_NONE_VALUE && data.terrainOf((int)oldValue - 1, oldSet, oldLocal);
        patch.setTerrainValue(store, cell, newValue);
        if (hadOld)
            solverAddNeighbourhood(context.grid, data.terrainSets[(size_t)oldSet].mode, context.dualGrid, cell, affected);
        if (newSet >= 0)
            solverAddNeighbourhood(context.grid, newMode, context.dualGrid, cell, affected);
        if (!context.dualGrid && newValue == TERRAIN_NONE_VALUE && !(store.get(cell) & TILE_LOCK)){
            if (store.get(cell) != 0)
                patch.setValue(store, cell, 0);
            report.rechosen++;
        }
    }
    SolverCache cache(context);
    for (const CellCoord& cell : affected.cells())
        solverRechoose(store, context, cache, cell, patch, report);
    return true;
}

void terrain::refresh(TileCells& store, const TerrainContext& context, const CellRect& rect, TileCellPatch& patch, TerrainReport& report){
    if (!solverContextValid(context) || rect.isEmpty())
        return;
    if (context.dualGrid && !solverDualLayoutValid(context.grid))
        return;
    SolverAffected affected;
    const int size = store.getChunkSize();
    for (const ChunkCoord& coord : store.sortedChunks()){
        const TileChunk* chunk = store.chunk(coord);
        if (chunk->terrain.empty())
            continue;
        const CellRect area = store.chunkRect(coord);
        // a logical cell also reaches the display cells one step up and right of it
        const int64_t margin = context.dualGrid ? 1 : 0;
        if ((int64_t)area.maxX + margin < rect.minX || area.minX > rect.maxX || (int64_t)area.maxY + margin < rect.minY || area.minY > rect.maxY)
            continue;
        const CellCoord origin = store.chunkOrigin(coord);
        for (int i = 0; i < size * size; i++){
            if (chunk->terrain[(size_t)i] == TERRAIN_NONE_VALUE)
                continue;
            const CellCoord cell{origin.x + i % size, origin.y + i / size};
            if (!context.dualGrid){
                if (rect.contains(cell))
                    affected.add(cell);
                continue;
            }
            for (int dy = 0; dy <= 1; dy++){
                for (int dx = 0; dx <= 1; dx++){
                    const CellCoord display{cell.x + dx, cell.y + dy};
                    if (rect.contains(display))
                        affected.add(display);
                }
            }
        }
    }
    SolverCache cache(context);
    for (const CellCoord& cell : affected.cells())
        solverRechoose(store, context, cache, cell, patch, report);
}
