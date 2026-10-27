// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CONTROLLER_COLLIDER_HIT_H
#define CONTROLLER_COLLIDER_HIT_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"

namespace jasperengine{

    // What a CharacterController's move ran into (Unity's ControllerColliderHit): one per body
    // it touched, after the move. Scripts get it in onControllerColliderHit(hit), C++ through
    // PhysicsSystem::onControllerColliderHit.
    struct JASPERENGINE_API ControllerColliderHit{
        Entity controller = NULL_ENTITY;    // the controller that moved
        Entity entity = NULL_ENTITY;        // what it hit (a Rigidbody's GameObject, or another controller)
        Vector3 point;                      // world contact point
        Vector3 normal;                     // world surface normal at it, pointing at the controller
        Vector3 moveDirection;              // the move's direction (normalized)
        float moveLength = 0.0f;            // the move's length

        Entity getController() const { return controller; }
        Entity getEntity() const { return entity; }
        Vector3 getPoint() const { return point; }
        Vector3 getNormal() const { return normal; }
        Vector3 getMoveDirection() const { return moveDirection; }
        float getMoveLength() const { return moveLength; }
    };

}

#endif //CONTROLLER_COLLIDER_HIT_H
