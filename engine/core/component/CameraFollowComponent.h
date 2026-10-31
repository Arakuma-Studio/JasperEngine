// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (camera shake spec 5.9, 7.1, 7.3 step 1)
// - Purpose: Camera Follow, a Cinemachine-lite follower: keeps its GameObject (usually a camera)
//   at `offset` from `target` with Unity's SmoothDamp per world axis, optionally aiming at the
//   target, with a 2D dead zone and world bounds.
// - CameraControlSystem runs it each frame after the scene's scripts, animations and physics
//   interpolation and before the shakers. While enabled it owns its GameObject's Transform: a
//   script that also moves it is overwritten (a warning says so once, `foreignWriteDetected`).
// - Only the authored fields are serialized, cloned and emitted on export; the runtime fields
//   below the marker are not (a clone snaps on its first update).

#ifndef CAMERA_FOLLOW_COMPONENT_H
#define CAMERA_FOLLOW_COMPONENT_H

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Vector2.h"
#include "math/Vector3.h"

#include <cstdint>

namespace jasperengine{

    enum class FollowOffsetSpace{
        // the offset is in world axes
        World,
        // the offset turns with the target's rotation
        Target,
        // the offset turns with the target's heading only (its rotation about world Y)
        TargetYaw
    };

    struct JASPERENGINE_API CameraFollowComponent{
        bool enabled = true;
        // the GameObject followed (same scene); nothing happens without one
        Entity target = NULL_ENTITY;
        Vector3 offset = Vector3(0, 2, -6);
        FollowOffsetSpace offsetSpace = FollowOffsetSpace::World;
        // SmoothDamp smoothTime per world axis, seconds; 0 = rigid
        Vector3 damping = Vector3(0.2f, 0.2f, 0.2f);
        // SmoothDamp maxSpeed, units per second; 0 = unlimited
        float maxSpeed = 0.0f;
        // aim at the target (+ lookAtOffset, world)
        bool lookAt = true;
        Vector3 lookAtOffset = Vector3(0, 1, 0);
        // seconds to cover 99% of a turn; 0 = rigid
        float rotationDamping = 0.1f;
        // width / height (world x / y) where target moves don't move the follower; orthographic
        // and UI cameras only (ignored in 3D perspective)
        Vector2 deadZone = Vector2(0, 0);
        // the follower's position is clamped to [boundsMin, boundsMax] (world)
        bool useBounds = false;
        Vector3 boundsMin = Vector3(-10, -10, -10);
        Vector3 boundsMax = Vector3(10, 10, 10);
        bool unscaledTime = false;

        // ---- runtime (not serialized) ----
        Vector3 velocity = Vector3(0, 0, 0);        // SmoothDamp state per axis
        Vector3 desired = Vector3(0, 0, 0);         // the position aimed at this frame (world)
        Vector3 writtenPosition = Vector3(0, 0, 0); // the local values written last frame
        Quaternion writtenRotation;
        Vector3 writtenTarget = Vector3(0, 0, 0);   // CameraComponent::target written (useTarget)
        Entity followedTarget = NULL_ENTITY;        // the target placed for (a change snaps)
        bool placed = false;                        // false: the next update places without damping
        bool foreignWriteDetected = false;          // something else moved the Transform
        bool warnedForeignWrite = false;
        bool warnedMissingTarget = false;
    };

}

#endif //CAMERA_FOLLOW_COMPONENT_H
