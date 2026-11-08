// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Recast configuration derived from the agent and one cell size follows OpenEngine's
// DetourNavMap.cpp, Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License.
// See NOTICE.md.

#include "NavBakeInput.h"
#include "NavHash.h"

#include "jobs/JobSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::nav;

namespace {

    // cells beyond the agent radius a tile reads past its edge, so erosion near the edge sees the
    // neighbouring geometry (spec D3)
    constexpr int TILE_BORDER_PADDING = 3;
    // Recast's sample values, in world units where they are lengths
    constexpr float MAX_EDGE_LENGTH = 12.0f;
    constexpr float MERGE_REGION_RATIO = 6.25f;
    constexpr float DETAIL_SAMPLE_CELLS = 6.0f;

    int ilog2(unsigned int v) {
        int r = 0;
        while (v > 1) {
            v >>= 1;
            r++;
        }
        return r;
    }

    unsigned int nextPow2(unsigned int v) {
        if (v <= 1)
            return 1;
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        return v + 1;
    }

    bool overlaps(const float* aMin, const float* aMax, const float* bMin, const float* bMax) {
        return aMin[0] <= bMax[0] && aMax[0] >= bMin[0] && aMin[1] <= bMax[1] && aMax[1] >= bMin[1] &&
               aMin[2] <= bMax[2] && aMax[2] >= bMin[2];
    }

}

NavBuildConfig NavBuildConfig::derive(const AgentTypeSettings& agent, NavPlane plane, float cellSize, int tileSize,
                                      float minRegionArea, bool carving, float unitScale) {
    NavBuildConfig config;
    config.plane = plane;
    config.carving = carving;
    const float scale = (plane == NavPlane::XY && unitScale > 0.0f) ? unitScale : 1.0f;

    config.agentRadius = std::max(agent.radius * scale, 1e-4f);
    if (plane == NavPlane::XY) {
        // flat ground, obstacles are marked areas: height and climb only have to keep the
        // ground's spans walkable
        config.agentHeight = 0.0f;
        config.agentClimb = 0.0f;
        config.agentMaxSlope = 45.0f;
    } else {
        config.agentHeight = std::max(agent.height, 1e-3f);
        config.agentClimb = std::max(agent.stepHeight, 0.0f);
        config.agentMaxSlope = std::clamp(agent.maxSlope, 0.0f, 89.9f);
    }

    config.cellSize = cellSize > 0.0f ? cellSize : config.agentRadius / 3.0f;
    config.cellSize = std::max(config.cellSize, 1e-3f);
    config.cellHeight = config.cellSize * 0.5f;
    config.tileSize = std::clamp(tileSize, 16, 1024);

    if (plane == NavPlane::XY) {
        config.walkableHeight = 3;
        config.walkableClimb = 1;
    } else {
        config.walkableHeight = std::max(3, (int)std::ceil(config.agentHeight / config.cellHeight));
        config.walkableClimb = (int)std::floor(config.agentClimb / config.cellHeight);
    }
    config.walkableRadius = (int)std::ceil(config.agentRadius / config.cellSize);
    config.borderSize = config.walkableRadius + TILE_BORDER_PADDING;

    const float area = minRegionArea >= 0.0f ? minRegionArea * scale * scale : 2.0f * scale * scale;
    config.minRegionArea = std::max(0, (int)std::ceil(area / (config.cellSize * config.cellSize)));
    config.mergeRegionArea = std::max(config.minRegionArea, (int)std::ceil((float)config.minRegionArea * MERGE_REGION_RATIO));
    config.maxEdgeLen = std::max(1, (int)(MAX_EDGE_LENGTH * scale / config.cellSize));
    config.maxSimplificationError = 1.3f;
    config.detailSampleDist = DETAIL_SAMPLE_CELLS * config.cellSize;
    config.detailSampleMaxError = config.cellHeight;
    config.maxVertsPerPoly = 6;
    // a tile holds at most half a polygon per cell; Detour's poly bits round it up
    const unsigned int cells = (unsigned int)config.tileSize * (unsigned int)config.tileSize;
    config.maxPolysPerTile = (int)nextPow2(std::max(64u, cells / 2u));
    return config;
}

uint64_t NavBuildConfig::hash() const {
    NavHasher hasher;
    hasher.u8((uint8_t)plane);
    hasher.boolean(carving);
    hasher.f32(cellSize);
    hasher.f32(cellHeight);
    hasher.i32(tileSize);
    hasher.f32(agentRadius);
    hasher.f32(agentHeight);
    hasher.f32(agentClimb);
    hasher.f32(agentMaxSlope);
    hasher.i32(walkableHeight);
    hasher.i32(walkableClimb);
    hasher.i32(walkableRadius);
    hasher.i32(borderSize);
    hasher.i32(minRegionArea);
    hasher.i32(mergeRegionArea);
    hasher.i32(maxEdgeLen);
    hasher.f32(maxSimplificationError);
    hasher.f32(detailSampleDist);
    hasher.f32(detailSampleMaxError);
    hasher.i32(maxVertsPerPoly);
    hasher.i32(maxPolysPerTile);
    return hasher.value();
}

Vector3 NavTerrainInput::localAt(int i, int j) const {
    const float x = (i == samples - 1) ? halfSize : (-halfSize + spacing * (float)i);
    const float z = (j == samples - 1) ? halfSize : (-halfSize + spacing * (float)j);
    return Vector3(x, heightAt(i, j), z);
}

void NavTerrainInput::computeBounds() {
    fromNav = toNav.inverse();
    for (int a = 0; a < 3; a++) {
        bmin[a] = std::numeric_limits<float>::max();
        bmax[a] = -std::numeric_limits<float>::max();
    }
    if (samples < 2 || heights.size() < (size_t)samples * (size_t)samples)
        return;
    float minH = std::numeric_limits<float>::max(), maxH = -std::numeric_limits<float>::max();
    for (float h : heights) {
        minH = std::min(minH, h);
        maxH = std::max(maxH, h);
    }
    // the 8 corners of the local box bound the lattice under any affine map
    for (int c = 0; c < 8; c++) {
        const Vector3 local((c & 1) ? halfSize : -halfSize, (c & 2) ? maxH : minH, (c & 4) ? halfSize : -halfSize);
        const Vector3 p = toNav * local;
        for (int a = 0; a < 3; a++) {
            bmin[a] = std::min(bmin[a], p[(unsigned)a]);
            bmax[a] = std::max(bmax[a], p[(unsigned)a]);
        }
    }
}

bool NavTerrainInput::cellRange(const float* boxMin, const float* boxMax, int& i0, int& i1, int& j0, int& j1) const {
    if (samples < 2 || !overlaps(boxMin, boxMax, bmin, bmax))
        return false;
    // the box's corners in the lattice's space bound the cells that may touch it
    float lminX = std::numeric_limits<float>::max(), lmaxX = -std::numeric_limits<float>::max();
    float lminZ = std::numeric_limits<float>::max(), lmaxZ = -std::numeric_limits<float>::max();
    for (int c = 0; c < 8; c++) {
        const Vector3 corner((c & 1) ? boxMax[0] : boxMin[0], (c & 2) ? boxMax[1] : boxMin[1], (c & 4) ? boxMax[2] : boxMin[2]);
        const Vector3 local = fromNav * corner;
        lminX = std::min(lminX, local.x);
        lmaxX = std::max(lmaxX, local.x);
        lminZ = std::min(lminZ, local.z);
        lmaxZ = std::max(lmaxZ, local.z);
    }
    const int cells = samples - 1;
    i0 = std::clamp((int)std::floor((lminX + halfSize) / spacing), 0, cells);
    i1 = std::clamp((int)std::floor((lmaxX + halfSize) / spacing) + 1, 0, cells);
    j0 = std::clamp((int)std::floor((lminZ + halfSize) / spacing), 0, cells);
    j1 = std::clamp((int)std::floor((lmaxZ + halfSize) / spacing) + 1, 0, cells);
    return i0 < i1 && j0 < j1;
}

NavTileCoord NavBakeInput::tileAt(const float* point) const {
    const float size = config.tileWorldSize();
    return NavTileCoord{(int)std::floor(point[0] / size), (int)std::floor(point[2] / size)};
}

void NavBakeInput::tileBounds(int x, int y, float* outMin, float* outMax) const {
    const float size = config.tileWorldSize();
    outMin[0] = (float)x * size;
    outMin[1] = bmin[1];
    outMin[2] = (float)y * size;
    outMax[0] = (float)(x + 1) * size;
    outMax[1] = bmax[1];
    outMax[2] = (float)(y + 1) * size;
}

void NavBakeInput::computeTileGrid() {
    // the grid covers the surface bounds, anchored at whole tiles
    const float size = config.tileWorldSize();
    if (!(bmax[0] >= bmin[0]) || !(bmax[2] >= bmin[2])) {
        minTileX = minTileY = 0;
        maxTileX = maxTileY = -1;
        return;
    }
    // far beyond any surface Detour can hold: clamped so the counts stay integers
    const double limit = 1 << 20;
    minTileX = (int)std::clamp(std::floor((double)bmin[0] / size), -limit, limit);
    minTileY = (int)std::clamp(std::floor((double)bmin[2] / size), -limit, limit);
    maxTileX = std::max(minTileX, (int)std::clamp(std::ceil((double)bmax[0] / size) - 1.0, -limit, limit));
    maxTileY = std::max(minTileY, (int)std::clamp(std::ceil((double)bmax[2] / size) - 1.0, -limit, limit));
}

void NavBakeInput::finalize() {
    for (NavTerrainInput& terrain : terrains)
        terrain.computeBounds();
    for (NavConvexVolume& volume : volumes) {
        volume.bmin[0] = volume.bmin[2] = std::numeric_limits<float>::max();
        volume.bmax[0] = volume.bmax[2] = -std::numeric_limits<float>::max();
        for (size_t i = 0; i + 1 < volume.points.size(); i += 2) {
            volume.bmin[0] = std::min(volume.bmin[0], volume.points[i]);
            volume.bmax[0] = std::max(volume.bmax[0], volume.points[i]);
            volume.bmin[2] = std::min(volume.bmin[2], volume.points[i + 1]);
            volume.bmax[2] = std::max(volume.bmax[2], volume.points[i + 1]);
        }
        volume.bmin[1] = volume.minY;
        volume.bmax[1] = volume.maxY;
    }

    computeTileGrid();
    const float size = config.tileWorldSize();
    const int count = tileCount();
    tileTriangles.assign((size_t)count, {});
    tileHashes.assign((size_t)count, 0);
    nonEmptyTiles.clear();
    if (count == 0)
        return;

    // buckets: a triangle goes to every tile its xz bounds touch, border included, in triangle order
    const float border = config.borderWorldSize();
    const size_t triangleTotal = triangles.size() / 3;
    for (size_t t = 0; t < triangleTotal; t++) {
        float tmin[2] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        float tmax[2] = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
        for (int c = 0; c < 3; c++) {
            const float* v = &vertices[(size_t)triangles[t * 3 + (size_t)c] * 3];
            tmin[0] = std::min(tmin[0], v[0]);
            tmax[0] = std::max(tmax[0], v[0]);
            tmin[1] = std::min(tmin[1], v[2]);
            tmax[1] = std::max(tmax[1], v[2]);
        }
        const int x0 = std::max(minTileX, (int)std::floor((tmin[0] - border) / size));
        const int x1 = std::min(maxTileX, (int)std::floor((tmax[0] + border) / size));
        const int y0 = std::max(minTileY, (int)std::floor((tmin[1] - border) / size));
        const int y1 = std::min(maxTileY, (int)std::floor((tmax[1] + border) / size));
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++)
                tileTriangles[tileIndex(x, y)].push_back((uint32_t)t);
        }
    }

    // links belong to the tile of their start
    std::vector<std::vector<uint32_t>> tileLinks((size_t)count);
    for (size_t l = 0; l < links.size(); l++) {
        const NavTileCoord tile = tileAt(links[l].start);
        if (tile.x >= minTileX && tile.x <= maxTileX && tile.y >= minTileY && tile.y <= maxTileY)
            tileLinks[tileIndex(tile.x, tile.y)].push_back((uint32_t)l);
    }

    const uint64_t configHash = config.hash();
    std::vector<uint8_t> hasInput((size_t)count, 0);
    jobs::parallelFor((size_t)count, [&](size_t index) {
        const NavTileCoord tile = tileCoord((uint32_t)index);
        float boxMin[3], boxMax[3];
        tileBounds(tile.x, tile.y, boxMin, boxMax);
        boxMin[0] -= border;
        boxMin[2] -= border;
        boxMax[0] += border;
        boxMax[2] += border;

        NavHasher hasher;
        hasher.u64(configHash);
        hasher.i32(tile.x);
        hasher.i32(tile.y);
        hasher.f32(bmin[1]);
        hasher.f32(bmax[1]);
        bool any = false;

        const std::vector<uint32_t>& bucket = tileTriangles[index];
        hasher.u64(bucket.size());
        for (uint32_t t : bucket) {
            for (int c = 0; c < 3; c++)
                hasher.floats(&vertices[(size_t)triangles[(size_t)t * 3 + (size_t)c] * 3], 3);
            hasher.u8(triangleAreas[t]);
        }
        any = any || !bucket.empty();

        for (size_t k = 0; k < terrains.size(); k++) {
            const NavTerrainInput& terrain = terrains[k];
            int i0, i1, j0, j1;
            if (!terrain.cellRange(boxMin, boxMax, i0, i1, j0, j1))
                continue;
            any = true;
            hasher.u64(k);
            hasher.floats(&terrain.toNav[0][0], 16);
            hasher.f32(terrain.halfSize);
            hasher.f32(terrain.spacing);
            hasher.i32(terrain.samples);
            hasher.u8(terrain.recastArea);
            hasher.i32(i0);
            hasher.i32(i1);
            hasher.i32(j0);
            hasher.i32(j1);
            for (int j = j0; j <= j1; j++) {
                for (int i = i0; i <= i1; i++)
                    hasher.f32(terrain.heightAt(i, j));
            }
            // T3 R23: a hole under this tile bakes it again. No holes hash as before.
            if (!terrain.holeCells.empty()) {
                for (int j = j0; j < j1; j++) {
                    for (int i = i0; i < i1; i++)
                        hasher.u8(terrain.isHoleCell(i, j) ? 1 : 0);
                }
            }
        }

        for (const NavConvexVolume& volume : volumes) {
            if (!overlaps(boxMin, boxMax, volume.bmin, volume.bmax))
                continue;
            hasher.floats(volume.points.data(), volume.points.size());
            hasher.f32(volume.minY);
            hasher.f32(volume.maxY);
            hasher.u8(volume.recastArea);
            if (volume.boundaryOnly)
                hasher.boolean(true);
        }

        for (uint32_t l : tileLinks[index]) {
            const NavLinkInput& link = links[l];
            hasher.floats(link.start, 3);
            hasher.floats(link.end, 3);
            hasher.f32(link.radius);
            hasher.boolean(link.bidirectional);
            hasher.u8(link.area);
            hasher.u8(link.polyArea);
            hasher.u32(link.userId);
            if (link.polyArea >= FIRST_LINK_COST_AREA)
                hasher.f32(linkAreaCosts[(size_t)(link.polyArea - FIRST_LINK_COST_AREA)]);
        }

        hasInput[index] = any ? 1 : 0;
        tileHashes[index] = any ? hasher.value() : 0;
    }, {/*grain*/ 4});

    for (size_t i = 0; i < hasInput.size(); i++) {
        if (hasInput[i])
            nonEmptyTiles.push_back((uint32_t)i);
    }
}

size_t NavBakeInput::memoryBytes() const {
    size_t bytes = vertices.capacity() * sizeof(float) + triangles.capacity() * sizeof(int) + triangleAreas.capacity();
    for (const NavTerrainInput& terrain : terrains)
        bytes += terrain.heights.capacity() * sizeof(float);
    for (const NavConvexVolume& volume : volumes)
        bytes += volume.points.capacity() * sizeof(float);
    bytes += links.capacity() * sizeof(NavLinkInput);
    for (const std::vector<uint32_t>& bucket : tileTriangles)
        bytes += bucket.capacity() * sizeof(uint32_t);
    bytes += tileHashes.capacity() * sizeof(uint64_t);
    bytes += (triangleSources.capacity() + terrainSources.capacity() + volumeSources.capacity()) * sizeof(Entity);
    bytes += excludedEntities.capacity() * sizeof(std::pair<Entity, std::string>);
    return bytes;
}

int nav::saltBitsFor(int64_t tileCount, int maxPolysPerTile) {
    int tileBits = 0;
    while (((int64_t)1 << tileBits) < std::max<int64_t>(1, tileCount))
        tileBits++;
    const int polyBits = ilog2(nextPow2((unsigned int)std::max(1, maxPolysPerTile)));
    return 32 - tileBits - polyBits;
}

std::string nav::checkPolyRefBits(int64_t tileCount, int maxPolysPerTile) {
    if (saltBitsFor(tileCount, maxPolysPerTile) >= 10)
        return std::string();
    return "Surface too large: " + std::to_string(tileCount) + " tiles \xC3\x97 " + std::to_string(maxPolysPerTile) +
           " polys; increase cell/tile size or split the surface";
}
