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

    // Unity's NavMeshPathStatus
    enum class PathStatus : uint8_t {
        Complete,
        Partial,
        Invalid
    };

    // The plane a surface or grid lies in: XZ is 3D (Y up), XY is a 2D scene
    enum class NavPlane {
        XZ,
        XY
    };

    // What a query may walk on: the agent type whose navmesh it reads and the areas it may enter
    // (Unity's NavMeshQueryFilter)
    struct QueryFilter {
        int agentType = 0;
        uint16_t areaMask = ALL_AREAS;
    };

    // samplePosition / raycast / findClosestEdge (Unity's NavMeshHit)
    struct NavHit {
        Vector3 position;
        Vector3 normal;
        float distance = 0.0f;
        uint8_t area = 0;
        bool hit = false;
    };

    // calculatePath (Unity's NavMeshPath)
    struct NavPath {
        PathStatus status = PathStatus::Invalid;
        std::vector<Vector3> corners;
        float length = 0.0f;
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

}

#endif // NAV_TYPES_H
