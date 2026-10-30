// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A skeleton plus its humanoid map, with the rest pose, the canonical character frame, the
// neutral standing pose, the scale, forward kinematics and the joint-angle conversions
// (spec: pose animation phase 1, sections 4.3, 4.4, 4.5 and 4.7).
//
// Decisions the spec leaves open:
// - Bones with a parent are transformed as parent * T * R * S (the engine's convention);
//   world rotations are the product of the local rotations (exact for uniform scale).
// - Non-slot bones and unmapped slots always keep their rest local transform; HumanPose
//   rotations of unmapped slots are ignored (identity in poses produced here).
// - The joint angles of a slot depend only on its own local rotation (the bones between a slot
//   and its effective parent slot keep their rest rotations), so a slot can be converted alone
//   (localFromAngles / anglesFromLocal).
// - A body pose whose swing (the vector of dof1 and dof2) is longer than 180 degrees describes
//   the same rotation as a shorter swing in the opposite direction: toBodyPose returns the
//   short one, so such poses do not round-trip literally.
// - fromBodyPose ignores BodyPose::offset.y. Its skin argument overrides the rig's skin for
//   grounding; nullptr uses the rig's skin (setSkin) and, without one, the sole estimate.
// - setSkin may be called before or after build; it recomputes the floor, the sole offsets,
//   the neutral hips height and humanScale (valid() turns false if the skin makes the rig
//   degenerate). A skin without samples falls back to the sole estimate.

#ifndef POSE_RIG_H
#define POSE_RIG_H

#include "Export.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "pose/HumanBones.h"
#include "pose/HumanoidMap.h"
#include "pose/PoseMath.h"
#include "pose/PoseSkin.h"
#include "pose/PoseTypes.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::pose {

    class JASPERENGINE_API PoseRig {
    public:
        // Validates (validateHumanoid) and computes every derived value; false with `error` set
        // when the map is invalid or the rig is degenerate (hips not above the floor)
        bool build(const Skeleton& skeleton, const HumanoidMap& humanoid, std::string* error);
        bool valid() const;

        bool has(HumanBone bone) const;
        int boneIndex(HumanBone bone) const;                // skeleton bone index, -1 when unmapped
        HumanBone effectiveParent(HumanBone bone) const;    // nearest present ancestor slot (Count for hips)
        int slotOfBone(int skeletonBone) const;             // slot index of a skeleton bone, -1 for non-slot bones

        const Skeleton& skeleton() const;
        const HumanoidMap& humanoid() const;

        Quaternion characterRotation() const;               // C (4.3): canonical axes -> model space
        Vector3 characterLeft() const;                      // C * L, C * U, C * F in model space
        Vector3 characterUp() const;
        Vector3 characterForward() const;
        float humanScale() const;                           // neutral hips height above the floor / 0.95
        float floorHeight() const;                          // model-space height of the rest pose's lowest point
        const HumanPose& restPose() const;
        const HumanPose& neutralPose() const;
        Vector3 neutralHipsWorldPosition() const;           // model space

        // FK: local slot rotations/hips position -> model-space world rotation/position of every
        // skeleton bone (indexed like skeleton().bones)
        void computeWorld(const HumanPose& pose, std::vector<Quaternion>& worldRot, std::vector<Vector3>& worldPos) const;
        void computeWorldMatrices(const HumanPose& pose, std::vector<Matrix4>& world) const;
        void computeWorldPrecise(const HumanPose& pose, std::vector<Quatd>& worldRot, std::vector<Affine3d>& world) const;

        // joint angles (4.5) and realisation
        BodyPose toBodyPose(const HumanPose& pose) const;   // offset from hips horizontal displacement vs neutral
        HumanPose fromBodyPose(const BodyPose& body, const PoseSkin* skin, bool grounded, float lift) const;   // grounded via 4.10
        // rotations of every slot from `body`, keeping pose.hipsPosition (no grounding)
        void applyBodyPoseRotations(const BodyPose& body, HumanPose& pose) const;
        // one slot: its three DOF values (degrees) <-> its local rotation
        Quaternion localFromAngles(HumanBone bone, const float angles[DOFS_PER_BONE]) const;
        void anglesFromLocal(HumanBone bone, const Quaternion& local, float angles[DOFS_PER_BONE]) const;
        // a slot's frame (local = left * E * neutral, E the joint-angle rotation): what the
        // Animator's humanoid retargeting maps local rotations between rigs with (S21 D12);
        // identity for an unmapped slot
        void slotFrame(HumanBone bone, Quaternion& left, Quaternion& neutral) const;

        void setSkin(std::shared_ptr<PoseSkin> skin);
        const PoseSkin* skin() const;                       // optional exact lowest point

        // Canonical-frame helpers (rotations only) used by IK, follow-through and the editor
        Quatd neutralWorldCanonical(HumanBone bone) const;  // N_b (4.4)
        // canonical world rotation of every present slot (identity for unmapped slots)
        void slotWorldCanonical(const HumanPose& pose, std::array<Quatd, HUMAN_BONE_COUNT>& world) const;
        // the local rotation that gives slot `bone` the canonical world rotation `world`, given
        // the canonical world rotation of its effective parent slot (ignored for the hips)
        Quatd localFromSlotWorld(HumanBone bone, const Quatd& parentSlotWorld, const Quatd& world) const;

        // hips local position <-> model space
        Vector3 hipsWorldPosition(const HumanPose& pose) const;
        void setHipsWorldPosition(HumanPose& pose, const Vector3& modelPosition) const;

        // Grounding support (4.10): sole offsets per Foot/Toes slot (0 for other or absent slots),
        // the lowest point of a pose (skin when given, else the sole estimate) and grounding
        float soleOffset(HumanBone bone) const;
        double lowestPointPrecise(const HumanPose& pose, const PoseSkin* skin) const;
        // moves the hips along up so the lowest point is floor + lift * humanScale
        void groundPose(HumanPose& pose, float lift, const PoseSkin* skin) const;
        // the same towards a model-space height `target`, from the pose's already measured
        // lowestPointPrecise(pose, skin) `lowest`; returns the lowest point it ends at
        double moveLowestTo(HumanPose& pose, double target, double lowest, const PoseSkin* skin) const;

    private:
        struct Slot {
            int bone = -1;                      // skeleton index
            HumanBone parent = HumanBone::Count;
            Quatd left;                         // local = left * E * neutral (4.5 with the rest chain folded in)
            Quatd neutral;                      // N_b, canonical
        };

        bool computeGrounding(std::string* error);
        void fkPrecise(const HumanPose& pose, std::vector<Quatd>& rot, std::vector<Affine3d>& world) const;
        Quatd boneE(HumanBone bone, const Quatd& local) const;

        bool built = false;
        Skeleton skel;
        HumanoidMap map;
        std::vector<int> order;                 // parents before children
        std::vector<int> boneSlot;              // skeleton index -> slot or -1
        std::array<Slot, HUMAN_BONE_COUNT> slots;
        Quatd character;                        // C
        Affine3d hipsParent;                    // model transform of the hips' parent (identity for a root hips)
        HumanPose rest;
        HumanPose neutral;
        Vec3d neutralHips;                      // model space
        std::array<double, HUMAN_BONE_COUNT> soles{};
        double floor = 0.0;
        double scale = 1.0;
        std::shared_ptr<PoseSkin> skinPtr;
    };

    // Mirror (4.5): left/right triples swapped, centre U/F-axis DOFs negated, offset.x negated
    JASPERENGINE_API BodyPose mirrorBodyPose(const BodyPose& body);

    // A slot's joint-angle rotation E (4.5) from its three DOF values in degrees (the swing about
    // the two DOF axes, then the twist), and back (swing-twist about the neutral direction, twist
    // wrapped to (-180, 180]). Rig-independent: PoseRig's local rotations are left * E * neutral.
    JASPERENGINE_API Quatd jointRotation(HumanBone bone, const float angles[DOFS_PER_BONE]);
    JASPERENGINE_API void jointAngles(HumanBone bone, const Quatd& rotation, float angles[DOFS_PER_BONE]);

    // the longest swing |(dof1, dof2)| clampToLimits keeps (spec 4.5): below 180, where the twist
    // is undefined
    constexpr float MAX_SWING_DEGREES = 179.5f;

    // Clamps every DOF to the 4.5 joint limits, then caps each swing |(dof1, dof2)| at
    // MAX_SWING_DEGREES by scaling the pair; returns the indices of the changed DOFs (ascending)
    JASPERENGINE_API std::vector<int> clampToLimits(BodyPose& body);
    // the same for one slot's three values; returns a bit mask of the changed DOFs (bit k = dof k)
    JASPERENGINE_API unsigned clampBoneToLimits(HumanBone bone, float angles[DOFS_PER_BONE]);
    // every value within its limits and the swing at most MAX_SWING_DEGREES long: exactly the
    // values clampBoneToLimits leaves unchanged (and every result of clampBoneToLimits passes)
    JASPERENGINE_API bool boneWithinLimits(HumanBone bone, const float angles[DOFS_PER_BONE]);

}

#endif // POSE_RIG_H
