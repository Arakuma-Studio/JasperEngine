// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_PLACEMENT_H
#define TERRAIN_PLACEMENT_H

// T1 R21: where a placement brush puts objects, as pure data the editor turns into commands. The
// same seed and inputs give the same plan on every platform (PCG32, no std distribution).

#include "Export.h"
#include "math/Vector2.h"

#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine::terrain {

    struct PlacementParams {
        float spacing = 1.0f;           // against every existing and planned point
        float minScale = 1.0f, maxScale = 1.0f;
        float rotationJitter = 1.0f;    // share of a full turn of random yaw
        uint32_t seed = 0;
    };

    struct PlacementStamp {
        float x = 0.0f, z = 0.0f;
        float radius = 0.0f;
        bool square = false;
    };

    struct PlacedObject {
        float x = 0.0f, z = 0.0f;       // terrain-local
        float yaw = 0.0f;               // radians
        float scale = 1.0f;
    };

    struct PlacementPlan {
        std::vector<PlacedObject> objects;
        int attempts = 0;
        int rejectedBySpacing = 0;
        int rejectedByRegion = 0;
    };

    // Whether a candidate may land here (the op's shape and filter)
    using PlacementAccept = bool (*)(void* user, float x, float z);

    // One random attempt per stamp, scattered inside it and clamped to bounds (T1 R21: a drag makes
    // one attempt per spaced stamp, not per event)
    JASPERENGINE_API PlacementPlan planPlacement(std::span<const PlacementStamp> stamps, const PlacementParams& params, std::span<const Vector2> existing,
                                                 Vector2 boundsMin, Vector2 boundsMax, PlacementAccept accept = nullptr, void* acceptUser = nullptr);

}

#endif // TERRAIN_PLACEMENT_H
