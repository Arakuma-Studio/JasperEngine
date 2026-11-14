// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef PHYSICSQUERYOPTIONS_H
#define PHYSICSQUERYOPTIONS_H

// The options every Unity-style physics query takes (docs/specs/S96-physics-gaps.md D4): the
// casts, overlaps and checks of PhysicsQueries3D / PhysicsQueries2D and the Physics /
// Physics2D ray casts. Apart from Ray.h so that header stays free of the query modules.

#include "Entity.h"
#include "registry/TagManager.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace jasperengine{

    // Unity QueryTriggerInteraction: USE_GLOBAL follows Physics.queriesHitTriggers (3D) or
    // Physics2D.queriesHitTriggers (2D)
    enum class QueryTriggerInteraction{
        USE_GLOBAL,
        IGNORE,
        COLLIDE
    };

    // Unity CapsuleDirection2D: the axis of a 2D capsule's size
    enum class CapsuleDirection2D{
        VERTICAL,
        HORIZONTAL
    };

    struct QueryOptions{
        uint32_t layerMask = LAYER_MASK_DEFAULT_RAYCAST;
        // the bodies of these GameObjects are skipped (NULL_ENTITY entries are ignored)
        const std::vector<Entity>* ignore = nullptr;
        QueryTriggerInteraction triggers = QueryTriggerInteraction::USE_GLOBAL;
        // 3D: a collider containing the start (ray) or overlapping it (shape cast) is reported at
        // distance 0; false is Unity's rule (not reported)
        bool hitFromInside = false;
        // 3D mesh and heightfield triangles seen from behind; unset: Physics.queriesHitBackfaces
        std::optional<bool> hitBackfaces;
        // 2D: a collider containing the start is reported at distance 0; unset:
        // Physics2D.queriesStartInColliders
        std::optional<bool> startInColliders;
        // the most hits a ...All query or an overlap returns (the nearest ones for casts)
        size_t maxHits = 256;
    };

}

#endif //PHYSICSQUERYOPTIONS_H
