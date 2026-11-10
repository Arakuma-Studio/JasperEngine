// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT
//
// The dual_grid_4x4 table (frameByMask, the mask bit order, frame 12) is SpriteCook's
// (skills/spritecook-use-dual-grid-tilesets/SKILL.md), Copyright (c) 2025 SpriteCook, MIT License.
// See NOTICE.md.

#include "tilemap/TerrainTemplates.h"

#include "tilemap/TerrainSolver.h"

#include <algorithm>
#include <map>
#include <set>

using namespace jasperengine;

namespace {

    constexpr int TPL_CR31[7][7] = {
        {0, 4, 92, 124, 116, 80, 0},
        {16, 20, 87, 223, 241, 21, 64},
        {29, 117, 85, 71, 221, 125, 112},
        {31, 253, 113, 28, 127, 247, 209},
        {23, 199, 213, 95, 255, 245, 81},
        {5, 84, 93, 119, 215, 193, 17},
        {0, 1, 7, 197, 69, 68, 65},
    };

    // Godot's bits: TL=1 T=2 TR=4 L=8 C=16 R=32 BL=64 B=128 BR=256; 0 is the blank cell
    constexpr int TPL_GODOT[4][12] = {
        {144, 176, 184, 152, 187, 440, 248, 190, 432, 506, 504, 216},
        {146, 178, 186, 154, 434, 510, 507, 218, 438, 254, 0, 251},
        {18, 50, 58, 26, 182, 447, 255, 155, 446, 511, 443, 219},
        {16, 48, 56, 24, 250, 62, 59, 442, 54, 63, 191, 27},
    };

    constexpr int TPL_DUAL_FRAME_BY_MASK[16] = {-1, 15, 8, 9, 0, 11, 14, 7, 13, 4, 1, 10, 3, 2, 5, 6};
    constexpr int TPL_DUAL_SURFACE_FRAME = 12;

    int tplCr31FromGodot(int godot){
        int cr31 = 0;
        if (godot & 2) cr31 |= 1;
        if (godot & 4) cr31 |= 2;
        if (godot & 32) cr31 |= 4;
        if (godot & 256) cr31 |= 8;
        if (godot & 128) cr31 |= 16;
        if (godot & 64) cr31 |= 32;
        if (godot & 8) cr31 |= 64;
        if (godot & 1) cr31 |= 128;
        return cr31;
    }

    TilesetProblem tplProblem(const char* kind, const std::string& field, const std::string& message, const std::string& fix){
        return TilesetProblem{kind, field, message, fix};
    }

    struct TplAssignment{
        int column = 0;
        int row = 0;
        std::string name;
        TileTerrain terrain;
    };

    void tplBlobPeering(TileTerrain& terrain, int cr31, int on){
        for (int slot = 0; slot < 8; slot++)
            terrain.peering[(size_t)slot] = (cr31 & (1 << slot)) ? (int16_t)on : PEERING_NONE;
    }

    std::vector<TplAssignment> tplAssignments(TerrainTemplate value, int set, int terrain, int second, bool twoSurface){
        std::vector<TplAssignment> out;
        const auto addBlob = [&](int column, int row, int cr31, const std::string& name){
            TplAssignment a;
            a.column = column;
            a.row = row;
            a.name = name;
            a.terrain.set = set;
            a.terrain.center = terrain;
            tplBlobPeering(a.terrain, cr31, terrain);
            out.push_back(std::move(a));
        };
        switch (value){
            case TerrainTemplate::Blob47:{
                std::map<int, int> copies;
                for (int row = 0; row < 7; row++){
                    for (int column = 0; column < 7; column++){
                        const int cr31 = TPL_CR31[row][column];
                        const int copy = copies[cr31]++;
                        addBlob(column, row, cr31, "blob_" + std::to_string(cr31) + (copy ? "_" + std::to_string(copy) : ""));
                    }
                }
                break;
            }
            case TerrainTemplate::Blob47Godot:
                for (int row = 0; row < 4; row++){
                    for (int column = 0; column < 12; column++){
                        if (TPL_GODOT[row][column] == 0)
                            continue;
                        const int cr31 = tplCr31FromGodot(TPL_GODOT[row][column]);
                        addBlob(column, row, cr31, "blob_" + std::to_string(cr31));
                    }
                }
                break;
            case TerrainTemplate::Sides16:
                for (int index = 0; index < 16; index++){
                    TplAssignment a;
                    a.column = index % 4;
                    a.row = index / 4;
                    a.name = "sides_" + std::to_string(index);
                    a.terrain.set = set;
                    a.terrain.center = terrain;
                    for (int bit = 0; bit < 4; bit++)
                        a.terrain.peering[(size_t)(bit * 2)] = (index & (1 << bit)) ? (int16_t)terrain : PEERING_NONE;
                    out.push_back(std::move(a));
                }
                break;
            case TerrainTemplate::DualGrid4x4:{
                static constexpr int CORNER_SLOT[4] = {7, 1, 5, 3};   // TL TR BL BR
                for (int mask = 1; mask < 16; mask++){
                    const int frame = TPL_DUAL_FRAME_BY_MASK[mask];
                    TplAssignment a;
                    a.column = frame % 4;
                    a.row = frame / 4;
                    a.name = "dual_" + std::to_string(mask);
                    a.terrain.set = set;
                    for (int bit = 0; bit < 4; bit++)
                        a.terrain.peering[(size_t)CORNER_SLOT[bit]] = (mask & (1 << bit)) ? (int16_t)terrain : (twoSurface ? (int16_t)second : PEERING_NONE);
                    out.push_back(std::move(a));
                }
                if (twoSurface){
                    TplAssignment a;
                    a.column = TPL_DUAL_SURFACE_FRAME % 4;
                    a.row = TPL_DUAL_SURFACE_FRAME / 4;
                    a.name = "dual_surface";
                    a.terrain.set = set;
                    for (int bit = 0; bit < 4; bit++)
                        a.terrain.peering[(size_t)CORNER_SLOT[bit]] = (int16_t)second;
                    out.push_back(std::move(a));
                }
                break;
            }
        }
        return out;
    }

    std::vector<std::array<int, 2>> tplPairs(const TerrainSetDef& set){
        std::vector<std::array<int, 2>> pairs;
        const int count = (int)set.terrains.size();
        for (int center = 0; center < count; center++){
            pairs.push_back({center, PEERING_NONE});
            for (int other = 0; other < count; other++){
                if (other != center)
                    pairs.push_back({center, other});
            }
        }
        return pairs;
    }

    using TplMask = std::array<int16_t, MAX_PEERING_SLOTS>;
    using TplKey = std::pair<int, TplMask>;

    void tplEnumerateSquareBlob(std::set<TplKey>& out, int center, int16_t other){
        const int16_t own = (int16_t)center;
        for (int sides = 0; sides < 16; sides++){
            TplMask mask;
            mask.fill(PEERING_ANY);
            for (int bit = 0; bit < 4; bit++)
                mask[(size_t)(bit * 2)] = (sides & (1 << bit)) ? own : other;
            std::vector<int> open;
            for (int corner = 1; corner < 8; corner += 2){
                mask[(size_t)corner] = PEERING_NONE;
                if (mask[(size_t)(corner - 1)] == own && mask[(size_t)((corner + 1) % 8)] == own)
                    open.push_back(corner);
            }
            for (int bits = 0; bits < (1 << open.size()); bits++){
                TplMask full = mask;
                for (size_t i = 0; i < open.size(); i++)
                    full[(size_t)open[i]] = (bits & (1 << i)) ? own : other;
                out.insert(TplKey{center, full});
            }
        }
    }

    void tplEnumerateHex(std::set<TplKey>& out, int center, int16_t other, bool blob, int firstSide){
        const int16_t own = (int16_t)center;
        for (int sides = 0; sides < 64; sides++){
            TplMask mask;
            mask.fill(PEERING_ANY);
            for (int side = 0; side < 6; side++)
                mask[(size_t)side] = (sides & (1 << side)) ? own : other;
            if (blob){
                for (int corner = 0; corner < 6; corner++){
                    const int16_t a = mask[(size_t)((corner + firstSide) % 6)];
                    const int16_t b = mask[(size_t)((corner + firstSide + 1) % 6)];
                    mask[(size_t)(6 + corner)] = a == b ? a : PEERING_NONE;
                }
            }
            out.insert(TplKey{center, mask});
        }
    }

    void tplEnumerateDual(std::set<TplKey>& out, int16_t a, int16_t b){
        static constexpr int CORNERS[4] = {7, 1, 5, 3};
        for (int bits = 0; bits < 16; bits++){
            TplMask mask;
            mask.fill(PEERING_ANY);
            for (int corner = 0; corner < 4; corner++)
                mask[(size_t)CORNERS[corner]] = (bits & (1 << corner)) ? a : b;
            // all four corners empty: the display cell draws nothing, no tile is needed
            if (bits != 0 || b != PEERING_NONE)
                out.insert(TplKey{-1, mask});
        }
    }

    bool tplTileMatches(const TplMask& peering, const TplMask& mask, const std::vector<int>& slots){
        for (int slot : slots){
            if (peering[(size_t)slot] != PEERING_ANY && peering[(size_t)slot] != mask[(size_t)slot])
                return false;
        }
        return true;
    }

}

const char* terrain::templateName(TerrainTemplate value){
    switch (value){
        case TerrainTemplate::Blob47: return "blob_47";
        case TerrainTemplate::Blob47Godot: return "blob_47_godot";
        case TerrainTemplate::Sides16: return "sides_16";
        case TerrainTemplate::DualGrid4x4: return "dual_grid_4x4";
    }
    return "blob_47";
}

bool terrain::parseTemplate(std::string_view text, TerrainTemplate& out){
    for (TerrainTemplate value : {TerrainTemplate::Blob47, TerrainTemplate::Blob47Godot, TerrainTemplate::Sides16, TerrainTemplate::DualGrid4x4}){
        if (text == templateName(value)){
            out = value;
            return true;
        }
    }
    return false;
}

void terrain::templateSize(TerrainTemplate value, int& columns, int& rows){
    switch (value){
        case TerrainTemplate::Blob47: columns = 7; rows = 7; return;
        case TerrainTemplate::Blob47Godot: columns = 12; rows = 4; return;
        case TerrainTemplate::Sides16:
        case TerrainTemplate::DualGrid4x4: columns = 4; rows = 4; return;
    }
    columns = 0;
    rows = 0;
}

bool terrain::applyTemplate(TilesetData& data, int set, TerrainTemplate value, int32_t originTile, const TerrainTemplateOptions& options, std::vector<TilesetProblem>& problems){
    if (set < 0 || (size_t)set >= data.terrainSets.size()){
        problems.push_back(tplProblem("unknown_terrain", "set", "there is no terrain set " + std::to_string(set), "use a terrain set of this tileset"));
        return false;
    }
    const TerrainSetDef& setDef = data.terrainSets[(size_t)set];
    const TerrainMode wanted = value == TerrainTemplate::Sides16 ? TerrainMode::Sides : (value == TerrainTemplate::DualGrid4x4 ? TerrainMode::DualGrid : TerrainMode::Blob);
    if (setDef.mode != wanted){
        problems.push_back(tplProblem("invalid_peering", "template", std::string("the template ") + templateName(value) + " needs a " + tileset::terrainModeName(wanted) + " terrain set, this one is " + tileset::terrainModeName(setDef.mode), "choose a template of the set's mode"));
        return false;
    }
    if (data.shape == TileShape::HexPointy || data.shape == TileShape::HexFlat){
        problems.push_back(tplProblem("invalid_peering", "template", "templates cover square and isometric tiles only", "paint the peering values of hexagon tiles by hand"));
        return false;
    }
    const int count = (int)setDef.terrains.size();
    if (options.terrain < 0 || options.terrain >= count){
        problems.push_back(tplProblem("unknown_terrain", "terrain", "the set has no terrain " + std::to_string(options.terrain), "use a terrain index of the set"));
        return false;
    }
    int second = options.secondTerrain;
    if (setDef.twoSurface && value == TerrainTemplate::DualGrid4x4){
        if (second < 0)
            second = options.terrain == 0 ? 1 : 0;
        if (second >= count || second == options.terrain){
            problems.push_back(tplProblem("unknown_terrain", "second_terrain", "a two_surface set needs a second terrain different from the first", "add a second terrain to the set or name another one"));
            return false;
        }
    }

    const TileDef* origin = data.findTile(originTile);
    if (!origin){
        problems.push_back(tplProblem("unknown_tile", "origin", "there is no tile " + std::to_string(originTile), "use the id of the block's top-left tile"));
        return false;
    }
    const TilesetSource* source = data.findSource(origin->source);
    int columns = 0, rows = 0;
    templateSize(value, columns, rows);

    std::map<std::pair<int, int>, TileDef*> byPosition;
    std::vector<TileDef*> inSource;
    for (TileDef& tile : data.tiles){
        if (tile.source != origin->source)
            continue;
        inSource.push_back(&tile);
        byPosition[{tile.atX, tile.atY}] = &tile;
    }
    size_t originIndex = 0;
    while (inSource[originIndex]->id != originTile)
        originIndex++;
    const bool byGrid = source && source->kind == TilesetSourceKind::Texture;

    std::vector<TplAssignment> assignments = tplAssignments(value, set, options.terrain, second, setDef.twoSurface);
    std::vector<TileDef*> targets;
    targets.reserve(assignments.size());
    for (const TplAssignment& assignment : assignments){
        TileDef* target = nullptr;
        if (byGrid){
            auto it = byPosition.find({origin->atX + assignment.column, origin->atY + assignment.row});
            if (it != byPosition.end())
                target = it->second;
        }else{
            const size_t index = originIndex + (size_t)assignment.row * (size_t)columns + (size_t)assignment.column;
            if (index < inSource.size())
                target = inSource[index];
        }
        if (!target){
            problems.push_back(tplProblem("unknown_tile", "origin", "the block has no tile at column " + std::to_string(assignment.column) + ", row " + std::to_string(assignment.row),
                                          "slice the source so that the whole " + std::to_string(columns) + "x" + std::to_string(rows) + " block exists"));
            return false;
        }
        targets.push_back(target);
    }
    for (size_t i = 0; i < assignments.size(); i++){
        targets[i]->terrain = assignments[i].terrain;
        if (options.assignNames)
            targets[i]->name = assignments[i].name;
    }
    return true;
}

TerrainCoverage terrain::coverage(const TilesetData& data, int set, bool allowTransforms){
    TerrainCoverage result;
    if (set < 0 || (size_t)set >= data.terrainSets.size())
        return result;
    const TerrainSetDef& setDef = data.terrainSets[(size_t)set];
    const bool hex = data.shape == TileShape::HexPointy || data.shape == TileShape::HexFlat;
    const int firstSide = data.shape == TileShape::HexFlat ? 5 : 0;
    const std::vector<int> slots = tileset::peeringSlots(data.shape, setDef.mode);

    std::set<TplKey> masks;
    if (setDef.mode == TerrainMode::DualGrid){
        if (hex)
            return result;
        if (setDef.twoSurface && setDef.terrains.size() >= 2){
            tplEnumerateDual(masks, 0, 1);
        }else{
            for (size_t a = 0; a < setDef.terrains.size(); a++){
                tplEnumerateDual(masks, (int16_t)a, PEERING_NONE);
                for (size_t b = a + 1; b < setDef.terrains.size(); b++)
                    tplEnumerateDual(masks, (int16_t)a, (int16_t)b);
            }
        }
    }else{
        for (const std::array<int, 2>& pair : tplPairs(setDef)){
            if (hex)
                tplEnumerateHex(masks, pair[0], (int16_t)pair[1], setDef.mode == TerrainMode::Blob, firstSide);
            else if (setDef.mode == TerrainMode::Blob)
                tplEnumerateSquareBlob(masks, pair[0], (int16_t)pair[1]);
            else{
                for (int sides = 0; sides < 16; sides++){
                    TplMask mask;
                    mask.fill(PEERING_ANY);
                    for (int bit = 0; bit < 4; bit++)
                        mask[(size_t)(bit * 2)] = (sides & (1 << bit)) ? (int16_t)pair[0] : (int16_t)pair[1];
                    masks.insert(TplKey{pair[0], mask});
                }
            }
        }
    }

    result.required = masks.size();
    const bool transforms = allowTransforms && data.shape == TileShape::Square;
    static constexpr uint32_t VARIANTS[8] = {0u, TILE_FLIP_X, TILE_FLIP_Y, TILE_FLIP_X | TILE_FLIP_Y, TILE_TRANSPOSE, TILE_TRANSPOSE | TILE_FLIP_X,
                                             TILE_TRANSPOSE | TILE_FLIP_Y, TILE_TRANSPOSE | TILE_FLIP_X | TILE_FLIP_Y};
    for (const TplKey& key : masks){
        bool found = false;
        for (const TileDef& tile : data.tiles){
            if (tile.terrain.set != set || (setDef.mode != TerrainMode::DualGrid && tile.terrain.center != key.first))
                continue;
            for (int v = 0; v < (transforms ? 8 : 1) && !found; v++)
                found = tplTileMatches(transformPeering(tile.terrain.peering, VARIANTS[v]), key.second, slots);
            if (found)
                break;
        }
        if (!found){
            TerrainMissingMask missing;
            missing.center = key.first;
            missing.peering = key.second;
            result.missing.push_back(missing);
        }
    }
    return result;
}
