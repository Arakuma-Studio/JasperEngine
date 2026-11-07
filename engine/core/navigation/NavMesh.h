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
#include "navigation/NavDiagnostics.h"
#include "navigation/NavMeshBuilder.h"
#include "navigation/NavRandom.h"
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
        // the query filter's mask and costs (the project's where it sets none)
        static NavFilter fromQuery(const QueryFilter& filter);
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

        // ---- S75: per tile of tileBounds, its x, y and layer
        std::vector<int32_t> tileCoords;
        // per triangle: its island (DEBUG_ISLANDS, -1: none), the index of its tile in
        // tileBounds (DEBUG_TILES); empty without the flag
        std::vector<int32_t> triangleIslands;
        std::vector<int32_t> triangleTiles;
    };

    // ---- S75 diagnostics (docs/specs/S75-navigation-diagnostics.md)

    // getDebugGeometry's extras
    constexpr uint32_t DEBUG_ISLANDS = 1u << 0;
    constexpr uint32_t DEBUG_TILES = 1u << 1;

    // One off-mesh connection of a baked link and whether Detour joined its ends to a polygon
    struct NavLinkState {
        uint32_t userId = 0;
        Vector3 start, end;
        bool startConnected = false, endConnected = false;
        // the islands of the polygons its ends joined (-1: unconnected)
        int startIsland = -1, endIsland = -1;
    };

    // One Detour tile (the overlays' labels, the bake report)
    struct NavTileInfo {
        int x = 0, y = 0, layer = 0;
        // ground polygons
        int polys = 0;
        size_t bytes = 0;
        float bmin[3] = {0, 0, 0};
        float bmax[3] = {0, 0, 0};
    };

    // A carving obstacle (S74 R11), in nav space: Detour's cylinders and yaw-only boxes
    struct NavObstacleDesc {
        enum class Shape : uint8_t {
            Cylinder,
            Box
        };
        Shape shape = Shape::Box;
        // cylinder: the centre of its base; box: its centre
        Vector3 position;
        float radius = 0.5f;
        float height = 2.0f;
        Vector3 halfExtents{0.5f, 0.5f, 0.5f};
        float yRadians = 0.0f;
    };

    struct NavMeshCreateOptions {
        // carving: the tiles built from the layers are queued and built by updateCarving (a bake
        // landing in play, S74 R15) instead of all at once
        bool deferCarvedBuilds = false;
        // the carving obstacles the mesh may hold (Max Carving Obstacles); -1: the header's
        int maxObstacles = -1;
    };

    // What one updateCarving may do (S74 R14): a count of tile builds, and a wall-clock ceiling
    // that stops it after at least one build
    struct NavCarveBudget {
        int maxTileBuilds = 4;
        // 0: no ceiling (tests, benchmarks)
        double ceilingMs = 4.0;
    };

    class JASPERENGINE_API NavMesh {
    public:
        NavMesh();
        ~NavMesh();
        NavMesh(const NavMesh&) = delete;
        NavMesh& operator=(const NavMesh&) = delete;

        // Builds the runtime mesh from baked data. keepData keeps the records (the editor saves
        // them and rebakes incrementally); a game drops them once Detour has its copies.
        bool create(NavMeshData data, bool keepData, std::string& error, const NavMeshCreateOptions& options = {});
        void clear();
        bool isValid() const;
        // no carved tile build is queued (deferred builds, S74 R15)
        bool isComplete() const;
        int getQueuedBuilds() const;
        // changes on every create() and clear(): a new Detour mesh (crowds are made again, S74 R9)
        uint64_t getGeneration() const;

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
        // the query searches at most 2048 nodes (spec R26); the path keeps its polygon corridor,
        // per-corner areas and flags, its cost and the length per area (S76 R5)
        NavPath findPath(const Vector3& start, const Vector3& end, const NavFilter& filter, const Vector3& halfExtents,
                         const PathOptions& options = {}) const;
        // the nearest point within maxDistance
        NavHit samplePosition(const Vector3& point, float maxDistance, const NavFilter& filter) const;
        // hit == true when the ray leaves the mesh (or meets a wall) before `to`; cost: the
        // travelled part's
        NavHit raycast(const Vector3& from, const Vector3& to, const NavFilter& filter, const Vector3& halfExtents) const;
        // the nearest wall within maxDistance (<= 0: 64 tiles); hit false with distance
        // maxDistance when there is none
        NavHit findClosestEdge(const Vector3& point, const NavFilter& filter, const Vector3& halfExtents, float maxDistance = -1.0f) const;
        // ---- S76 queries
        // A point uniform by area over the polygons the filter passes (of island `island`, -1:
        // any): three draws of `random`. False when no polygon passes.
        bool findRandomPoint(const NavFilter& filter, NavRandom& random, int island, NavHit& out) const;
        // A point reachable from `center` through the filter within `radius` (in the XZ plane),
        // Detour's findRandomPointAroundCircle: not uniform. At most 8 tries; island >= 0 keeps
        // only points of that island.
        bool findRandomPointAround(const Vector3& center, float radius, const NavFilter& filter, const Vector3& halfExtents,
                                   NavRandom& random, int island, NavHit& out) const;
        // Moves from the polygon under `from` towards `to` along the surface (at most 64 polygons):
        // position (on the polygon), hit (stopped more than 1 mm short of `to`), distance (moved,
        // XZ), area. False when `from` is off the mesh.
        bool moveAlongSurface(const Vector3& from, const Vector3& to, const NavFilter& filter, const Vector3& halfExtents,
                              NavHit& out) const;
        // the polygon height under a point (false off the mesh)
        bool getHeight(const Vector3& point, const NavFilter& filter, const Vector3& halfExtents, float& height) const;

        // ---- runtime carving (S74 R11-R17). The mesh owns its obstacles: ids are stable and never
        // 0, requests queue and reach Detour's tile cache as it has room, so none is lost.
        // 0 when the mesh does not carve, or holds its Max Carving Obstacles
        uint32_t addObstacle(const NavObstacleDesc& desc);
        void moveObstacle(uint32_t id, const NavObstacleDesc& desc);
        void removeObstacle(uint32_t id);
        // whether the id is one of this mesh's obstacles (an adoption keeps at most Max Carving Obstacles)
        bool hasObstacle(uint32_t id) const;
        // takes the obstacles of the mesh this one replaces (a full bake, a reload): same ids,
        // queued as adds on this tile cache
        void adoptObstacles(NavMesh& previous);
        // the tile-cache layers the obstacle's bounds touch (Detour cuts at most 8)
        int countTileLayers(const NavObstacleDesc& desc) const;
        // Queued builds first, then obstacle requests and the tiles they touch, within the budget
        bool updateCarving(const NavCarveBudget& budget, bool& upToDate, int& tilesRebuilt);
        // the carved tile builds of replaceTiles and updateLink are queued for updateCarving (in
        // play) instead of built at once; create() sets it from its options
        void setDeferCarvedBuilds(bool defer);
        // the obstacles Detour's tile cache holds (added, or being removed)
        int getTileCacheObstacleCount() const;
        // tile-cache builds that failed (each logged once per tile)
        int getFailedCarveBuilds() const;

        // ---- links: moves the connections of a link to new endpoints (an auto-updating
        // NavMeshLink that moved). False when a start left its tile (the surface must be rebaked).
        // A carving mesh created with deferCarvedBuilds queues the rebuilt tiles (S74 R15).
        bool updateLink(uint32_t userId, const std::vector<NavLinkInput>& connections, std::string& error);
        // the connections of a link as the mesh holds them now (false: it holds none)
        bool getLinkConnections(uint32_t userId, std::vector<NavLinkInput>& out) const;

        // ---- overlays: the tiles `visible` accepts (nav-space bounds), at most maxTriangles;
        // extras: DEBUG_ISLANDS, DEBUG_TILES (islands label the mesh when stale)
        void getDebugGeometry(const std::function<bool(const float* bmin, const float* bmax)>& visible, size_t maxTriangles,
                              bool detailMesh, NavDebugGeometry& out, uint32_t extras = 0) const;

        // Detour objects of this mesh, for NavCrowd (engine-internal; null when invalid)
        void* detourNavMesh() const;
        void* detourQuery() const;
        // Changes whenever tiles change (rebuilds of overlays, crowd corridor checks)
        uint64_t getRevision() const;
        // whether a tile holds off-mesh connections (links)
        bool hasOffMeshConnections() const;

        // ---- S75 diagnostics (nav space). Islands are labelled lazily, on the main thread, when
        // the revision changed since the last labelling (R8): the calls that say so label;
        // path queries never do, they only use labels that are current.
        // the islands (nav-space bounds, nav units^2); labels when stale
        const std::vector<NavIsland>& getIslands() const;
        // the ground polygons' area, nav units^2; labels when stale
        float getWalkableArea() const;
        bool areIslandsCurrent() const;
        // a polygon's island; -1 when the labels are not current (no labelling) or it has none
        int getIslandOf(uint32_t polyRef) const;
        // the revision the labels describe (0: never labelled)
        uint64_t getLabelledRevision() const;
        // what the labels hold (the bench's islands_bytes_per_poly)
        size_t getIslandMemoryBytes() const;
        // where a point is (R10): onMesh when a polygon is in the search box, else the nearest
        // within maxDistance; labels when stale
        NavLocation locate(const Vector3& point, const NavFilter& filter, const Vector3& halfExtents, float maxDistance) const;
        // whether a polygon lies under (or over) the point within the box's height: the point is
        // walkable, not merely near the mesh
        bool covers(const Vector3& point, const NavFilter& filter, const Vector3& halfExtents) const;
        // R6: false without a search when the islands are current and the ends' differ; else
        // findPath's status is Complete. why: the failure (None when reachable)
        bool isReachable(const Vector3& start, const Vector3& end, const NavFilter& filter, const Vector3& halfExtents,
                         PathFailure* why = nullptr) const;
        // the A* searches findPath and isReachable ran on this mesh (tests, the bench)
        uint64_t getSearchCount() const;
        // every off-mesh connection; labels when stale
        void getLinkStates(std::vector<NavLinkState>& out) const;
        void getTileInfos(std::vector<NavTileInfo>& out) const;
        // the polygon nearest the point within the box (0: none), for setPolyFlags
        uint32_t getPolyAt(const Vector3& point, const NavFilter& filter, const Vector3& halfExtents) const;
        // the only way to change a polygon's flags (0 disables it); bumps the revision. False for
        // an invalid reference.
        bool setPolyFlags(uint32_t polyRef, uint16_t flags);
        // whether a polygon of one static tile's data covers the point (the probe's rebuilds)
        static bool tileCovers(const NavTileBlob& staticTile, const NavBuildConfig& config, const Vector3& point,
                               const Vector3& halfExtents);

        // ---- S76 areas (docs/specs/S76-navigation-gameplay-api.md R11-R18, D5). Runtime area
        // overrides (nav-space prisms: the ground polygons whose centroid lies inside take the
        // area, Not Walkable gives flags 0) and link states over the baked flags. They apply in
        // ascending (order, id), a later one wins and Not Walkable always; every tile change
        // (create, replaceTiles, updateCarving, updateLink) applies them again before it returns,
        // and they outlive clear() and create(). An applied change bumps the revision once.
        // id > 0; on a mesh not built yet it applies to the tiles that come
        uint32_t addAreaOverride(const NavConvexVolume& volume, uint8_t area, uint64_t order = 0);
        bool updateAreaOverride(uint32_t id, const NavConvexVolume& volume, uint8_t area);
        bool removeAreaOverride(uint32_t id);
        // the ground polygons in the override's volume now (-1: no such override)
        int getAreaOverridePolyCount(uint32_t id) const;
        // a deactivated link's connections pass nothing (flags 0; their polygon area stays)
        void setLinkActive(uint32_t userId, bool active);
        bool isLinkActive(uint32_t userId) const;
        // the polygons an override or link state changed since the last call (crowd replans)
        void takeChangedPolys(std::vector<uint32_t>& out);
        // the tiles holding a baked-state snapshot: none without overrides and deactivated links (R18)
        size_t getPolyStateTileCount() const;
        // Detour's storeTileState bytes of a tile (empty: no such tile), what R13's restore matches
        std::vector<uint8_t> getTileStateBytes(int x, int y, int layer = 0) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}

#endif // NAV_MESH_H
