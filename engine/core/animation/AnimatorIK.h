// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md R11, D11)
// - Purpose: the IK solvers of the Animator's constraint stack, on a rig's local pose and its
//   rig-space cache: two-bone (law of cosines, pole), FABRIK, look-at (weighted, clamped), and the
//   helpers FootIK and FootLock build on (rotate a bone in rig space, move a bone, springs).
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

namespace jasperengine::anim{

    struct IKContext{
        Pose* local = nullptr;
        const int* parents = nullptr;
        RigSpace space;
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
    // brings the cache up to date for bones from `bone` on
    JASPERENGINE_API void refreshRigSpace(IKContext& context, int bone);

    // upper -> mid -> end (an ancestor chain, other bones between allowed): the end reaches
    // `target` (rig space; clamped to 0.999 of the chain's reach), the mid joint bends toward
    // `pole` (null: keeps its current bend plane). `endRotation` (optional) is the rig-space
    // rotation the end bone takes.
    JASPERENGINE_API void solveTwoBone(IKContext& context, int upper, int mid, int end, const Vector3& target,
                                       const Vector3* pole, const Quaternion* endRotation, float weight);

    // `chain` root first (2..16 bones, each an ancestor of the next): the last reaches `target`
    // within `tolerance` in at most `iterations` passes; an unreachable target straightens the
    // chain toward it. Twist is kept (shortest-arc rotations per joint).
    JASPERENGINE_API void solveFabrik(IKContext& context, const int* chain, int count, const Vector3& target,
                                      int iterations, float tolerance, float weight);

    // `bones` parents first (up to 4): together they turn so that the last bone's `forward` (its
    // local axis) points at `target`, each taking its share boneWeights[i] / sum; the last bone
    // never turns more than `clampDegrees` from where the animation had it
    JASPERENGINE_API void solveLookAt(IKContext& context, const int* bones, const float* boneWeights, int count, const Vector3& forward,
                                      const Vector3& target, float clampDegrees, float weight);

    // One exact step of a critically damped spring toward `target`: from rest it covers half the
    // distance in `halfLife` seconds; `velocity` is the spring's state (0 to start)
    JASPERENGINE_API float springDamp(float current, float target, float& velocity, float halfLife, float dt);

}

#endif //ANIMATOR_IK_H
