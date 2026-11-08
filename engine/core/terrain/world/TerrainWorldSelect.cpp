// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/world/TerrainWorldSelect.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    size_t levelOffset(int level) {
        size_t offset = 0;
        for (int l = 0; l < level; l++) {
            const size_t side = static_cast<size_t>(WORLD_TILES_PER_SIDE >> l);
            offset += side * side;
        }
        return offset;
    }

    struct Box {
        float minX, minY, minZ, maxX, maxY, maxZ;
    };

    struct Walker {
        const WorldSelectParams& params;
        const WorldOccupancy& occupancy;
        WorldSelection& out;
        int tileLevel = 0;
        float tileSize = 0.0f;
        float ranges[WorldOccupancy::LEVELS + 12] = {};

        struct Pending {
            TileCoord tile;
            float distance;
        };
        std::vector<Pending> pending;

        float distanceSquared(const Box& box) const {
            const float dx = params.eyeX < box.minX ? box.minX - params.eyeX : (params.eyeX > box.maxX ? params.eyeX - box.maxX : 0.0f);
            const float dy = params.eyeY < box.minY ? box.minY - params.eyeY : (params.eyeY > box.maxY ? params.eyeY - box.maxY : 0.0f);
            const float dz = params.eyeZ < box.minZ ? box.minZ - params.eyeZ : (params.eyeZ > box.maxZ ? params.eyeZ - box.maxZ : 0.0f);
            return dx * dx + dy * dy + dz * dz;
        }

        bool inSphere(float radius, const Box& box) const {
            return distanceSquared(box) <= radius * radius;
        }

        bool inFrustum(const Box& box) const {
            const float cx = (box.minX + box.maxX) * 0.5f, cy = (box.minY + box.maxY) * 0.5f, cz = (box.minZ + box.maxZ) * 0.5f;
            const float hx = (box.maxX - box.minX) * 0.5f, hy = (box.maxY - box.minY) * 0.5f, hz = (box.maxZ - box.minZ) * 0.5f;
            for (const auto& plane : params.planes) {
                if (plane[0] == 0.0f && plane[1] == 0.0f && plane[2] == 0.0f && plane[3] == 0.0f) continue;
                const float distance = plane[0] * cx + plane[1] * cy + plane[2] * cz + plane[3];
                const float reach = std::fabs(plane[0]) * hx + std::fabs(plane[1]) * hy + std::fabs(plane[2]) * hz;
                if (distance < -reach) return false;
            }
            return true;
        }

        Box boxOf(float minX, float minZ, float size, uint16_t low, uint16_t high) const {
            const float scale = params.maxHeight / 65535.0f;
            return Box{minX, static_cast<float>(low) * scale, minZ, minX + size, static_cast<float>(high) * scale, minZ + size};
        }

        bool push(std::vector<WorldSelectNode>& grid, float minX, float minZ, int level, float range, bool half) {
            if (static_cast<int>(grid.size()) >= params.maxNodesPerGrid) {
                out.overflow = true;
                return false;
            }
            const float size = params.lattice.nodeSize(level);
            WorldSelectNode node;
            node.x = minX + size * 0.5f;
            node.z = minZ + size * 0.5f;
            node.size = size;
            node.range = range;
            node.resolution = static_cast<float>(half ? params.lattice.gridResolution / 2 : params.lattice.gridResolution);
            node.level = level;
            grid.push_back(node);
            return true;
        }

        // level >= T, in tile units: the node covers 2^(level - T) tiles from (tileX, tileZ)
        float nodeMin(int tile) const { return static_cast<float>(tile) * tileSize; }

        bool full(int l, int tileX, int tileZ) const {
            const uint32_t span = 1u << l;
            return occupancy.count(l, tileX, tileZ) == span * span;
        }

        void forceOverview(int level, int tileX, int tileZ) {
            const int l = level - tileLevel;
            if (occupancy.count(l, tileX, tileZ) == 0) return;
            out.visited++;
            if (full(l, tileX, tileZ)) {
                push(out.overviewHalf, nodeMin(tileX), nodeMin(tileZ), level, WORLD_INFINITE_RANGE, true);
                return;
            }
            const int half = 1 << (l - 1);
            for (int k = 0; k < 4; k++) forceOverview(level - 1, tileX + (k % 2) * half, tileZ + (k / 2) * half);
        }

        bool visitAbove(int level, int tileX, int tileZ) {
            const int l = level - tileLevel;
            const uint32_t count = occupancy.count(l, tileX, tileZ);
            if (count == 0) return true;
            out.visited++;
            const TileMinMax heights = occupancy.minMax(l, tileX, tileZ);
            const float size = params.lattice.nodeSize(level);
            const Box box = boxOf(nodeMin(tileX), nodeMin(tileZ), size, heights.min, heights.max);
            if (!inSphere(ranges[level], box)) return false;
            if (!inFrustum(box)) return true;
            if (l == 0) {
                pending.push_back(Pending{TileCoord(tileX, tileZ), distanceSquared(box)});
                return true;
            }
            const int half = 1 << (l - 1);
            const bool isFull = full(l, tileX, tileZ);
            if (isFull && !inSphere(ranges[level - 1], box)) {
                push(out.overviewFull, box.minX, box.minZ, level, ranges[level], false);
                return true;
            }
            for (int k = 0; k < 4; k++) {
                const int childX = tileX + (k % 2) * half, childZ = tileZ + (k / 2) * half;
                if (visitAbove(level - 1, childX, childZ)) continue;
                // out of its own range: drawn coarser here, or tile by tile where it is not all there
                if (isFull) {
                    push(out.overviewHalf, nodeMin(childX), nodeMin(childZ), level - 1, ranges[level], true);
                } else {
                    forceOverview(level - 1, childX, childZ);
                }
            }
            return true;
        }

        bool walkNear(const TileData& tile, float originX, float originZ, int level, int cellX, int cellZ, WorldTileDraws& draws) {
            out.visited++;
            const TileMinMax heights = tileNodeMinMax(tile, params.lattice, level, cellX, cellZ);
            const float size = params.lattice.nodeSize(level);
            const float minX = originX + static_cast<float>(cellX) * size, minZ = originZ + static_cast<float>(cellZ) * size;
            const Box box = boxOf(minX, minZ, size, heights.min, heights.max);
            if (!inSphere(ranges[level], box)) return false;
            if (!inFrustum(box)) return true;
            if (level == 0 || !inSphere(ranges[level - 1], box)) {
                push(draws.full, minX, minZ, level, ranges[level], false);
                return true;
            }
            for (int k = 0; k < 4; k++) {
                const int childX = cellX * 2 + (k % 2), childZ = cellZ * 2 + (k / 2);
                if (!walkNear(tile, originX, originZ, level - 1, childX, childZ, draws)) {
                    push(draws.half, originX + static_cast<float>(childX) * size * 0.5f, originZ + static_cast<float>(childZ) * size * 0.5f, level - 1,
                         ranges[level], true);
                }
            }
            return true;
        }
    };

}

// ------------------------------------------------------------------ WorldOccupancy

size_t WorldOccupancy::index(int level, int tileX, int tileZ) const {
    const int side = WORLD_TILES_PER_SIDE >> level;
    const int nx = (tileX - WORLD_TILE_MIN) >> level;
    const int nz = (tileZ - WORLD_TILE_MIN) >> level;
    return levelOffset(level) + static_cast<size_t>(nz) * side + nx;
}

void WorldOccupancy::refresh(int level, int tileX, int tileZ) {
    const size_t at = index(level, tileX, tileZ);
    const int half = 1 << (level - 1);
    const int baseX = WORLD_TILE_MIN + (((tileX - WORLD_TILE_MIN) >> level) << level);
    const int baseZ = WORLD_TILE_MIN + (((tileZ - WORLD_TILE_MIN) >> level) << level);
    uint32_t total = 0;
    uint16_t low = 0xFFFF, high = 0;
    for (int k = 0; k < 4; k++) {
        const size_t child = index(level - 1, baseX + (k % 2) * half, baseZ + (k / 2) * half);
        if (counts[child] == 0) continue;
        total += counts[child];
        low = std::min(low, mins[child]);
        high = std::max(high, maxs[child]);
    }
    counts[at] = total;
    mins[at] = low;
    maxs[at] = high;
}

void WorldOccupancy::setTile(TileCoord tile, bool present, uint16_t minHeight, uint16_t maxHeight) {
    if (!tile.addressable()) return;
    if (counts.empty()) {
        const size_t total = levelOffset(LEVELS);
        counts.assign(total, 0);
        mins.assign(total, 0xFFFF);
        maxs.assign(total, 0);
    }
    const size_t at = index(0, tile.x, tile.z);
    if (counts[at] != 0 && !present) tiles--;
    if (counts[at] == 0 && present) tiles++;
    counts[at] = present ? 1 : 0;
    mins[at] = present ? minHeight : 0xFFFF;
    maxs[at] = present ? maxHeight : 0;
    for (int level = 1; level < LEVELS; level++) refresh(level, tile.x, tile.z);
}

void WorldOccupancy::clear() {
    counts.clear();
    mins.clear();
    maxs.clear();
    tiles = 0;
}

bool WorldOccupancy::present(TileCoord tile) const {
    return !counts.empty() && tile.addressable() && counts[index(0, tile.x, tile.z)] != 0;
}

uint32_t WorldOccupancy::count(int level, int tileX, int tileZ) const {
    if (counts.empty() || level < 0 || level >= LEVELS) return 0;
    if (tileX < WORLD_TILE_MIN || tileZ < WORLD_TILE_MIN || tileX > WORLD_TILE_MAX || tileZ > WORLD_TILE_MAX) return 0;
    return counts[index(level, tileX, tileZ)];
}

TileMinMax WorldOccupancy::minMax(int level, int tileX, int tileZ) const {
    if (count(level, tileX, tileZ) == 0) return TileMinMax{0, 0};
    const size_t at = index(level, tileX, tileZ);
    return TileMinMax{mins[at], maxs[at]};
}

bool WorldOccupancy::bounds(TileCoord& low, TileCoord& high) const {
    if (tiles == 0) return false;
    int minX = WORLD_TILE_MAX, minZ = WORLD_TILE_MAX, maxX = WORLD_TILE_MIN, maxZ = WORLD_TILE_MIN;
    for (const TileCoord& tile : presentTiles()) {
        minX = std::min<int>(minX, tile.x);
        minZ = std::min<int>(minZ, tile.z);
        maxX = std::max<int>(maxX, tile.x);
        maxZ = std::max<int>(maxZ, tile.z);
    }
    low = TileCoord(minX, minZ);
    high = TileCoord(maxX, maxZ);
    return true;
}

std::vector<TileCoord> WorldOccupancy::presentTiles() const {
    std::vector<TileCoord> out;
    if (counts.empty()) return out;
    out.reserve(static_cast<size_t>(tiles));
    for (int z = WORLD_TILE_MIN; z <= WORLD_TILE_MAX; z++) {
        // skip empty rows of 32 tiles at once through level 5
        for (int x = WORLD_TILE_MIN; x <= WORLD_TILE_MAX; x++) {
            if ((x - WORLD_TILE_MIN) % 32 == 0 && count(5, x, z) == 0) {
                x += 31;
                continue;
            }
            if (counts[index(0, x, z)] != 0) out.push_back(TileCoord(x, z));
        }
    }
    return out;
}

// ------------------------------------------------------------------ selection

int WorldSelection::drawItems() const {
    int items = (overviewFull.empty() ? 0 : 1) + (overviewHalf.empty() ? 0 : 1);
    for (const WorldTileDraws& draws : near) items += (draws.full.empty() ? 0 : 1) + (draws.half.empty() ? 0 : 1);
    return items;
}

int WorldSelection::selectedNodes() const {
    size_t nodes = overviewFull.size() + overviewHalf.size();
    for (const WorldTileDraws& draws : near) nodes += draws.full.size() + draws.half.size();
    return static_cast<int>(nodes);
}

void WorldSelection::clear() {
    near.clear();
    overviewFull.clear();
    overviewHalf.clear();
    visited = 0;
    fellBack = 0;
    missingPages = 0;
    overflow = false;
}

float jasperengine::terrain::worldLevelRange(const WorldSelectParams& params, int level) {
    const float range = 2.0f * params.lattice.nodeSize(level) * params.lodDistance;
    if (level >= params.lattice.rootLevel()) return std::max(range, params.farClip);
    return range;
}

void jasperengine::terrain::selectWorld(const WorldSelectParams& params, const WorldOccupancy& occupancy, WorldSelection& out) {
    out.clear();
    Walker walker{params, occupancy, out};
    walker.tileLevel = params.lattice.tileLevel();
    walker.tileSize = params.lattice.tileSize();
    const int root = params.lattice.rootLevel();
    for (int level = 0; level <= root; level++) walker.ranges[level] = worldLevelRange(params, level);

    const int span = WORLD_TILES_PER_SIDE / 2;
    for (int k = 0; k < 4; k++) walker.visitAbove(root, WORLD_TILE_MIN + (k % 2) * span, WORLD_TILE_MIN + (k / 2) * span);

    // D4: tiles nearest first against D; the rest, and those without a page, fall back (R10)
    std::sort(walker.pending.begin(), walker.pending.end(), [](const Walker::Pending& a, const Walker::Pending& b) {
        return a.distance != b.distance ? a.distance < b.distance : a.tile.key() < b.tile.key();
    });
    for (const Walker::Pending& entry : walker.pending) {
        const TileData* tile = static_cast<int>(out.near.size()) < params.maxNearTiles && params.page ? params.page(entry.tile) : nullptr;
        if (tile) {
            WorldTileDraws draws;
            draws.tile = entry.tile;
            walker.walkNear(*tile, walker.nodeMin(entry.tile.x), walker.nodeMin(entry.tile.z), walker.tileLevel, 0, 0, draws);
            if (!draws.full.empty() || !draws.half.empty()) out.near.push_back(std::move(draws));
            continue;
        }
        if (static_cast<int>(out.near.size()) < params.maxNearTiles) out.missingPages++;
        out.fellBack++;
        walker.push(out.overviewHalf, walker.nodeMin(entry.tile.x), walker.nodeMin(entry.tile.z), walker.tileLevel, WORLD_INFINITE_RANGE, true);
    }
}
