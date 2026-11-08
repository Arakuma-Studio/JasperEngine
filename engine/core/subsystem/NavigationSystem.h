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
#include "math/Vector3.h"
#include "navigation/GridPathfinder.h"
#include "navigation/NavMesh.h"
#include "navigation/NavTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace jasperengine{

    namespace nav {
        struct NavWorldContext;
    }

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
        // that answers (entity order)
        nav::NavPath calculatePath(const Vector3& from, const Vector3& to, const nav::QueryFilter& filter = {});
        bool samplePosition(const Vector3& point, float maxDistance, nav::NavHit& out, const nav::QueryFilter& filter = {});
        // true when the ray is blocked before `to` (Unity's NavMesh.Raycast)
        bool raycast(const Vector3& from, const Vector3& to, nav::NavHit& out, const nav::QueryFilter& filter = {});
        bool findClosestEdge(const Vector3& point, nav::NavHit& out, const nav::QueryFilter& filter = {});
        bool isReachable(const Vector3& from, const Vector3& to, const nav::QueryFilter& filter = {});
        // the same on one surface
        nav::NavPath calculatePathOn(Entity surface, const Vector3& from, const Vector3& to, uint16_t areaMask = nav::ALL_AREAS);
        bool samplePositionOn(Entity surface, const Vector3& point, float maxDistance, nav::NavHit& out, uint16_t areaMask = nav::ALL_AREAS);
        bool raycastOn(Entity surface, const Vector3& from, const Vector3& to, nav::NavHit& out, uint16_t areaMask = nav::ALL_AREAS);
        bool findClosestEdgeOn(Entity surface, const Vector3& point, nav::NavHit& out, uint16_t areaMask = nav::ALL_AREAS);

        // ---- bakes (spec R4, R10)
        // Starts a bake: the inputs are collected now (main thread), the tiles build on the job
        // system (the Background lane in the editor, Normal in a game) or time-sliced (web).
        // The navmesh changes once every tile is built; a cancel keeps the previous one.
        bool bake(Entity surface, NavBakeMode mode, std::string& error);
        // Re-collects and rebuilds the tiles touching the world box (procedural levels)
        bool updateTiles(Entity surface, const AABB& worldBounds, std::string& error);
        // A bake from start to finish on this thread (waiting for the jobs)
        NavBakeResult bakeNow(Entity surface, NavBakeMode mode);
        bool cancelBake(Entity surface);
        bool isBaking(Entity surface) const;
        bool isAnyBaking() const;
        // the last finished bake (null before any)
        const NavBakeResult* getLastBakeResult(Entity surface) const;
        // Lands finished bakes and runs time-sliced ones (the editor calls it every frame;
        // fixedUpdate and update do it in a game)
        void processBakes(double budgetMs);
        // Recollects the inputs and compares their hashes with the baked tiles' (the stale-bake
        // warning, the export check); -1 when the surface cannot be collected
        int countDirtyTiles(Entity surface, std::string* error = nullptr);
        NavSurfaceStatus getSurfaceStatus(Entity surface);

        // ---- Terrain Worlds (T4 R25)
        // Follow Terrain World Focus from a script: a single-threaded web build follows only then
        void setFollowTerrainWorldFocus(Entity surface, bool follow);
        // what the surface keeps of its Terrain Worlds (its Follow region and last step); null before any
        const nav::NavWorldContext* getTerrainWorldContext(Entity surface) const;

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

        // ---- overlays (editor, spec R33): the surface's tiles in world space that `visible`
        // accepts (world bounds), at most maxTriangles; tileBounds holds 8 world corners per
        // tile (corner c: x max if c & 1, y max if c & 2, z max if c & 4, in the surface's frame)
        bool getDebugGeometry(Entity surface, const std::function<bool(const AABB& worldBounds)>& visible, size_t maxTriangles,
                              bool detailMesh, nav::NavDebugGeometry& out);

        // ---- statistics
        int getCrowdCount() const;
        int getAgentCount() const;
        size_t getMemoryBytes() const;
        // fixed ticks that did navigation work (zero-cost proof: an empty scene leaves it alone)
        uint64_t getWorkingTicks() const { return workingTicks; }
        uint32_t getFixedTick() const { return fixedTick; }
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
        bool keepBakeData = false;
        bool bakeInBackground = false;
        uint64_t workingTicks = 0;
        uint32_t fixedTick = 0;
        double maxBakeSliceMs = 0.0;
        double slowestBakeStepMs = 0.0;
        std::string slowestBakeStep;
        std::map<std::string, double> bakeStageMaxMs;

        bool hasNavigationComponents() const;
        Runtime& rt();
        // the fixed tick's Follow steps of surfaces following a Terrain World's focus
        void followTerrainWorlds();
    };

}

#endif //NAVIGATION_SYSTEM_H
