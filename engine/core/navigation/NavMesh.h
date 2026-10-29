// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The tiled dtNavMesh with tiles added and removed on the query thread, and the tile data checks
// before Detour sees a tile, follow OpenEngine's DetourNavMap (Engine/Modules/Pathfinding/Source/
// DetourNavMap.cpp), Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License.
// See NOTICE.md.

#ifndef NAV_MESH_H
#define NAV_MESH_H

// A navmesh: its baked data (NavMeshData, what a .navmesh file holds) and the runtime Detour mesh
// built from it, with the queries of spec R8 in nav space (the surface's frame; NavigationSystem
// maps world positions). A surface with runtime carving keeps tile-cache layers and builds its
// navmesh tiles from them, so carving obstacles can cut it (spec R9, D4).
//
// Main thread: the queries share one Detour query object.

#include "Export.h"
#include "navigation/NavMeshBuilder.h"
#include "navigation/NavTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::nav {

    // The header of a baked navmesh (spec R39)
    struct JASPERENGINE_API NavMeshHeader {
        // the agent type it was baked for, as it was then
        AgentTypeSettings agent;
        uint64_t areaTableHash = 0;
        NavBuildConfig config;
        // Detour's tile pool (a power of two), the polygons a tile may hold
        int maxTiles = 0;
        // the tile-cache obstacles a carving surface may hold at once
        int maxObstacles = 128;
        // the cost of each link cost area (FIRST_LINK_COST_AREA + i)
        std::vector<float> linkAreaCosts;
    };

    struct JASPERENGINE_API NavMeshData {
        NavMeshHeader header;
        // ascending (y, x, layer)
        std::vector<NavTileBlob> tiles;

        void sortTiles();
        size_t memoryBytes() const;
        // the blob of a tile and layer, null when none
        const NavTileBlob* findTile(int x, int y, int layer = 0) const;
    };

    // The .navmesh format (little-endian): "JNAV", the format version, the header, then each
    // tile's x, y, layer, input hash, flags and its Detour tile data (static) or deflated
    // tile-cache layer (carving) with its links. A newer or unknown version, a big-endian host,
    // a short, corrupt or inconsistent file are refused with an error (nothing crashes).
    namespace NavMeshFile {
        constexpr uint32_t VERSION = 1;
        JASPERENGINE_API std::vector<uint8_t> write(const NavMeshData& data);
        JASPERENGINE_API bool read(const uint8_t* bytes, size_t size, NavMeshData& out, std::string& error);
    }

    // What a query may walk on, with its costs (spec D5: the area mask is Detour's include flags)
    struct JASPERENGINE_API NavFilter {
        uint16_t areaMask = ALL_AREAS;
        float areaCosts[AREA_COUNT];

        NavFilter();
        // the mask with the project's area costs
        static NavFilter fromSettings(uint16_t areaMask);
    };

    // The geometry the editor's overlays draw (spec R33, D8), in nav space
    struct NavDebugGeometry {
        // triangles: 3 points each, an area per triangle
        std::vector<float> triangles;
        std::vector<uint8_t> triangleAreas;
        // segments, 2 points each: the walkable boundary, and the edges between polygons
        std::vector<float> boundaryEdges;
        std::vector<float> polygonEdges;
        // per tile: bmin, bmax
        std::vector<float> tileBounds;
        struct Link {
            float start[3];
            float end[3];
            float radius;
            bool bidirectional;
            uint8_t area;
            uint32_t userId;
        };
        std::vector<Link> links;
        // the triangle cap stopped the extraction
        bool truncated = false;
    };

    class JASPERENGINE_API NavMesh {
    public:
        NavMesh();
        ~NavMesh();
        NavMesh(const NavMesh&) = delete;
        NavMesh& operator=(const NavMesh&) = delete;

        // Builds the runtime mesh from baked data. keepData keeps the records (the editor saves
        // them and rebakes incrementally); a game drops them once Detour has its copies.
        bool create(NavMeshData data, bool keepData, std::string& error);
        void clear();
        bool isValid() const;

        const NavMeshHeader& getHeader() const;
        // the baked records, when kept (create's keepData)
        const NavMeshData* getData() const;

        // Replaces the tiles of the given coordinates by `blobs` (none: the tile is removed),
        // keeping every other tile: an incremental bake's result. Kept data only.
        bool replaceTiles(const std::vector<NavTileCoord>& coords, std::vector<NavTileBlob> blobs, std::string& error);

        int getTileCount() const;
        int getPolyCount() const;
        // what Detour holds (tiles, tile cache, query) plus the kept records
        size_t getMemoryBytes() const;
        bool isCarving() const;

        // ---- queries (nav space)
        // the query searches at most 2048 nodes (spec R26)
        NavPath findPath(const Vector3& start, const Vector3& end, const NavFilter& filter, const Vector3& halfExtents) const;
        // the nearest point within maxDistance
        NavHit samplePosition(const Vector3& point, float maxDistance, const NavFilter& filter) const;
        // hit == true when the ray leaves the mesh (or meets a wall) before `to`
        NavHit raycast(const Vector3& from, const Vector3& to, const NavFilter& filter, const Vector3& halfExtents) const;
        NavHit findClosestEdge(const Vector3& point, const NavFilter& filter, const Vector3& halfExtents) const;
        // the polygon height under a point (false off the mesh)
        bool getHeight(const Vector3& point, const NavFilter& filter, const Vector3& halfExtents, float& height) const;

        // ---- runtime carving (a carving surface; 0 when it is not, or the request queue is full)
        uint32_t addCylinderObstacle(const Vector3& position, float radius, float height);
        uint32_t addBoxObstacle(const Vector3& center, const Vector3& halfExtents, float yRadians);
        bool removeObstacle(uint32_t obstacle);
        // Processes obstacle requests and rebuilds touched tiles until upToDate or budgetMs is spent
        bool updateCarving(double budgetMs, bool& upToDate, int& tilesRebuilt);

        // ---- links: moves the connections of a link to new endpoints (an auto-updating
        // NavMeshLink that moved). False when a start left its tile (the surface must be rebaked).
        bool updateLink(uint32_t userId, const std::vector<NavLinkInput>& connections, std::string& error);

        // ---- overlays: the tiles `visible` accepts (nav-space bounds), at most maxTriangles
        void getDebugGeometry(const std::function<bool(const float* bmin, const float* bmax)>& visible, size_t maxTriangles,
                              bool detailMesh, NavDebugGeometry& out) const;

        // Detour objects of this mesh, for NavCrowd (engine-internal; null when invalid)
        void* detourNavMesh() const;
        void* detourQuery() const;
        // Changes whenever tiles change (rebuilds of overlays, crowd corridor checks)
        uint64_t getRevision() const;
        // whether a tile holds off-mesh connections (links)
        bool hasOffMeshConnections() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}

#endif // NAV_MESH_H
