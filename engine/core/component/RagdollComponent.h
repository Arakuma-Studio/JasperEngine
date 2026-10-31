// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RAGDOLL_COMPONENT_H
#define RAGDOLL_COMPONENT_H

#include "Engine.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace jasperengine{

    // A character's ragdoll (Unity's Ragdoll Wizard pattern): the Rigidbodies, colliders and
    // joints on its bone GameObjects are ordinary components (made by the "3D Object > Ragdoll..."
    // wizard or by hand); this switches them between animated (kinematic parts following the
    // animated bones) and simulated (dynamic parts moving the bones, and so the skin).
    struct JASPERENGINE_API RagdollComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        // the state: false = animated, true = simulated (the start state when saved)
        bool simulated = false;
        // the subtree holding the parts (every Rigidbody below it, itself excluded); none = this
        // GameObject
        Entity root = NULL_ENTITY;
        // the Animation Player paused while simulated; none = the first one on root, its
        // ancestors, then its descendants
        Entity animator = NULL_ENTITY;
        // simulated starts with the velocities the parts followed the animation with
        bool inheritVelocity = true;
        // false: no part collides with another part
        bool selfCollision = true;
        // what the wizard distributed over the parts (informational, not applied at run time)
        float totalMass = 20.0f;

        // Runtime, not saved
        // the parts: Rigidbody entities in root's subtree, collected again when needed
        std::vector<Entity> parts;
        bool needCollectParts = true;
        // what the parts were collected from: the hierarchy and the Rigidbody count of then
        uint64_t collectedHierarchyRevision = 0;
        size_t collectedBodyCount = 0;
        // the state applied to the parts and the animator (-1: none yet)
        int appliedState = -1;
        bool appliedSelfCollision = true;
        // the pairs whose collision this ragdoll turned off (selfCollision false)
        std::vector<std::pair<Entity, Entity>> ignoredPairs;
        // the animator this ragdoll paused and whether it was enabled before
        Entity pausedAnimator = NULL_ENTITY;
        bool animatorWasEnabled = true;
        // the other bone drivers paused while simulated: entity animations (Model animations,
        // Animation actions), Pose Animators, Vertex Animators, to resume after
        enum class Driver : uint8_t { ACTION, POSE, VERTEX };
        std::vector<std::pair<Entity, Driver>> pausedDrivers;
        // impulses (point given) and forces asked for before the simulated state was applied
        struct Push{
            Vector3 value;
            Vector3 point;
            bool force = false;
        };
        std::vector<Push> pendingPushes;
    };

}

#endif //RAGDOLL_COMPONENT_H
