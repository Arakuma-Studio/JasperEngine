// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md R11, D11)
// - Purpose: the IK solvers of the Animator's constraint stack, on a rig's local pose and its
//   rig-space cache: two-bone (law of cosines, pole), FABRIK, look-at (weighted, clamped), and the
//   helpers FootIK and FootLock build on (rotate a bone in rig space, move a bone, the ankle's
//   height on a slope, springs). S77 R32-R34: two-bone soften, hinge axis and twist, the look-at
//   chain walk with an aim offset, and an IKResult from every solve.
// - Every solver changes local rotations (local = parentRigRotation^-1 * rigRotation) and brings
//   the rig-space cache up to date again for the bones after the first one it changed (parents
//   first). Scale: positions come from the full affine (non-uniform scale included), rotations
//   follow the engine's convention (rig rotation = parent * local).
// - Weights blend the solved local rotations with the incoming ones (nlerp); weight 0 changes
//   nothing.
// - Thread-safety: pure functions over the caller's buffers; allocation-free.

#ifndef ANIMATOR_IK_H
#define ANIMATOR_IK_H

#include "Export.h"
#include "animation/AnimatorPose.h"
#include "math/Matrix4.h"

namespace jasperengine::anim{

    struct IKContext{
        Pose* local = nullptr;
        const int* parents = nullptr;
        RigSpace space;
        // rig space to world: what IKResult measures in (null: rig space is world space)
        const Matrix4* rigToWorld = nullptr;
    };

    // How far from its target a chain's end may stay and still count as reached, in metres; the
    // aim error a look-at may keep, in degrees (S77 R33)
    constexpr float kIKReachTolerance = 0.001f;
    constexpr float kIKAimTolerance = 0.1f;

    // What a solve achieved, measured on the final pose, after the weight: a target beyond the
    // chain, one soft-limited out of reach, a clamped look-at or a partial weight are not reached.
    // Weight 0 (nothing solved) gives the default.
    struct IKResult{
        bool reached = false;
        float error = 0.0f;     // metres: the end to the target; a look-at: the target to its aim ray
    };

    // The optional shape of a two-bone solve; the defaults are the plain solve
    struct TwoBoneOptions{
        // the part of the reach (0..1, from the far end) over which the target's distance eases
        // toward full extension, so that the mid joint keeps moving instead of stopping at the
        // reach. 0: a hard limit at 0.999 of the reach.
        float soften = 0.0f;
        // the mid joint's local axis it only bends about: a positive turn opens the joint, so the
        // sign picks the bend side, and that side stays defined on a straight chain. Zero: bends
        // through the shortest arcs toward the pole or the current bend. Along a bone: as zero.
        // Under non-uniform parent scale only the side is kept: the joint may also twist.
        Vector3 hingeAxis = Vector3(0, 0, 0);
        float twist = 0.0f;     // degrees about the line from the upper joint to the target
    };

    // the rotation taking direction `from` to direction `to` by the shortest arc (identity for a
    // zero vector; a half turn about a perpendicular axis for opposite ones)
    JASPERENGINE_API Quaternion shortestArc(const Vector3& from, const Vector3& to);
    // rotates `bone` by `rotation` in rig space (about its own position) and updates the cache
    JASPERENGINE_API void rotateBoneInRigSpace(IKContext& context, int bone, const Quaternion& rotation);
    // turns `bone` about its own position so that `point` (a rig-space point it carries, such as
    // its child joint) moves onto the ray toward `target`. The turn is found in the parent's
    // local space, so the direction is exact under non-uniform parent scale.
    JASPERENGINE_API void aimBoneInRigSpace(IKContext& context, int bone, const Vector3& point, const Vector3& target);
    // moves `bone` by `delta` (rig space) and updates the cache
    JASPERENGINE_API void translateBoneInRigSpace(IKContext& context, int bone, const Vector3& delta);
    // brings the cache up to date for `bone` and its descendants (all bones for bone <= 0)
    JASPERENGINE_API void refreshRigSpace(IKContext& context, int bone);

    // upper -> mid -> end (an ancestor chain, other bones between allowed): the end reaches
    // `target` (rig space; clamped to 0.999 of the chain's reach), the mid joint bends toward
    // `pole` (null: keeps its current bend plane). `endRotation` (optional) is the rig-space
    // rotation the end bone takes.
    JASPERENGINE_API IKResult solveTwoBone(IKContext& context, int upper, int mid, int end, const Vector3& target,
                                           const Vector3* pole, const Quaternion* endRotation, float weight,
                                           const TwoBoneOptions& options = TwoBoneOptions());

    // `chain` root first (2..16 bones, each an ancestor of the next): the last reaches `target`
    // within `tolerance` in at most `iterations` passes; an unreachable target straightens the
    // chain toward it. Twist is kept (shortest-arc rotations per joint).
    JASPERENGINE_API IKResult solveFabrik(IKContext& context, const int* chain, int count, const Vector3& target,
                                          int iterations, float tolerance, float weight);

    // `bones` parents first (up to 4): walking from the first, each bone re-aims from where the
    // ones before it left the chain and takes boneWeights[i] / sum of what is left, the last with
    // a weight takes all of it, so the aim ray ends on `target` however far the parents carry the
    // head. The ray starts at `aimOffset` (the last bone's local space, the eyes) and runs along
    // `forward` (its local axis). The last bone never turns more than `clampDegrees` from where
    // the animation had it. A target closer to the bones than the offset allows stays missed.
    JASPERENGINE_API IKResult solveLookAt(IKContext& context, const int* bones, const float* boneWeights, int count, const Vector3& forward,
                                          const Vector3& target, float clampDegrees, float weight,
                                          const Vector3& aimOffset = Vector3(0, 0, 0));

    // The ankle's height above the ground under it that keeps the centre of the sole on a slope
    // (`normal`, rig space, rig up is +y): the foot stands `footHeight` from the plane along its
    // own up, turned toward the normal by at most `maxAngleDegrees`. The height stops growing
    // past the steeper of 60 degrees and that clamp: no foot stands on a wall.
    JASPERENGINE_API float footAnkleHeight(float footHeight, const Vector3& normal, float maxAngleDegrees);

    // One exact step of a critically damped spring toward `target`: from rest it covers half the
    // distance in `halfLife` seconds; `velocity` is the spring's state (0 to start)
    JASPERENGINE_API float springDamp(float current, float target, float& velocity, float halfLife, float dt);

}

#endif //ANIMATOR_IK_H
