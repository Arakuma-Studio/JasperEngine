// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NAV_MESH_OBSTACLE_COMPONENT_H
#define NAV_MESH_OBSTACLE_COMPONENT_H

#include "Export.h"
#include "math/Vector3.h"

#include <cstdint>

namespace jasperengine{

    enum class NavObstacleShape{
        CAPSULE,    // an upright cylinder of radius and height
        BOX
    };

    // Unity's NavMeshObstacle (spec S19 R9): agents avoid it, or, with Carve on a surface with
    // runtime carving, it cuts a hole in the navmesh (re-cut when it moves more than the move
    // threshold, or once it has stood still for the time to stationary)
    struct JASPERENGINE_API NavMeshObstacleComponent{
        // component checkbox: a disabled obstacle neither blocks nor carves
        bool enabled = true;

        NavObstacleShape shape = NavObstacleShape::BOX;
        Vector3 center = Vector3(0.0f, 0.0f, 0.0f);
        // Box: the size; Capsule: radius and height
        Vector3 size = Vector3(1.0f, 1.0f, 1.0f);
        float radius = 0.5f;
        float height = 2.0f;
        bool carve = false;
        float moveThreshold = 0.1f;
        float timeToStationary = 0.5f;
        bool carveOnlyStationary = true;
    };

}

#endif //NAV_MESH_OBSTACLE_COMPONENT_H
