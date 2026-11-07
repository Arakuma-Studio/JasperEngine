// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAVIGATION_SYSTEM_H
#define NAVIGATION_SYSTEM_H

// The scene's navigation (docs/navigation.md, docs/specs/S19-navigation.md): the navmeshes of its
// NavMeshSurfaces (loaded from their .navmesh, or baked in tiles on the job system), their crowds,
// the NavGrids and every NavMeshAgent, NavMeshObstacle and NavMeshLink. Queries take and return
// world positions and work in the editor and in play.
//
// Each fixed tick (spec D2): 1. finished bake tiles land, 2. agents and obstacles sync from their
// transforms, 3. grid paths are solved as a batch, 4. carving obstacles update the tile caches
// (budgeted), 5. the crowds step, 6. agents write back by their driver, 7. events fire (in entity
// order, after the step: a callback may call any navigation API). update() only draws the
// Transform driver's agents between their last two ticks. A scene without navigation components
// costs nothing (spec R25): no crowd, no query, no allocation.

#include "Export.h"
#include "ecs/SubSystem.h"
#include "math/AABB.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "navigation/GridPathfinder.h"
#include "navigation/NavBakeCollector.h"
#include "navigation/NavDiagnostics.h"
#include "navigation/NavMesh.h"
#include "navigation/NavTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace jasperengine{

    // bake_navigation's modes: the tiles whose inputs changed, every tile, or none (removes it)
    enum class NavBakeMode : uint8_t{
        DIRTY,
        FULL,
        CLEAR
    };

    enum class NavBakeState : uint8_t{
        NOT_BAKED,
        BAKED,
        // baked, but some tiles' inputs changed since (countDirtyTiles)
        DIRTY,
        BAKING
    };

    struct JASPERENGINE_API NavBakeResult{
        bool ok = false;
        bool cancelled = false;
        std::string error;
        int tilesBuilt = 0;
        int tilesTotal = 0;
        int polys = 0;
        double milliseconds = 0.0;
        std::vector<std::string> warnings;
        // the engine logged the failure already (Log::error): the editor does not again (S53 R2)
        bool logged = false;

        // ---- S54 bake report (R16): why it failed, what it collected, the tiles it built
        nav::NavBakeError errorCode = nav::NavBakeError::None;
        // bakeErrorHint(errorCode)
        std::string hint;
        nav::NavCollectStats sources;
        std::vector<nav::NavTileReport> tiles;
        // filled on the first getLastBakeResult (islands label on demand, never in the landing):
        // m^2, -1 until then, the 16 largest islands (world bounds, m^2)
        float walkableArea = 0.0f;
        int islandCount = -1;
        std::vector<nav::NavIsland> islands;
    };

    struct JASPERENGINE_API NavSurfaceStatus{
        NavBakeState state = NavBakeState::NOT_BAKED;
        int tiles = 0;
        int polys = 0;
        // tiles whose inputs differ from the baked ones (-1 until countDirtyTiles ran)
        int dirtyTiles = -1;
        size_t bytes = 0;
        float progress = 0.0f;
        bool carving = false;
        // why the navmesh asset was refused ("" when it loaded or there is none)
        std::string loadError;
        // the agents and obstacles of its crowd
        int crowdAgents = 0;
        int crowdCapacity = 0;
        // carved tile builds left before its navmesh is complete (a bake landing in play, S53 R15)
        int queuedBuilds = 0;
        // ---- S55 queries: its crowd's query filters in use (of 16) and the agents on a fallback one
        int crowdFilters = 0;
        int crowdFilterOverflow = 0;
    };

    class JASPERENGINE_API NavigationSystem : public SubSystem{
    public:
        NavigationSystem(Scene* scene);
        virtual ~NavigationSystem();

        void load() override;
        void destroy() override;
        void draw() override;
        void update(double dt) override;
        void fixedUpdate(double dt) override;
        void onComponentAdded(Entity entity, ComponentId componentId) override;
        void onComponentRemoved(Entity entity, ComponentId componentId) override;

        // ---- queries (world positions); without a surface, the first surface of the agent type
        // that answers (entity order). Every query honours the filter's area costs (S55 R8).
        nav::NavPath calculatePath(const Vector3& from, const Vector3& to, const nav::QueryFilter& filter = {},
                                   const nav::PathOptions& options = {});
        bool samplePosition(const Vector3& point, float maxDistance, nav::NavHit& out, const nav::QueryFilter& filter = {});
        // true when the ray is blocked before `to` (Unity's NavMesh.Raycast)
        bool raycast(const Vector3& from, const Vector3& to, nav::NavHit& out, const nav::QueryFilter& filter = {});
        // the nearest wall within maxDistance (<= 0: S19's reach); false with out.distance =
        // maxDistance when none is that close
        bool findClosestEdge(const Vector3& point, nav::NavHit& out, const nav::QueryFilter& filter = {}, float maxDistance = -1.0f);
        // S54 R6: false without a search when the ends' islands are current and differ; why: the
        // failure of the answering surface (None when reachable)
        bool isReachable(const Vector3& from, const Vector3& to, const nav::QueryFilter& filter = {}, nav::PathFailure* why = nullptr);
        // the same on one surface
        nav::NavPath calculatePathOn(Entity surface, const Vector3& from, const Vector3& to, uint16_t areaMask = nav::ALL_AREAS);
        bool samplePositionOn(Entity surface, const Vector3& point, float maxDistance, nav::NavHit& out, uint16_t areaMask = nav::ALL_AREAS);
        bool raycastOn(Entity surface, const Vector3& from, const Vector3& to, nav::NavHit& out, uint16_t areaMask = nav::ALL_AREAS);
        bool findClosestEdgeOn(Entity surface, const Vector3& point, nav::NavHit& out, uint16_t areaMask = nav::ALL_AREAS);

        // ---- S55 queries (spec S55 R1-R9)
        static constexpr uint64_t DEFAULT_RANDOM_SEED = 0x5EED;
        // A random navmesh point of the filter's agent type: anywhere (uniform by area), around
        // query.center (reachable within query.radius, not uniform) and/or on the island of
        // query.islandOf. Draws come from the system's stream, or from query.seed's own.
        bool randomPoint(nav::NavHit& out, const nav::QueryFilter& filter = {}, const nav::RandomPointQuery& query = {});
        bool randomPointOn(Entity surface, nav::NavHit& out, const nav::QueryFilter& filter = {}, const nav::RandomPointQuery& query = {});
        // restarts the system's stream (a new scene, and Play, start at DEFAULT_RANDOM_SEED)
        void setRandomSeed(uint64_t seed);
        uint64_t getRandomSeed() const;
        // A controller step: from the polygon under `from` along the surface towards `to` (at most
        // 64 polygons). out.hit: stopped short of `to`. False when `from` is off the mesh.
        bool moveAlongSurface(const Vector3& from, const Vector3& to, nav::NavHit& out, const nav::QueryFilter& filter = {});
        // on one surface with a filter's mask and costs (its agent type is the surface's)
        nav::NavPath calculatePathOn(Entity surface, const Vector3& from, const Vector3& to, const nav::QueryFilter& filter,
                                     const nav::PathOptions& options = {});
        bool raycastOn(Entity surface, const Vector3& from, const Vector3& to, nav::NavHit& out, const nav::QueryFilter& filter);
        bool findClosestEdgeOn(Entity surface, const Vector3& point, nav::NavHit& out, const nav::QueryFilter& filter,
                               float maxDistance = -1.0f);
        bool moveAlongSurfaceOn(Entity surface, const Vector3& from, const Vector3& to, nav::NavHit& out, const nav::QueryFilter& filter = {});
        // A NavMeshAgent's own area costs (Unity's SetAreaCost): runtime only, gone at Stop; the
        // areas it does not set follow the project's costs. They apply to its crowd planning and
        // to the queries made with getAgentQueryFilter.
        void setAgentAreaCost(Entity agent, int area, float cost);
        float getAgentAreaCost(Entity agent, int area) const;
        void resetAgentAreaCost(Entity agent, int area);
        // the agent's agent type, area mask and own costs
        nav::QueryFilter getAgentQueryFilter(Entity agent) const;

        // ---- bakes (spec R4, R10)
        // Starts a bake: the inputs are collected now (main thread), the tiles build on the job
        // system (the Background lane in the editor, Normal in a game) or time-sliced (web).
        // The navmesh changes once every tile is built; a cancel keeps the previous one.
        // A bake that cannot start (nothing collected, too large, unavailable) is the surface's
        // last result too; code: why (S54)
        bool bake(Entity surface, NavBakeMode mode, std::string& error, nav::NavBakeError* code = nullptr);
        // Re-collects and rebuilds the tiles touching the world box (procedural levels)
        bool updateTiles(Entity surface, const AABB& worldBounds, std::string& error, nav::NavBakeError* code = nullptr);
        // A bake from start to finish on this thread (waiting for the jobs)
        NavBakeResult bakeNow(Entity surface, NavBakeMode mode);
        bool cancelBake(Entity surface);
        bool isBaking(Entity surface) const;
        bool isAnyBaking() const;
        // the last finished bake (null before any). Not const (S54): the first read fills its
        // island fields (main thread: the editor, Lua)
        const NavBakeResult* getLastBakeResult(Entity surface);
        // Lands finished bakes and runs time-sliced ones (the editor calls it every frame;
        // fixedUpdate and update do it in a game)
        void processBakes(double budgetMs);
        // Recollects the inputs and compares their hashes with the baked tiles' (the stale-bake
        // warning, the export check); -1 when the surface cannot be collected
        int countDirtyTiles(Entity surface, std::string* error = nullptr);
        NavSurfaceStatus getSurfaceStatus(Entity surface);

        // ---- navmesh data
        // The surface's navmesh, loading its asset when it changed (null when none)
        const nav::NavMesh* getNavMesh(Entity surface);
        // Replaces the surface's navmesh (an editor load, a test)
        bool setNavMeshData(Entity surface, nav::NavMeshData data, std::string& error);
        // the .navmesh bytes of the surface's navmesh (empty when not baked)
        std::vector<uint8_t> getNavMeshFile(Entity surface);
        // (Re)reads the surface's navMeshData asset
        bool reloadNavMesh(Entity surface, std::string& error);
        // The navmesh in memory is what the surface's navMeshData asset now holds (the editor
        // wrote the bake there): no reload when the reference changes to it
        void adoptNavMeshAsset(Entity surface);
        // The editor keeps every tile record (saving, incremental bakes); a game does not
        void setKeepBakeData(bool keep);
        bool isKeepBakeData() const { return keepBakeData; }
        // the bake runs its tiles on the Background lane (the editor) instead of Normal
        void setBakeInBackground(bool background);

        // ---- agents (NavMeshAgent)
        bool setDestination(Entity agent, const Vector3& target);
        void resetPath(Entity agent);
        bool warp(Entity agent, const Vector3& position);
        void move(Entity agent, const Vector3& offset);
        void completeOffMeshLink(Entity agent);
        // the corners ahead of the agent (world), its path as it steers now
        bool getAgentPath(Entity agent, std::vector<Vector3>& corners);
        // the surface or grid the agent walks (NULL_ENTITY: none)
        Entity getAgentSurface(Entity agent);
        // the link it is on (when isOnOffMeshLink)
        bool getCurrentOffMeshLink(Entity agent, Vector3& start, Vector3& end, Entity& link);

        // ---- grids (spec R15-R20)
        // the grid's cells (loading its asset, or sized empty), null when the entity has no NavGrid
        nav::NavGridMap* getGridMap(Entity grid);
        nav::GridPath findGridPath(Entity grid, nav::GridCell from, nav::GridCell to, const nav::GridQuery& query = {});
        // the path as world waypoints
        std::vector<Vector3> gridPathToWorld(Entity grid, const nav::GridPath& path);
        nav::GridCell gridWorldToCell(Entity grid, const Vector3& world);
        Vector3 gridCellToWorld(Entity grid, nav::GridCell cell);
        // a grid-local 2D point (NavGridMap's space) and height in the world (the overlays, the brush)
        Vector3 gridLocalToWorld(Entity grid, const Vector2& local, float height = 0.0f);
        bool setGridCellArea(Entity grid, nav::GridCell cell, uint8_t area);
        int fillGridRect(Entity grid, nav::GridCell a, nav::GridCell b, uint8_t area);
        // marks the cells under a world box Not Walkable (a building placed in a strategy game)
        int stampGridObstacle(Entity grid, const AABB& worldBounds, uint8_t area = nav::AREA_NOT_WALKABLE);
        // Rebuilds the cells from the grid's source (Tilemap or Terrain), keeping overridden cells
        bool rebuildGrid(Entity grid, std::string& error);
        // Rebuilds the grids with Auto Rebuild whose source changed (each fixed tick; the editor
        // calls it every frame in edit mode)
        void updateGridSources();
        // whether the grid holds a world point (agents find their grid by it)
        bool gridContains(Entity grid, const Vector3& world);
        // the cells grid agents hold at the current fixed tick (their lookahead, spec R18), ascending;
        // false when the entity has no NavGrid
        bool getGridReservedCells(Entity grid, std::vector<int>& cells);

        // ---- overlays (editor, spec R33): the surface's tiles in world space that `visible`
        // accepts (world bounds), at most maxTriangles; tileBounds holds 8 world corners per
        // tile (corner c: x max if c & 1, y max if c & 2, z max if c & 4, in the surface's frame)
        bool getDebugGeometry(Entity surface, const std::function<bool(const AABB& worldBounds)>& visible, size_t maxTriangles,
                              bool detailMesh, nav::NavDebugGeometry& out, uint32_t extras = 0);

        // ---- S54 diagnostics (docs/specs/S54-navigation-diagnostics.md; world positions, m, m^2).
        // Islands label on demand here (R8); path queries never label.
        // Where a point is (R10): over the agent type's surfaces, the on-mesh one with the
        // smallest snap distance, else the closest found
        nav::NavLocation locate(const Vector3& point, const nav::QueryFilter& filter = {}, float maxDistance = 5.0f);
        nav::NavLocation locateOn(Entity surface, const Vector3& point, uint16_t areaMask = nav::ALL_AREAS, float maxDistance = 5.0f);
        // the surface's islands, world bounds, m^2 (empty when not baked)
        std::vector<nav::NavIsland> getIslands(Entity surface);
        nav::NavCoverage getCoverage(Entity surface);
        // The box a query searches around a point on the surface (nav units; zero when not baked)
        Vector3 getSearchExtents(Entity surface);

        // every enabled link, per surface whose navmesh holds it (surface NULL_ENTITY: none does)
        struct LinkStatus {
            Entity link = NULL_ENTITY;
            Entity surface = NULL_ENTITY;
            int connections = 0;
            // every connection joined at both ends
            bool connected = false;
            // the first connection's ends (-1: unconnected)
            int islandStart = -1, islandEnd = -1;
        };
        std::vector<LinkStatus> getLinkStatus();

        // every tile of the surface's navmesh with its last build in this session (ms -1: not
        // built since it loaded, R17)
        std::vector<nav::NavTileReport> getTileReports(Entity surface);

        // The surface's inputs collected now, against its navmesh (the stale check, the
        // Inspector's estimate, the probe's data)
        struct InputReport {
            // tiles whose inputs differ from the baked ones (-1: not collected)
            int dirtyTiles = -1;
            std::vector<nav::NavTileCoord> dirty;
            int tilesWide = 0, tilesHigh = 0, tileSize = 0, nonEmptyTiles = 0;
            float cellSize = 0, agentRadius = 0;
            std::string error;
            nav::NavBakeError errorCode = nav::NavBakeError::None;
            // keepInput only
            std::shared_ptr<const nav::NavBakeInput> input;
        };
        InputReport inspectInputs(Entity surface, const nav::NavCollectOptions& options = {}, bool keepInput = false);

        // The scene's navigation problems (R13, R14), error first, then by entity and code
        std::vector<nav::NavFinding> getFindings(const nav::NavFindingOptions& options = {});

        // ---- S55 areas (docs/specs/S55-navigation-gameplay-api.md R11-R17; NavigationAreas.cpp).
        // A script volume (a world box): on every surface of the agent type (< 0: all), the
        // polygons whose centroid lies in it take `area` (Not Walkable: nothing walks there) until
        // removeAreaOverride. Handles end with the system (Stop). 0 for an area outside 0-15.
        uint32_t setAreaInVolume(const Vector3& center, const Vector3& size, const Quaternion& rotation, uint8_t area, int agentType = -1);
        bool updateAreaOverride(uint32_t handle, const Vector3& center, const Vector3& size, const Quaternion& rotation, uint8_t area);
        bool removeAreaOverride(uint32_t handle);
        bool hasAreaOverride(uint32_t handle) const;
        bool getAreaOverride(uint32_t handle, Vector3& center, Vector3& size, Quaternion& rotation, uint8_t& area) const;
        // Samples the runtime modifier volumes (Use Volume + Apply At Runtime) and link Activated
        // into the navmeshes: the editor every frame, fixedUpdate as step 1b in play. True when a
        // polygon changed.
        bool syncAreaOverrides();
        // a runtime modifier volume or a script volume (the overlay, get_navigation_info)
        struct AreaOverrideStatus {
            Entity modifier = NULL_ENTITY;
            uint32_t handle = 0;
            uint8_t area = 0;
            int agentType = -1;
            // on a navmesh now, and the ground polygons in it
            bool applied = false;
            int polygons = 0;
            // world from the unit box [-0.5, 0.5]^3
            Matrix4 box;
        };
        // the runtime modifier volumes by entity, then the script volumes by handle
        std::vector<AreaOverrideStatus> getAreaOverrides();
        // the polygons a runtime modifier volume or a script volume holds now (0: not applied)
        int getModifierAppliedPolygons(Entity modifier);
        int getAreaOverridePolygons(uint32_t handle);
        // R16, R17: whether agents may use the link, and whether one is crossing it now
        bool isLinkActive(Entity link) const;
        bool isLinkOccupied(Entity link) const;
        // the link's baked connections follow its current fields; false (logged) when a surface
        // must be rebaked
        bool updateLink(Entity link);
        // world, from its transform at the last fixed tick
        Vector3 getObstacleVelocity(Entity obstacle) const;
        Scene* getScene() const { return scene; }

        // ---- statistics
        int getCrowdCount() const;
        int getAgentCount() const;
        size_t getMemoryBytes() const;
        // fixed ticks that did navigation work (zero-cost proof: an empty scene leaves it alone)
        uint64_t getWorkingTicks() const { return workingTicks; }
        uint32_t getFixedTick() const { return fixedTick; }
        // path searches run on the main thread (calculatePath*, isReachable); crowd agents never
        // add to it (S53 R6)
        uint64_t getPathQueryCount() const { return pathQueryCount; }
        // the wall-clock ceiling of a tick's carving builds, ms (S53 R14); 0 turns it off
        void setCarveTimeCeiling(double ms) { carveTimeCeilingMs = ms; }
        // the tile-cache builds of the last fixed tick, every surface together
        int getCarveTileBuilds() const { return carveTileBuilds; }
        // the longest main-thread slice of a time-sliced (web, no threads) bake since the reset
        double getMaxBakeSliceMs() const { return maxBakeSliceMs; }
        void resetBakeSliceStats() { maxBakeSliceMs = 0.0; slowestBakeStepMs = 0.0; slowestBakeStep.clear(); bakeStageMaxMs.clear(); }
        // each Recast stage's longest time-sliced step since the reset, ms
        const std::map<std::string, double>& getBakeStageMaxMs() const { return bakeStageMaxMs; }
        // the longest single Recast stage of those slices, and which ("regions, tile 7")
        double getSlowestBakeStepMs() const { return slowestBakeStepMs; }
        const std::string& getSlowestBakeStep() const { return slowestBakeStep; }

    private:
        struct Runtime;
        // made on the first navigation component or query; null otherwise (spec R25)
        std::unique_ptr<Runtime> runtime;
        // S55: made on the first runtime volume, script volume or deactivated link (R18)
        struct AreaState;
        std::unique_ptr<AreaState> areas;
        bool areaSyncing = false;
        bool keepBakeData = false;
        bool bakeInBackground = false;
        uint64_t workingTicks = 0;
        uint32_t fixedTick = 0;
        uint64_t pathQueryCount = 0;
        double carveTimeCeilingMs = 4.0;
        int carveTileBuilds = 0;
        double maxBakeSliceMs = 0.0;
        double slowestBakeStepMs = 0.0;
        std::string slowestBakeStep;
        std::map<std::string, double> bakeStageMaxMs;

        // ---- S55 queries: the random point stream (R2)
        nav::NavRandom randomStream{DEFAULT_RANDOM_SEED};
        uint64_t randomSeed = DEFAULT_RANDOM_SEED;

        bool hasNavigationComponents() const;
        Runtime& rt();
        // ---- S55 areas
        nav::NavMesh* editableNavMesh(Entity surface);
        // its crowd has agents (they replan on area changes)
        bool hasAreaCrowd(Entity surface) const;
        // the surface's navmesh was replaced: its overrides and link states go to the new one now
        void onAreaMeshReplaced(Entity surface);
    };

}

#endif //NAVIGATION_SYSTEM_H
