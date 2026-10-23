// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Analytic two-bone IK on the limbs, moving the hips with planted feet and grounding (spec: pose
// animation phase 1, section 4.10). Positions are model space.
//
// Decisions the spec leaves open:
// - The reach clamp margin is 1e-4 * humanScale (the spec's 1e-4 in humanScale units, so it
//   does not depend on the model's unit).
// - A target at the upper bone's position keeps the limb's current aim (only the bend changes).

#ifndef POSE_IK_H
#define POSE_IK_H

#include "Export.h"
#include "math/Vector3.h"
#include "pose/PoseRig.h"
#include "pose/PoseTypes.h"

#include <cstdint>

namespace jasperengine::pose {

    enum class Limb : uint8_t { LeftArm, RightArm, LeftLeg, RightLeg };

    // upper, lower and end slots of a limb
    JASPERENGINE_API void limbBones(Limb limb, HumanBone& upper, HumanBone& lower, HumanBone& end);

    // Moves the limb's end bone to `targetModel` (clamped to the limb's reach). The end bone keeps
    // its world rotation when `keepEndWorldRotation` (feet), else its local rotation (hands).
    // false when the rig is not built or the limb has zero-length bones.
    JASPERENGINE_API bool solveLimbIK(const PoseRig& rig, HumanPose& pose, Limb limb, const Vector3& targetModel, bool keepEndWorldRotation);

    // Moves the hips to `newHipsWorldModel` and re-solves both legs so the feet keep their world
    // position and rotation
    JASPERENGINE_API void moveHipsKeepFeet(const PoseRig& rig, HumanPose& pose, const Vector3& newHipsWorldModel);

    // model-space height along up of the lowest point: the rig's skin if set, else the sole estimate
    JASPERENGINE_API float lowestPoint(const PoseRig& rig, const HumanPose& pose);

    // moves the hips along up so the lowest point is floor + lift * humanScale (lift in humanScale units)
    JASPERENGINE_API void ground(const PoseRig& rig, HumanPose& pose, float lift);

}

#endif // POSE_IK_H
