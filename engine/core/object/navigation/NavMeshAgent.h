// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_MESH_AGENT_H
#define NAV_MESH_AGENT_H

#include "object/EntityHandle.h"
#include "component/NavMeshAgentComponent.h"

#include <vector>

namespace jasperengine{

    class NavigationSystem;

    // Unity's NavMeshAgent (docs/navigation.md): Lua NavMeshAgent(scene, entity). Paths are
    // asynchronous: pathPending stays true until the crowd has one; arrival fires
    // onDestinationReached once per destination. On a 2D surface its lengths and speeds are in
    // metres (scaled by the physics' points per metre); what it reports is in scene units.
    class JASPERENGINE_API NavMeshAgent: public EntityHandle{
    private:
        NavMeshAgentComponent& component() const;
        NavigationSystem* system() const;

    public:
        NavMeshAgent(Scene* scene, Entity entity);
        virtual ~NavMeshAgent();

        NavMeshAgent(const NavMeshAgent& rhs);
        NavMeshAgent& operator=(const NavMeshAgent& rhs);
        NavMeshAgent(NavMeshAgent&& rhs) noexcept;
        NavMeshAgent& operator=(NavMeshAgent&& rhs) noexcept;

        bool isValid() const;

        // false when the destination cannot be planned at once (no surface, off the mesh)
        bool setDestination(const Vector3& target);
        Vector3 getDestination() const;
        void resetPath();
        // teleports onto the mesh, keeping the destination (its path is planned again)
        bool warp(const Vector3& position);
        // a relative move along the mesh surface (Unity's Move)
        void move(const Vector3& offset);
        // ends the link it waits on (autoTraverseOffMeshLink off), at the link's end
        void completeOffMeshLink();
        // the corners ahead, world positions
        std::vector<Vector3> getPathCorners() const;

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setAgentTypeID(int agentType);
        int getAgentTypeID() const;
        void setSpeed(float speed);
        float getSpeed() const;
        void setAngularSpeed(float degreesPerSecond);
        float getAngularSpeed() const;
        void setAcceleration(float acceleration);
        float getAcceleration() const;
        void setStoppingDistance(float distance);
        float getStoppingDistance() const;
        void setAutoBraking(bool autoBraking);
        bool isAutoBraking() const;
        void setRadius(float radius);
        float getRadius() const;
        void setHeight(float height);
        float getHeight() const;
        void setBaseOffset(float offset);
        float getBaseOffset() const;
        void setStopped(bool stopped);
        bool isStopped() const;
        void setAvoidancePriority(int priority);
        int getAvoidancePriority() const;
        void setObstacleAvoidanceType(nav::AvoidanceQuality quality);
        nav::AvoidanceQuality getObstacleAvoidanceType() const;
        void setAreaMask(int mask);
        int getAreaMask() const;
        void setAutoRepath(bool autoRepath);
        bool isAutoRepath() const;
        void setAutoTraverseOffMeshLink(bool autoTraverse);
        bool isAutoTraverseOffMeshLink() const;
        void setUpdatePosition(bool update);
        bool isUpdatePosition() const;
        void setUpdateRotation(bool update);
        bool isUpdateRotation() const;
        void setDriver(NavAgentDriver driver);
        NavAgentDriver getDriver() const;
        void setSurface(Entity surface);
        Entity getSurface() const;

        float getRemainingDistance() const;
        bool isPathPending() const;
        bool hasPath() const;
        nav::PathStatus getPathStatus() const;
        Vector3 getVelocity() const;
        Vector3 getDesiredVelocity() const;
        Vector3 getNextPosition() const;
        bool isOnNavMesh() const;
        bool isOnOffMeshLink() const;
        // the link it is on: false when it is on none
        bool getCurrentOffMeshLink(Vector3& start, Vector3& end, Entity& link) const;

        // ---- S55 queries

        // its own area cost (Unity's SetAreaCost), runtime only: it applies to its crowd planning
        // and its queries; the areas it does not set cost the project's
        void setAreaCost(int area, float cost);
        // its cost of an area: its own, else the project's
        float getAreaCost(int area) const;
        // back to the project's cost
        void resetAreaCost(int area);

        // ---- S55 agents (docs/navigation.md "Scripted paths and root motion", "Stuck agents")

        // a path from where it stands to the target, planned now with its mask and costs (Unity's
        // CalculatePath); its own path is unchanged
        nav::NavPath calculatePath(const Vector3& target) const;
        // follows a path of calculatePath (Unity's SetPath); false (and no path) when refused
        bool setPath(const nav::NavPath& path);
        // its corridor as a path, from its position (Unity's path)
        nav::NavPath getPath() const;
        // the corner it steers to now
        Vector3 getSteeringTarget() const;
        // its path crosses a polygon that is gone or closed to it
        bool isPathStale() const;
        // root motion: its simulation position, moved along the surface (the Transform follows only
        // with updatePosition)
        void setNextPosition(const Vector3& position);
        // its velocity for the next fixed tick only
        void setVelocity(const Vector3& velocity);
        // from its position with its mask and costs: true when blocked before the target
        bool raycast(const Vector3& target, nav::NavHit& hit) const;
        bool findClosestEdge(nav::NavHit& hit) const;
        // along its path up to maxDistance: true where it first enters an area outside areaMask
        bool samplePathPosition(int areaMask, float maxDistance, nav::NavHit& hit) const;
        // the next link along its path (Unity's nextOffMeshLinkData)
        bool getNextOffMeshLink(Vector3& start, Vector3& end, Entity& link) const;

        void setStuckDetection(bool detect);
        bool isStuckDetection() const;
        void setStuckSpeedRatio(float ratio);
        float getStuckSpeedRatio() const;
        void setStuckTimeout(float seconds);
        float getStuckTimeout() const;
        void setRepathWhenStuck(bool repath);
        bool isRepathWhenStuck() const;
        float getStuckTime() const;
        nav::PathFailure getLastPathFailure() const;

        FunctionSubscribe<void(NavMeshAgent)>& getOnDestinationReached();
        FunctionSubscribe<void(NavMeshAgent, nav::PathFailure)>& getOnPathFailed();
        FunctionSubscribe<void(NavMeshAgent)>& getOnOffMeshLinkStart();
        FunctionSubscribe<void(NavMeshAgent)>& getOnOffMeshLinkEnd();
        FunctionSubscribe<void(NavMeshAgent)>& getOnStuck();
    };

}

#endif //NAV_MESH_AGENT_H
