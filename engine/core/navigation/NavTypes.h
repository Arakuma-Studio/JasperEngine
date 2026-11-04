// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_TYPES_H
#define NAV_TYPES_H

// The shared vocabulary of navigation (docs/navigation.md, docs/specs/S19-navigation.md): areas,
// agent types, query filters and results, and the project's navigation settings. The core
// library (engine/core/navigation) has no ECS dependency; NavigationSystem binds it to scenes.
//
// Coordinates: a navmesh lives in its surface's frame (its "nav space"): the surface's world
// position and rotation (scale ignored, as Unity does), with Recast's Y up. A 2D surface (plane
// XY) maps world (x, y) to nav (x, 0, y). Queries of NavigationSystem take and return world
// positions; NavMesh works in nav space.

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#ifndef JASPERENGINE_NAVIGATION
    #define JASPERENGINE_NAVIGATION 1
#endif

namespace jasperengine::nav {

    // Areas are poly flags (spec D5): area a has the flag 1 << a, so an area mask is Detour's
    // native include filter. 16 areas; Unity has 32 (a documented deviation).
    constexpr int AREA_COUNT = 16;
    constexpr uint8_t AREA_WALKABLE = 0;
    constexpr uint8_t AREA_NOT_WALKABLE = 1;
    constexpr uint8_t AREA_JUMP = 2;
    // the first area a project may define
    constexpr uint8_t FIRST_USER_AREA = 3;
    constexpr uint16_t ALL_AREAS = 0xFFFF;

    // Lua's NavAreas masks (a bit per area)
    enum class AreaMask : int {
        All = 65535,
        Walkable = 1,
        NotWalkable = 2,
        Jump = 4
    };

    // Unity's NavMeshPathStatus
    enum class PathStatus : uint8_t {
        Complete,
        Partial,
        Invalid
    };

    // ---- S54 diagnostics: why a path is not Complete (docs/specs/S54-navigation-diagnostics.md
    // R1-R4). Shared with S55 (onPathFailed); values may only be appended.
    enum class PathFailure : uint8_t {
        None,
        // no enabled, active NavMeshSurface of the agent type (or the given entity is not one)
        NoSurface,
        // surfaces exist, none has a navmesh (never baked, cleared, its asset refused)
        NotBaked,
        // no polygon in the surface's search box around the start (or the end)
        StartOffMesh,
        EndOffMesh,
        // a polygon is there, but its area is outside the query's mask
        StartFiltered,
        EndFiltered,
        // the ends never connect (separate islands, or only one-way links)
        Disconnected,
        // they connect through areas the mask leaves out
        BlockedByArea,
        // the search ran out of its 2048 nodes (or 2048 corridor polygons) first
        OutOfNodes
    };

    // "none", "no_surface", ... (MCP, Lua and the docs use these names)
    JASPERENGINE_API const char* pathFailureName(PathFailure failure);
    // one English sentence per value (the Path Tester, MCP text, docs)
    JASPERENGINE_API const char* pathFailureDescription(PathFailure failure);
    // false (out unchanged) for an unknown name
    JASPERENGINE_API bool pathFailureFromName(const std::string& name, PathFailure& out);

    // The plane a surface or grid lies in: XZ is 3D (Y up), XY is a 2D scene
    enum class NavPlane {
        XZ,
        XY
    };

    // ---- S55 queries

    // A path corner's flags (Detour's DT_STRAIGHTPATH_* values)
    enum CornerFlag : uint8_t {
        CORNER_START = 1,
        CORNER_END = 2,
        // the corner where an off-mesh link starts
        CORNER_OFF_MESH_LINK = 4
    };

    // What a query may walk on: the agent type whose navmesh it reads, the areas it may enter and
    // its own area costs (Unity's NavMeshQueryFilter). Excluding an area is clearing its mask bit.
    struct JASPERENGINE_API QueryFilter {
        int agentType = 0;
        uint16_t areaMask = ALL_AREAS;
        // the areas whose cost this filter sets (a bit per area); the others cost the project's
        uint16_t costMask = 0;
        std::array<float, AREA_COUNT> areaCosts{};

        // the cost per metre of an area for this filter's queries, clamped to >= 1 (A*'s estimate
        // needs it; one warning per process)
        void setAreaCost(int area, float cost);
        // the filter's cost, else the project's (0 for an area outside 0-15)
        float getAreaCost(int area) const;
        // back to the project's cost
        void resetAreaCost(int area);
    };

    // samplePosition / raycast / findClosestEdge / randomPoint / moveAlongSurface (Unity's NavMeshHit)
    struct NavHit {
        Vector3 position;
        Vector3 normal;
        float distance = 0.0f;
        uint8_t area = 0;
        bool hit = false;
        // raycast: the cost of the travelled part (metres x the filter's area costs)
        float cost = 0.0f;
    };

    // calculatePath (Unity's NavMeshPath)
    struct NavPath {
        PathStatus status = PathStatus::Invalid;
        std::vector<Vector3> corners;
        float length = 0.0f;

        // ---- S54 (R2, R4): failure is None exactly when status is Complete; Invalid goes with
        // NoSurface, NotBaked and the four end reasons, Partial with Disconnected, BlockedByArea
        // and OutOfNodes
        PathFailure failure = PathFailure::NoSurface;
        // the surface that answered (NULL_ENTITY: none did)
        Entity surface = NULL_ENTITY;
        // the ends snapped to the mesh (the requested point when that end found no polygon), and
        // how far they moved (< 0: no polygon)
        Vector3 start, end;
        float startSnapDistance = -1.0f, endSnapDistance = -1.0f;
        // the ends' islands, -1 unless the surface's islands were current (queries never label)
        int startIsland = -1, endIsland = -1;
        // the last corner (Complete, Partial; gap 0 when Complete) and how far it is from the
        // snapped end, or from the requested end when the end is off the mesh. An Invalid path
        // has no corners: the snapped start (else the requested one) and its distance to the end.
        Vector3 closestPoint;
        float gap = 0.0f;
        // ---- S55 queries
        // per corner: the area of the stretch that starts there (the last: the area at the end)
        std::vector<uint8_t> cornerAreas;
        // per corner: CornerFlag bits
        std::vector<uint8_t> cornerFlags;
        // the sum of each stretch's length x its area cost (a link at its own cost per unit)
        float cost = 0.0f;
        // the length walked in each area
        std::array<float, AREA_COUNT> areaLengths{};
        // engine: the polygon corridor (Detour refs) and the navmesh revision it was planned on
        std::vector<uint32_t> polys;
        uint64_t meshRevision = 0;
    };

    // calculatePath options: areaCrossings adds a corner wherever the path changes area
    struct PathOptions {
        bool areaCrossings = false;
    };

    // randomPoint (spec S55 R1): anywhere (uniform by area), around a point (reachable from it
    // within radius, not uniform) and/or on the island of a point; seeded: a private stream
    struct RandomPointQuery {
        bool around = false;
        Vector3 center;
        float radius = 0.0f;
        bool sameIsland = false;
        Vector3 islandOf;
        bool seeded = false;
        uint64_t seed = 0;
    };

    // ---- S54 diagnostics (R7-R10)

    // locate(): where a point is on the navmesh
    struct NavLocation {
        // a polygon is in the surface's search box (where a path from here would start)
        bool onMesh = false;
        // a polygon within maxDistance (always true when onMesh)
        bool found = false;
        // the nearest point on it, and the distance to it (< 0: none found)
        Vector3 position;
        float snapDistance = -1.0f;
        int island = -1;
        uint8_t area = 0;
        Entity surface = NULL_ENTITY;
    };

    // A connected component of a navmesh's polygons (links included, either direction)
    struct NavIsland {
        // discovery order over the tiles ascending (y, x, layer), then polygon index
        int id = -1;
        // m^2 (NavigationSystem; nav units^2 on NavMesh)
        float area = 0.0f;
        int polys = 0;
        Vector3 boundsMin, boundsMax;
    };

    struct NavCoverage {
        // m^2: the ground polygons, the largest island, the non-empty tiles' footprints
        float walkableArea = 0.0f;
        float largestIslandArea = 0.0f;
        float tilesArea = 0.0f;
        // walkableArea / tilesArea (0 without tiles)
        float fraction = 0.0f;
        int islandCount = 0;
    };

    // One row of the project's area table (Window > AI > Navigation > Areas)
    struct AreaSettings {
        std::string name;
        // the traversal cost per metre, >= 1
        float cost = 1.0f;
        // sRGB, the overlays' tint
        Vector4 color = Vector4(0.0f, 0.75f, 1.0f, 1.0f);

        bool operator==(const AreaSettings& other) const {
            return name == other.name && cost == other.cost && color == other.color;
        }
        bool operator!=(const AreaSettings& other) const { return !(*this == other); }
    };

    // One agent type (Window > AI > Navigation > Agents). Ids are stable: a surface, agent or link
    // names its type by id, so removing a type never renames another.
    struct AgentTypeSettings {
        int id = 0;
        std::string name = "Humanoid";
        float radius = 0.5f;
        float height = 2.0f;
        float stepHeight = 0.4f;
        // degrees
        float maxSlope = 45.0f;

        bool operator==(const AgentTypeSettings& other) const {
            return id == other.id && name == other.name && radius == other.radius && height == other.height &&
                   stepHeight == other.stepHeight && maxSlope == other.maxSlope;
        }
        bool operator!=(const AgentTypeSettings& other) const { return !(*this == other); }
    };

    // The project's navigation block (project.yaml `navigation:`). The editor sets it from the
    // project, an exported game from its generated main.cpp.
    struct JASPERENGINE_API NavigationSettings {
        std::array<AreaSettings, AREA_COUNT> areas;
        std::vector<AgentTypeSettings> agentTypes;
        // agents of one crowd (one surface and agent type); a crowd grows past it, with a warning
        int crowdMaxAgents = 128;
        // the main-thread time a bake may take per frame without worker threads (web), ms
        float webBakeBudgetMs = 4.0f;
        // grid path requests solved per fixed tick
        int maxGridSolvesPerTick = 64;
        // tile-cache builds a carving surface does per fixed tick (S53 R14); 0: Auto
        int carveTilesPerTick = 0;

        NavigationSettings();

        // Area 0 Walkable (1), 1 Not Walkable, 2 Jump (2), 3-15 "Area 3".."Area 15" (1); one
        // agent type, Humanoid 0.5 / 2 / 0.4 / 45
        static NavigationSettings defaults();

        // null when the id is unknown
        const AgentTypeSettings* findAgentType(int id) const;
        const AgentTypeSettings* findAgentType(const std::string& name) const;
        // the agent type to use for an id: its own, else the first (Humanoid), else the defaults
        AgentTypeSettings agentTypeOrDefault(int id) const;
        // the smallest unused id
        int nextAgentTypeId() const;
        // area by name ("Walkable", ...), -1 when none
        int findArea(const std::string& name) const;
        // carveTilesPerTick, Auto resolved: 4 on desktop, 2 on Android, iOS and the web
        int effectiveCarveTilesPerTick() const;

        // costs clamped to >= 1, names and fixed areas restored, at least one agent type,
        // positive sizes, unique ids
        void sanitize();

        bool operator==(const NavigationSettings& other) const;
        bool operator!=(const NavigationSettings& other) const { return !(*this == other); }
    };

    // The process-wide settings every navigation system reads; setting them bumps the version,
    // so systems refresh their filters (area costs) at their next fixed tick.
    JASPERENGINE_API const NavigationSettings& getSettings();
    JASPERENGINE_API void setSettings(const NavigationSettings& settings);
    JASPERENGINE_API uint64_t getSettingsVersion();

    // Whether the engine was built with Recast/Detour (JASPERENGINE_NAVIGATION). Without it every
    // navmesh bake, query and crowd fails and logs once; grids work either way.
    constexpr bool isNavMeshAvailable() { return JASPERENGINE_NAVIGATION != 0; }

    // 64-bit content hash of a navigation settings snapshot's area table (names, costs, colours)
    JASPERENGINE_API uint64_t hashAreaTable(const NavigationSettings& settings);

    JASPERENGINE_API const char* pathStatusName(PathStatus status);

    // ---- S55 queries
    // an area cost as queries use it: >= 1 (a lower or non-finite cost warns, once per process)
    JASPERENGINE_API float clampAreaCost(float cost);

}

#endif // NAV_TYPES_H
