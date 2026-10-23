// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Analytic two-bone IK on the limbs, moving the hips with planted feet and grounding (spec: pose
// animation phase 1, section 4.10). Positions are model space.
//
// Decisions the spec leaves open:
// - The reach clamp margin is 1e-4 * humanScale (the spec's 1e-4 in humanScale units, so it
//   does not depend on the model's unit).
// - A target at the upper bone's position keeps the limb's current aim (only the bend changes).
// - Bend side (spec 4.10, near-straight rule): with sinA the sine of the angle at the upper
//   joint, the bend plane normal blends from the pole's (sinA <= 0.02, ~1.1 degrees) to the
//   limb's current one (sinA >= 0.1, ~5.7 degrees) by smoothstep(0.02, 0.1, sinA) *
//   smoothstep(-1, 0, cos(angle between the current and the pole plane)): the current plane loses
//   its weight as it turns past 90 degrees from the pole plane, so a limb bent slightly away from
//   the pole side (float noise of a straight rest or neutral limb) takes the pole's side without
//   any jump. Above sinA = 0.1 the limb keeps its own plane on either side (the one documented
//   discontinuity: a limb bent backwards by ~5.7 degrees).
// - The solve: the chosen plane normal is carried by the shortest rotation from C - A to T - A
//   (like fromTo(C - A, T - A) of the spec), the target triangle is built in that plane (C* on the
//   target line at the clamped reach, B* on the normal's side), then A is turned onto it
//   (aligning B - A and the plane normal) and B onto C* by the shortest arc.

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
