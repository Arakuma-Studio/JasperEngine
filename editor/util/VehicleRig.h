// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef EDITOR_VEHICLE_RIG_H
#define EDITOR_VEHICLE_RIG_H

// GameObject > 3D Object > Vehicle... (docs/specs/S96-physics-gaps.md REQ-42): a dynamic body with
// a box collider and four Wheel Colliders at the corners of the selected mesh's bounds (or of a new
// 2 x 1 x 4 m car body), one undo step.

#include "ecs/Entity.h"

#include <cstdint>
#include <string>

namespace jasperengine::editor {

    class Project;

    // `target`: the GameObject to make the vehicle of (its mesh's bounds), NULL_ENTITY for a new
    // one under `parent`. The vehicle's GameObject, NULL_ENTITY with `error` set on a failure.
    Entity createVehicleRig(Project* project, uint32_t sceneId, Entity target, Entity parent, std::string& error);

}

#endif
