// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The dtCrowd wrapper and its four obstacle avoidance presets (every field of
// dtObstacleAvoidanceParams set, or the sampled velocity is zero) follow OpenEngine's
// ObstacleAvoidance.cpp (Engine/Modules/Pathfinding/Source/ObstacleAvoidance.cpp), Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#ifndef NAV_CROWD_H
#define NAV_CROWD_H

// One dtCrowd: the agents of one surface and agent type (spec R14), with Unity's avoidance
// quality and priority (R13, the DetourCrowd patch). Off-mesh links are traversed by the crowd's
// owner, not by Detour: an agent reaching a link start is handed over (takeOffMeshLinks), moved
// along the link linearly at its speed (or held for completeOffMeshLink), then handed back.
// Positions are in the navmesh's nav space. Main thread.

#include "Export.h"
#include "navigation/NavMesh.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace jasperengine::nav {

    // Unity's ObstacleAvoidanceType
    enum class AvoidanceQuality {
        None,
        Low,
        Medium,
        Good,
        High
    };

    struct NavCrowdAgentParams {
        float radius = 0.5f;
        float height = 2.0f;
        float maxSpeed = 3.5f;
        float maxAcceleration = 8.0f;
        AvoidanceQuality quality = AvoidanceQuality::High;
        // 0-99, lower is more important
        uint8_t priority = 50;
        uint16_t areaMask = ALL_AREAS;
        // a non-carving obstacle: it never steers and every agent avoids it
        bool obstacle = false;
        // ---- S55 queries
        // the agent's own area costs; 0: the project's (which follow its later edits)
        float areaCosts[AREA_COUNT] = {};
    };

    // An agent's movement request state (dtCrowd's)
    enum class NavCrowdTarget : uint8_t {
        None,
        Pending,
        Valid,
        Failed
    };

    // What the corridor of an agent with a valid target holds (S53 R6, D5): the way to the
    // target's polygon, its first 256 polygons (the search reached the target), or the way to
    // the polygon nearest an unreachable target
    enum class NavCrowdPath : uint8_t {
        Complete,
        Truncated,
        Partial
    };

    // A link an agent reached this update
    struct NavLinkEntry {
        int agent = -1;
        uint32_t userId = 0;
        Vector3 start;
        Vector3 end;
    };

    class JASPERENGINE_API NavCrowd {
    public:
        NavCrowd();
        ~NavCrowd();
        NavCrowd(const NavCrowd&) = delete;
        NavCrowd& operator=(const NavCrowd&) = delete;

        // maxAgentRadius: the largest radius an agent may have (it sizes the proximity grid and
        // the placement search)
        bool init(const NavMesh& mesh, int maxAgents, float maxAgentRadius);
        void release();
        bool isValid() const;
        int getCapacity() const;
        int getAgentCount() const;
        // the mesh the crowd was made for
        const NavMesh* getMesh() const;

        // -1 when the crowd is full; an agent off the mesh is added with isOnMesh() false
        int addAgent(const Vector3& position, const NavCrowdAgentParams& params);
        void removeAgent(int agent);
        void setParams(int agent, const NavCrowdAgentParams& params);
        const NavCrowdAgentParams& getParams(int agent) const;

        // the target must be on the mesh (snapped by the caller); false when it is not
        bool requestMoveTarget(int agent, const Vector3& target);
        void resetMoveTarget(int agent);
        NavCrowdTarget getTargetState(int agent) const;
        bool isPathPartial(int agent) const;
        NavCrowdPath getPathState(int agent) const;
        // the polygons of the agent's corridor
        int getCorridorSize(int agent) const;
        Vector3 getTargetPosition(int agent) const;

        // One crowd step (spec D2 step 5); the path queue runs 100 iterations at most
        void update(float dt);

        Vector3 getPosition(int agent) const;
        Vector3 getVelocity(int agent) const;
        Vector3 getDesiredVelocity(int agent) const;
        bool isOnMesh(int agent) const;
        // the polygon under the agent and its area
        uint8_t getArea(int agent) const;

        // Moves the agent along the mesh surface to `position` (a body that moved it, Move()):
        // the corridor follows, the path is kept
        void movePosition(int agent, const Vector3& position);
        // Teleports the agent (Warp): the corridor restarts there, the target is kept and
        // re-requested. False when nothing of the mesh is near the position.
        bool teleport(int agent, const Vector3& position);
        // An obstacle's state, from its transform
        void setObstacleState(int agent, const Vector3& position, const Vector3& velocity);
        // the velocity it will integrate (zeroed for a stop)
        void setVelocity(int agent, const Vector3& velocity);

        // Distance along the corridor to the target (up to maxCorners corners, then straight),
        // and whether the last corner found is the path's end
        float getRemainingDistance(int agent, bool& reachedEnd, int maxCorners = 64) const;
        // the corners dtCrowd steers by, then the rest of the way as the crow flies: what an
        // agent's remainingDistance shows each tick (exact once the end is among the corners)
        float getSteeringRemaining(int agent, bool& reachedEnd) const;
        // The same from the corners the crowd steered by in its last update (at most 4, no search):
        // exact when reachedEnd, else a lower bound
        float getSteeringDistance(int agent, bool& reachedEnd) const;
        // the corners ahead (the steering path), at most maxCorners
        void getCorners(int agent, std::vector<Vector3>& corners, int maxCorners = 64) const;

        // Agents that reached an off-mesh link start since the last call: they are now off the
        // crowd's steering (on the link) until completeOffMeshLink. Call before update().
        void takeOffMeshLinks(float lookahead, std::vector<NavLinkEntry>& entries);
        // dtCrowd started a traversal itself (a replan put a link within its trigger this step):
        // the agents now traversing on Detour's own animation, after update()
        void takeDetourTraversals(std::vector<int>& agents);
        bool isTraversingLink(int agent) const;
        // places an agent on a link (its own traversal) and hands it back at the end
        void setLinkPosition(int agent, const Vector3& position, const Vector3& velocity);
        void completeOffMeshLink(int agent, const Vector3& end);

        // the area costs of every filter from the project's settings and the mesh's link costs
        void refreshFilters();

        // ---- S55 queries: Detour's 16 query filters, keyed by area mask and costs, shared by
        // the agents with the same key and freed when none uses them (spec S55 R10)
        // the filters in use (the all-areas filter included)
        int getFilterCount() const;
        // the agents on a fallback filter now (no free filter: one with their mask, else all areas)
        int getFilterOverflowCount() const;
        bool isFilterFallback(int agent) const;

        // what the crowd holds (agents, grids, path queue), approximately
        size_t getMemoryBytes() const;

        // ---- S55 areas (R15): every walking agent whose whole corridor (not dtCrowd's 10-polygon
        // lookahead) holds one of the changed polygons (sorted) requests its target again; their
        // indices, ascending. Agents on a link finish it.
        void replanCrossing(const std::vector<uint32_t>& changedPolys, std::vector<int>& replanned);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}

#endif // NAV_CROWD_H
