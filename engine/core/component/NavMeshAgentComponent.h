// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_MESH_AGENT_COMPONENT_H
#define NAV_MESH_AGENT_COMPONENT_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "navigation/NavCrowd.h"
#include "navigation/NavTypes.h"
#include "util/FunctionSubscribe.h"

#include <cstdint>

namespace jasperengine{

    class NavMeshAgent;

    // What moves the GameObject along the agent's path
    enum class NavAgentDriver{
        TRANSFORM,              // the agent writes the Transform (the default)
        RIGIDBODY,              // the linear velocity of its Rigidbody (3D) or Rigidbody2D
        CHARACTER_CONTROLLER,   // CharacterController(2D).simpleMove with the velocity
        NONE                    // nothing: a script reads desiredVelocity / nextPosition
    };

    // Unity's NavMeshAgent (spec S19 R11, R12): moves its GameObject along paths on the navmesh
    // of its agent type (or across a NavGrid), avoiding other agents by priority. Paths are
    // asynchronous (pathPending), arrival fires onDestinationReached once per destination.
    struct JASPERENGINE_API NavMeshAgentComponent{
        // component checkbox: a disabled agent leaves its crowd
        bool enabled = true;

        int agentType = 0;
        float baseOffset = 0.0f;
        float speed = 3.5f;
        // degrees per second
        float angularSpeed = 120.0f;
        float acceleration = 8.0f;
        float stoppingDistance = 0.0f;
        bool autoBraking = true;
        float radius = 0.5f;
        float height = 2.0f;
        nav::AvoidanceQuality obstacleAvoidance = nav::AvoidanceQuality::High;
        // 0-99, lower is more important
        int avoidancePriority = 50;
        bool autoTraverseOffMeshLink = true;
        bool autoRepath = true;
        // a bit per area (1 << area)
        int areaMask = nav::ALL_AREAS;
        bool updatePosition = true;
        bool updateRotation = true;
        NavAgentDriver driver = NavAgentDriver::TRANSFORM;
        // the NavMeshSurface or NavGrid to walk; none: the surface of its agent type under it,
        // else the NavGrid containing it
        Entity surface = NULL_ENTITY;
        // ---- S55 agents: the stuck watchdog (spec S55 R25). While it walks a path, an agent that
        // stays within stuckSpeedRatio x speed x stuckTimeout of where it was for stuckTimeout
        // seconds fires onStuck (and asks for its destination again with repathWhenStuck)
        bool stuckDetection = false;
        float stuckSpeedRatio = 0.1f;
        // seconds
        float stuckTimeout = 2.0f;
        bool repathWhenStuck = true;

        // ---- runtime state (not saved; NavigationSystem)
        Vector3 destination;
        bool hasPath = false;
        bool isStopped = false;
        bool pathPending = false;
        nav::PathStatus pathStatus = nav::PathStatus::Invalid;
        float remainingDistance = 0.0f;
        Vector3 velocity;
        Vector3 desiredVelocity;
        Vector3 nextPosition;
        bool isOnNavMesh = false;
        bool isOnOffMeshLink = false;
        // ---- S55 agents: seconds without progress (the watchdog; 0 while it is off or not
        // walking) and why the last path failed (None after a later success or resetPath)
        float stuckTime = 0.0f;
        nav::PathFailure lastPathFailure = nav::PathFailure::None;

        FunctionSubscribe<void(NavMeshAgent)> onDestinationReached;
        // with why (spec S55 R27); a one-argument Lua function still works
        FunctionSubscribe<void(NavMeshAgent, nav::PathFailure)> onPathFailed;
        FunctionSubscribe<void(NavMeshAgent)> onOffMeshLinkStart;
        FunctionSubscribe<void(NavMeshAgent)> onOffMeshLinkEnd;
        FunctionSubscribe<void(NavMeshAgent)> onStuck;
    };

}

#endif //NAV_MESH_AGENT_COMPONENT_H
