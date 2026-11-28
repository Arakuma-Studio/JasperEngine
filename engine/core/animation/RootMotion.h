// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (S94 F32-F34): the root transform math the Animator and the Animation Player share.
// - A clip's root_motion block (ClipRootMotion) names a root bone, which components of its motion
//   stay in the pose (Bake Into Pose), what the root follows (Based Upon) and offsets.
// - Every function works in rig space: the player's (or Animator's) entity is the origin, +y up,
//   headings are radians about +y with 0 along +z.
// - Based Upon Body Orientation, Center Of Mass and Feet read a humanoid body (HumanBodyPoints);
//   without one they act as Original (the caller warns once).
// - Pure functions, no allocation: safe on the Animator's worker threads.

#ifndef ROOT_MOTION_H
#define ROOT_MOTION_H

#include "Export.h"
#include "animation/AnimatorAssetIO.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "pose/HumanBones.h"

#include <array>
#include <bitset>

namespace jasperengine::anim{

    // rig-space positions of the humanoid slots one pose has
    struct JASPERENGINE_API HumanBodyPoints{
        std::array<Vector3, pose::HUMAN_BONE_COUNT> at{};
        std::bitset<pose::HUMAN_BONE_COUNT> has;

        void set(pose::HumanBone bone, const Vector3& position){
            at[(size_t)bone] = position;
            has.set((size_t)bone);
        }
        // hips and both upper legs: enough for a heading, a centre of mass and feet
        bool usable() const;
    };

    // the root's frame of one pose: the point it follows and its heading
    struct JASPERENGINE_API RootBasisSample{
        Vector3 point = Vector3(0, 0, 0);
        float heading = 0.0f;
    };

    // what a clip's root motion keeps fixed, sampled once per clip (at time 0 and at rest)
    struct JASPERENGINE_API RootMotionFrame{
        Vector3 startPoint = Vector3(0, 0, 0);  // the basis point at time 0 (the pivot)
        float startHeading = 0.0f;
        float turn = 0.0f;                      // radians the pose (and its motion) turns: Body Orientation + Offset
        Vector3 shift = Vector3(0, 0, 0);       // the pose's translation: Feet / Center Of Mass at start + Y Offset
    };

    // the root motion needs a body (Body Orientation, Center Of Mass or Feet)
    JASPERENGINE_API bool rootMotionNeedsBody(const ClipRootMotion& rootMotion);

    JASPERENGINE_API float rootWrapAngle(float radians);
    JASPERENGINE_API float rootYawOf(const Quaternion& rotation);
    JASPERENGINE_API Vector3 rootRotateYaw(const Vector3& v, float yaw);
    JASPERENGINE_API Quaternion rootYawRotation(float yaw);

    // the body's heading (from the hips and shoulders line), its centre of mass (Unity-like mass
    // fractions over the mapped slots) and the lower foot's height
    JASPERENGINE_API float bodyHeading(const HumanBodyPoints& body);
    JASPERENGINE_API Vector3 bodyCenterOfMass(const HumanBodyPoints& body);
    JASPERENGINE_API float bodyFeetHeight(const HumanBodyPoints& body);

    // the basis of one pose from its root bone (rig space) and, when usable, its body
    JASPERENGINE_API RootBasisSample rootBasis(const ClipRootMotion& rootMotion, const Vector3& rootPosition,
                                               const Quaternion& rootRotation, const HumanBodyPoints* body);
    // the frame from the basis at time 0 (and the bodies at time 0 and at rest, null when none)
    JASPERENGINE_API RootMotionFrame rootMotionFrame(const ClipRootMotion& rootMotion, const RootBasisSample& start,
                                                     const HumanBodyPoints* startBody, const HumanBodyPoints* restBody);
    // the motion point P(t) (baked components held at the start) and h(t), the heading change
    // since time 0 (0 when the rotation is baked)
    JASPERENGINE_API void rootMotionPoint(const ClipRootMotion& rootMotion, const RootMotionFrame& frame, const RootBasisSample& now,
                                          Vector3& point, float& heading);
    // removes the unbaked motion from the root bone's rig-space transform (the basis `now` was
    // read from that pose), then turns and shifts it by the frame's start correction
    JASPERENGINE_API void removeRootMotion(const ClipRootMotion& rootMotion, const RootMotionFrame& frame, const RootBasisSample& now,
                                           Vector3& rootPosition, Quaternion& rootRotation);

}

#endif //ROOT_MOTION_H
