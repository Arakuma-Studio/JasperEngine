// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LATTICE_FOLLOWER_COMPONENT_H
#define LATTICE_FOLLOWER_COMPONENT_H

#include "Engine.h"
#include "ecs/Entity.h"

namespace jasperengine{

    // Places this entity where the `rest` entity's transform ends up once deformed by
    // `lattice` (lights, emitters, props on a squashed surface). Spec §6.3.
    struct JASPERENGINE_API LatticeFollowerComponent{
        bool enabled = true;
        Entity lattice = NULL_ENTITY;
        Entity rest = NULL_ENTITY;
        bool applyPosition = true;
        bool applyRotation = true;
        bool applyScale = false;
        float weight = 1.0f;
    };

}

#endif //LATTICE_FOLLOWER_COMPONENT_H
