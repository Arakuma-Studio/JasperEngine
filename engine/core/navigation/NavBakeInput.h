// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Recast configuration derived from the agent and one cell size (walkableClimb, walkableRadius
// and borderSize = walkableRadius + padding, detailSampleMaxError = ch) follows OpenEngine's
// DetourNavMap.cpp (Engine/Modules/Pathfinding/Source/DetourNavMap.cpp), Copyright (c) 2026
// Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef NAV_BAKE_INPUT_H
#define NAV_BAKE_INPUT_H

// What a navmesh bake reads (spec R4, D3): an immutable snapshot the collector makes on the main
// thread from a scene, held by shared_ptr while the tile jobs read it. World triangles are
// already in the surface's nav space with a Recast area each, terrains stay height lattices
// (each tile makes its own triangles from them), modifier volumes and 2D collider footprints are
// convex prisms that mark areas, links are off-mesh connections. finalize() buckets the
// triangles into the uniform tile grid and hashes every tile's inputs, so a bake rebuilds only
// the tiles whose hash changed.

#include "Export.h"
#include "math/Matrix4.h"
#include "navigation/NavTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::nav {

    // Recast area ids of the inputs: an area a is Recast area a + 1 (0 is Recast's null area),
    // and Not Walkable is marked with the top id so that it wins Recast's max-area span merge;
    // it becomes the null area once the heightfield is compacted (spec R5).
    constexpr uint8_t RECAST_NOT_WALKABLE = 63;
    // Recast's RC_NULL_AREA: nothing walks there
    constexpr uint8_t RECAST_NULL_AREA = 0;
    inline uint8_t recastAreaOf(uint8_t area) { return area == AREA_NOT_WALKABLE ? RECAST_NOT_WALKABLE : (uint8_t)(area + 1); }
    // the area of a Recast id of a built polygon (1-16)
    inline uint8_t areaOfRecast(uint8_t recastArea) { return (uint8_t)(recastArea - 1); }

    // Polygon areas above the 16 areas: links with a cost override get one each (their flag
    // stays their area's), so a link's cost needs no virtual query filter
    constexpr int FIRST_LINK_COST_AREA = AREA_COUNT;
    constexpr int MAX_POLY_AREAS = 64;

    // ---- S76 runtime volumes (docs/specs/S76-navigation-gameplay-api.md D4): the bake moves a
    // walkable span of Recast area r (1-16) inside a runtime volume to r + RUNTIME_VOLUME_AREA_OFFSET,
    // so regions and polygons split along the volume; built polygons map those ids back to r's
    // area, so stored areas stay 0-15 (R48). Recast and the collector use none of these ids.
    constexpr uint8_t RUNTIME_VOLUME_AREA_OFFSET = 32;
    constexpr uint8_t FIRST_RUNTIME_VOLUME_RECAST_AREA = RUNTIME_VOLUME_AREA_OFFSET + 1;
    constexpr uint8_t LAST_RUNTIME_VOLUME_RECAST_AREA = RUNTIME_VOLUME_AREA_OFFSET + AREA_COUNT;
    static_assert(FIRST_RUNTIME_VOLUME_RECAST_AREA > AREA_COUNT && LAST_RUNTIME_VOLUME_RECAST_AREA < RECAST_NOT_WALKABLE,
                  "the runtime volume ids lie between the areas' Recast ids and Not Walkable's");

    // Everything a tile build reads besides geometry. Lengths are in nav units (metres in 3D,
    // scene points on a 2D surface).
    struct JASPERENGINE_API NavBuildConfig {
        NavPlane plane = NavPlane::XZ;
        // tile-cache layers (runtime carving) instead of static tiles
        bool carving = false;

        float cellSize = 0.1667f;
        float cellHeight = 0.0833f;
        int tileSize = 64;

        float agentRadius = 0.5f;
        float agentHeight = 2.0f;
        float agentClimb = 0.4f;
        float agentMaxSlope = 45.0f;

        // derived, in cells
        int walkableHeight = 0;
        int walkableClimb = 0;
        int walkableRadius = 0;
        int borderSize = 0;
        int minRegionArea = 0;
        int mergeRegionArea = 0;
        int maxEdgeLen = 0;
        float maxSimplificationError = 1.3f;
        float detailSampleDist = 0.0f;
        float detailSampleMaxError = 0.0f;
        int maxVertsPerPoly = 6;
        // the most polygons a tile may hold (a power of two, Detour's poly bits)
        int maxPolysPerTile = 0;

        // The configuration of an agent type: cellSize <= 0 is radius / 3 (Unity's default voxel
        // size), minRegionArea in square units (Unity's, default 2 m^2). On a 2D surface the
        // agent is scaled by unitScale (scene points per metre) and its height and climb are
        // fixed internally (flat ground, obstacles are marked areas).
        static NavBuildConfig derive(const AgentTypeSettings& agent, NavPlane plane, float cellSize, int tileSize,
                                     float minRegionArea, bool carving, float unitScale = 1.0f);

        float tileWorldSize() const { return (float)tileSize * cellSize; }
        float borderWorldSize() const { return (float)borderSize * cellSize; }
        uint64_t hash() const;
    };

    // A height lattice (terrain, heightfield collider): samples x samples heights in its own
    // space, sample (i, j) at (centerX - halfSize + spacing * i, height, centerZ - halfSize +
    // spacing * j), brought into nav space by toNav (scale included)
    struct JASPERENGINE_API NavTerrainInput {
        Matrix4 toNav;
        float halfSize = 0.0f;
        float spacing = 0.0f;
        int samples = 0;
        std::vector<float> heights;
        uint8_t recastArea = 1;
        // T4 R25: a Terrain World's tiles share the World's toNav and are placed by their centres,
        // so two neighbours' border samples land on the same nav point and the lattices weld
        float centerX = 0.0f;
        float centerZ = 0.0f;
        // T4 R25: a tile's lattice reads the tile's own 16-bit samples (little-endian, tileRow
        // texels a row, sample (i, j) at texel (tileFirst + i * tileStep, tileFirst + j * tileStep))
        // instead of heights, and holds the tile's data by shared reference while the bake runs
        std::shared_ptr<const void> tileData;
        const uint8_t* tileSamples = nullptr;
        int tileRow = 0;
        int tileFirst = 0;
        int tileStep = 1;
        float tileMaxHeight = 0.0f;
        // T3 R23: (samples - 1)^2 flags, row by row: a lattice cell that overlaps a hole texel is
        // left out of the bake. Empty without holes.
        std::vector<uint8_t> holeCells;
        // finalize(): the inverse of toNav and the nav-space bounds
        Matrix4 fromNav;
        float bmin[3] = {0, 0, 0};
        float bmax[3] = {0, 0, 0};

        float heightAt(int i, int j) const {
            if (tileSamples) {
                const uint8_t* at = tileSamples + ((size_t)(tileFirst + j * tileStep) * (size_t)tileRow + (size_t)(tileFirst + i * tileStep)) * 2;
                return (float)(at[0] | (at[1] << 8)) / 65535.0f * tileMaxHeight;
            }
            return heights[(size_t)j * (size_t)samples + (size_t)i];
        }
        bool isHoleCell(int i, int j) const { return !holeCells.empty() && holeCells[(size_t)j * (size_t)(samples - 1) + (size_t)i] != 0; }
        Vector3 localAt(int i, int j) const;
        // The sample cells [i0, i1) x [j0, j1) whose nav-space footprint may touch the nav box
        // [boxMin, boxMax]; false when none does
        bool cellRange(const float* boxMin, const float* boxMax, int& i0, int& i1, int& j0, int& j1) const;
        // nav-space bounds from toNav and the heights
        void computeBounds();
    };

    // A convex prism marking an area on the compacted heightfield: a modifier volume, or a 2D
    // collider footprint (the null area)
    struct NavConvexVolume {
        // nav-space xz pairs, convex, either winding
        std::vector<float> points;
        float minY = 0.0f;
        float maxY = 0.0f;
        // a Recast area id (0: the null area)
        uint8_t recastArea = 0;
        float bmin[3] = {0, 0, 0};
        float bmax[3] = {0, 0, 0};
        // S76: a runtime volume's boundary: spans keep their area, only moved to the runtime
        // volume ids (recastArea unused); hashed only when true, so other tiles hash as before
        bool boundaryOnly = false;
    };

    // An off-mesh connection (NavMeshLink): baked into the tile holding its start
    struct NavLinkInput {
        float start[3] = {0, 0, 0};
        float end[3] = {0, 0, 0};
        float radius = 0.5f;
        bool bidirectional = true;
        // its flag is 1 << area
        uint8_t area = AREA_JUMP;
        // its polygon area: the area, or a link cost area
        uint8_t polyArea = AREA_JUMP;
        // the link's entity (events, overlays)
        uint32_t userId = 0;
    };

    struct NavTileCoord {
        int x = 0;
        int y = 0;
        bool operator==(const NavTileCoord& other) const { return x == other.x && y == other.y; }
        bool operator<(const NavTileCoord& other) const { return y < other.y || (y == other.y && x < other.x); }
    };

    struct JASPERENGINE_API NavBakeInput {
        NavBuildConfig config;

        // nav-space triangle soup with a Recast area per triangle
        std::vector<float> vertices;
        std::vector<int> triangles;
        std::vector<uint8_t> triangleAreas;

        std::vector<NavTerrainInput> terrains;
        // applied in order: the collector sorts them (ascending area, Not Walkable and the 2D
        // footprints last, so they win)
        std::vector<NavConvexVolume> volumes;
        std::vector<NavLinkInput> links;
        // the cost of each link cost area (index - FIRST_LINK_COST_AREA)
        std::vector<float> linkAreaCosts;

        // S75: with NavCollectOptions::recordSources only (the editor's probe), the GameObject of
        // each triangle, terrain and volume (NULL_ENTITY: one the collector made). Never hashed.
        std::vector<Entity> triangleSources;
        std::vector<Entity> terrainSources;
        std::vector<Entity> volumeSources;
        // NavCollectStats::excludedEntities, kept with the input the probe is given
        std::vector<std::pair<Entity, std::string>> excludedEntities;

        // nav-space bounds of the surface (the y range bounds every tile's heightfield)
        float bmin[3] = {0, 0, 0};
        float bmax[3] = {0, 0, 0};

        // the tile grid (tile (x, y) covers [x, x+1) * tileWorldSize in nav x and z), inclusive
        int minTileX = 0, minTileY = 0, maxTileX = -1, maxTileY = -1;
        // T4 R25: the tile pool a new navmesh holds at least: a Follow surface's whole region, so
        // that its steps replace tiles in place and never make the navmesh again under its agents
        int reserveTiles = 0;

        // finalize(): per tile in index order, the triangles that touch it with its border, and
        // the hash of every input of the tile
        std::vector<std::vector<uint32_t>> tileTriangles;
        std::vector<uint64_t> tileHashes;
        // ascending tile index: the tiles with any input
        std::vector<uint32_t> nonEmptyTiles;

        int tilesWide() const { return maxTileX >= minTileX ? maxTileX - minTileX + 1 : 0; }
        int tilesHigh() const { return maxTileY >= minTileY ? maxTileY - minTileY + 1 : 0; }
        int tileCount() const { return tilesWide() * tilesHigh(); }
        uint32_t tileIndex(int x, int y) const { return (uint32_t)((y - minTileY) * tilesWide() + (x - minTileX)); }
        NavTileCoord tileCoord(uint32_t index) const {
            const int wide = tilesWide();
            return NavTileCoord{minTileX + (int)index % wide, minTileY + (int)index / wide};
        }
        // the tile holding a nav-space point (Detour's calcTileLoc with a zero origin)
        NavTileCoord tileAt(const float* point) const;
        // [min, max) of a tile in nav x and z
        void tileBounds(int x, int y, float* outMin, float* outMax) const;

        // The tile grid from the bounds: tiles are anchored at whole multiples of the tile size,
        // so growing the bounds never moves existing tiles (their hashes stay valid for an
        // incremental bake)
        void computeTileGrid();
        // The grid, then buckets and hashes (parallel over tiles)
        void finalize();
        // tiles of the grid, as a 64-bit count (a surface far too large overflows an int)
        int64_t tileCount64() const { return (int64_t)tilesWide() * (int64_t)tilesHigh(); }

        // the vertex count, for statistics
        size_t triangleCount() const { return triangles.size() / 3; }
        size_t memoryBytes() const;
    };

    // Detour's limit (spec R6): 32-bit polygon references hold tile, polygon and salt bits; fewer
    // than 10 salt bits fail before any job runs. Empty when the surface fits.
    JASPERENGINE_API std::string checkPolyRefBits(int64_t tileCount, int maxPolysPerTile);
    JASPERENGINE_API int saltBitsFor(int64_t tileCount, int maxPolysPerTile);

}

#endif // NAV_BAKE_INPUT_H
