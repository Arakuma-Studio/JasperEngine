// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef CHARACTER_COLLISION_H
#define CHARACTER_COLLISION_H

namespace jasperengine{

    // CharacterController(2D).move's result (Unity's CollisionFlags): the parts of the capsule
    // that touched something during the last move
    enum CharacterCollisionFlags : int {
        CharacterCollision_None = 0,
        CharacterCollision_Sides = 1,
        CharacterCollision_Above = 2,
        CharacterCollision_Below = 4
    };

}

#endif //CHARACTER_COLLISION_H
