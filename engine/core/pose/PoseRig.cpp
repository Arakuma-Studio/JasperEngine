// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/PoseRig.h"

#include "pose/PoseDofs.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jasperengine::pose {

    namespace {

        using B = HumanBone;

        // the slot index (PoseRig::boneIndex, the skeleton index, hides pose::boneIndex in members)
        int slotIndex(HumanBone bone){ return static_cast<int>(bone); }

        Quatd fromColumns(const Vec3d& c0, const Vec3d& c1, const Vec3d& c2){
            const double m00 = c0.x, m10 = c0.y, m20 = c0.z;
            const double m01 = c1.x, m11 = c1.y, m21 = c1.z;
            const double m02 = c2.x, m12 = c2.y, m22 = c2.z;
            const double trace = m00 + m11 + m22;
            Quatd q;
            if (trace > 0.0){
                const double s = std::sqrt(trace + 1.0) * 2.0;
                q = Quatd(0.25 * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s);
            }else if (m00 > m11 && m00 > m22){
                const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
                q = Quatd((m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s);
            }else if (m11 > m22){
                const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
                q = Quatd((m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s);
            }else{
                const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
                q = Quatd((m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s);
            }
            return q.normalized();
        }

        // aim children of the aimed bones (4.4); empty for carried bones
        std::vector<HumanBone> aimChildren(HumanBone bone){
            switch (bone){
                case B::Hips: return {B::Spine};
                case B::Spine: return {B::Chest, B::UpperChest, B::Neck, B::Head};
                case B::Chest: return {B::UpperChest, B::Neck, B::Head};
                case B::UpperChest: return {B::Neck, B::Head};
                case B::Neck: return {B::Head};
                case B::LeftShoulder: return {B::LeftUpperArm};
                case B::RightShoulder: return {B::RightUpperArm};
                case B::LeftUpperArm: return {B::LeftLowerArm};
                case B::RightUpperArm: return {B::RightLowerArm};
                case B::LeftLowerArm: return {B::LeftHand};
                case B::RightLowerArm: return {B::RightHand};
                case B::LeftUpperLeg: return {B::LeftLowerLeg};
                case B::RightUpperLeg: return {B::RightLowerLeg};
                case B::LeftLowerLeg: return {B::LeftFoot};
                case B::RightLowerLeg: return {B::RightFoot};
                default: return {};
            }
        }

        const HumanBone SOLE_BONES[] = {B::LeftFoot, B::LeftToes, B::RightFoot, B::RightToes};

        // wraps degrees to (-180, 180]
        double wrapDegrees(double degrees){
            double d = std::fmod(degrees, 360.0);
            if (d <= -180.0) d += 360.0;
            if (d > 180.0) d -= 360.0;
            return d;
        }

    }

    bool PoseRig::build(const Skeleton& skeleton, const HumanoidMap& humanoid, std::string* error,
                        const std::array<float, HUMAN_BONE_COUNT>* twistDegrees){
        built = false;
        twists.fill(0.0f);
        if (twistDegrees){
            for (int s = 0; s < HUMAN_BONE_COUNT; s++){
                const float value = (*twistDegrees)[(size_t)s];
                twists[(size_t)s] = std::isfinite(value) ? std::min(std::max(value, -180.0f), 180.0f) : 0.0f;
            }
        }
        std::string validation;
        if (!validateHumanoid(skeleton, humanoid, &validation)){
            if (error) *error = validation;
            return false;
        }
        skel = skeleton;
        map = humanoid;
        const int count = static_cast<int>(skel.bones.size());

        // parents before children
        std::vector<int> depth(count, 0);
        for (int i = 0; i < count; i++){
            for (int b = skel.bones[i].parent; b >= 0; b = skel.bones[b].parent){
                depth[i]++;
            }
        }
        order.resize(count);
        for (int i = 0; i < count; i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](int a, int b){ return depth[a] < depth[b]; });

        boneSlot.assign(count, -1);
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            Slot& slot = slots[s];
            slot = Slot();
            slot.bone = findSkeletonBone(skel, map.bones[s]);
            if (slot.bone >= 0){
                boneSlot[slot.bone] = s;
                slot.parent = effectiveParentSlot(map, boneAt(s));
            }
        }

        // rest pose
        rest = HumanPose();
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            rest.rotations[s] = slots[s].bone >= 0 ? Quatd(skel.bones[slots[s].bone].rotation).normalized().toQuaternion() : Quaternion();
        }
        rest.hipsPosition = skel.bones[slots[0].bone].position;

        std::vector<Quatd> restRot;
        std::vector<Affine3d> restWorld;
        fkPrecise(rest, restRot, restWorld);

        // canonical character frame (4.3)
        const Vec3d up(0, 1, 0);
        Vec3d across = restWorld[slots[slotIndex(B::LeftUpperLeg)].bone].t - restWorld[slots[slotIndex(B::RightUpperLeg)].bone].t;
        across = across - up * across.dot(up);
        if (across.length() < 1e-9){
            if (error) *error = "the upper legs are not apart horizontally: cannot find the character's left";
            return false;
        }
        const Vec3d leftM = across.normalized();
        const Vec3d forwardM = leftM.cross(up);
        character = fromColumns(leftM, up, forwardM);
        const Quatd characterInv = character.inverse();
        restCanonical.resize(count);
        for (int i = 0; i < count; i++){
            restCanonical[i] = (characterInv * restRot[i]).normalized();
        }

        // hips parent transform
        const int hipsBone = slots[0].bone;
        const int hipsParentBone = skel.bones[hipsBone].parent;
        hipsParent = hipsParentBone >= 0 ? restWorld[hipsParentBone] : Affine3d();
        const Quatd hipsParentRot = hipsParentBone >= 0 ? restRot[hipsParentBone] : Quatd();
        {
            const Affine3d& h = hipsParent;
            const double det = h.m[0][0] * (h.m[1][1] * h.m[2][2] - h.m[1][2] * h.m[2][1])
                             - h.m[0][1] * (h.m[1][0] * h.m[2][2] - h.m[1][2] * h.m[2][0])
                             + h.m[0][2] * (h.m[1][0] * h.m[2][1] - h.m[1][1] * h.m[2][0]);
            double largest = 0.0;
            for (int r = 0; r < 3; r++){
                for (int c = 0; c < 3; c++){
                    largest = std::max(largest, std::fabs(h.m[r][c]));
                }
            }
            if (!(largest > 0.0) || std::fabs(det) < 1e-9 * largest * largest * largest){
                if (error) *error = "the hips' parent transform is degenerate (zero scale)";
                return false;
            }
        }

        // rest canonical world rotations / positions of the slots
        std::array<Quatd, HUMAN_BONE_COUNT> restCanon;
        std::array<Vec3d, HUMAN_BONE_COUNT> restPos;
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            if (slots[s].bone >= 0){
                restCanon[s] = (characterInv * restRot[slots[s].bone]).normalized();
                restPos[s] = characterInv * restWorld[slots[s].bone].t;
            }
        }

        // neutral pose (4.4), slots in table order (parents first)
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            Slot& slot = slots[s];
            if (slot.bone < 0){
                continue;
            }
            const HumanBone bone = boneAt(s);
            Quatd provisional;
            if (s == 0){
                provisional = restCanon[0];
                slot.left = (characterInv * hipsParentRot).normalized().inverse();   // H^-1
            }else{
                const int p = slotIndex(slot.parent);
                const Quatd restChain = restCanon[p].inverse() * restCanon[s] * Quatd(skel.bones[slot.bone].rotation).normalized().inverse();   // Q_b
                provisional = (slots[p].neutral * restCanon[p].inverse() * restCanon[s]).normalized();
                slot.left = (restChain.inverse() * slots[p].neutral.inverse()).normalized();
            }
            slot.neutral = provisional;
            for (HumanBone child : aimChildren(bone)){
                const int c = slotIndex(child);
                if (slots[c].bone < 0){
                    continue;
                }
                const Vec3d aimLocal = restCanon[s].inverse() * (restPos[c] - restPos[s]);
                const Vec3d v = provisional * aimLocal;
                if (v.length() > 1e-12){
                    slot.neutral = (fromToRotation(v, Vec3d(boneDofInfo(bone).neutralDirection)) * provisional).normalized();
                }
                break;
            }
            // the twist correction about the neutral direction (before the children use it)
            if (twists[(size_t)s] != 0.0f){
                slot.neutral = (axisAngleRad(Vec3d(boneDofInfo(bone).neutralDirection), degToRad(twists[(size_t)s])) * slot.neutral).normalized();
            }
        }

        neutral = HumanPose();
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            if (slots[s].bone >= 0){
                neutral.rotations[s] = (slots[s].left * slots[s].neutral).normalized().toQuaternion();
            }
        }
        neutral.hipsPosition = rest.hipsPosition;

        built = true;
        if (!computeGrounding(error)){
            built = false;
            return false;
        }
        return true;
    }

    bool PoseRig::computeGrounding(std::string* error){
        // floor and sole offsets from the rest pose
        std::vector<Quatd> rot;
        std::vector<Affine3d> world;
        fkPrecise(rest, rot, world);
        double restLowest = std::numeric_limits<double>::infinity();
        if (skinPtr){
            restLowest = skinPtr->lowest(world);
        }
        if (!std::isfinite(restLowest)){
            for (HumanBone b : SOLE_BONES){
                if (has(b)){
                    restLowest = std::min(restLowest, world[slots[slotIndex(b)].bone].t.y);
                }
            }
        }
        floor = restLowest;
        soles.fill(0.0);
        for (HumanBone b : SOLE_BONES){
            if (has(b)){
                soles[slotIndex(b)] = world[slots[slotIndex(b)].bone].t.y - restLowest;
            }
        }

        // neutral hips: the rest hips position grounded to the rest floor
        neutral.hipsPosition = rest.hipsPosition;
        scale = 1.0;
        groundPose(neutral, 0.0f, skinPtr.get());
        neutralHips = hipsParent.apply(Vec3d(neutral.hipsPosition));
        const double height = neutralHips.y - floor;
        if (!(height > 1e-9)){
            if (error) *error = "the hips are not above the floor in the neutral pose";
            return false;
        }
        scale = height / 0.95;
        return true;
    }

    bool PoseRig::valid() const{ return built; }

    float PoseRig::twist(HumanBone bone) const{
        return bone == HumanBone::Count ? 0.0f : twists[(size_t)slotIndex(bone)];
    }

    bool PoseRig::has(HumanBone bone) const{
        return built && bone != HumanBone::Count && slots[slotIndex(bone)].bone >= 0;
    }

    int PoseRig::boneIndex(HumanBone bone) const{
        return built && bone != HumanBone::Count ? slots[slotIndex(bone)].bone : -1;
    }

    HumanBone PoseRig::effectiveParent(HumanBone bone) const{
        return has(bone) ? slots[slotIndex(bone)].parent : effectiveParentSlot(map, bone);
    }

    int PoseRig::slotOfBone(int skeletonBone) const{
        return skeletonBone >= 0 && static_cast<size_t>(skeletonBone) < boneSlot.size() ? boneSlot[skeletonBone] : -1;
    }

    const Skeleton& PoseRig::skeleton() const{ return skel; }
    const HumanoidMap& PoseRig::humanoid() const{ return map; }
    Quaternion PoseRig::characterRotation() const{ return character.toQuaternion(); }
    Vector3 PoseRig::characterLeft() const{ return (character * Vec3d(1, 0, 0)).toVector3(); }
    Vector3 PoseRig::characterUp() const{ return (character * Vec3d(0, 1, 0)).toVector3(); }
    Vector3 PoseRig::characterForward() const{ return (character * Vec3d(0, 0, 1)).toVector3(); }
    float PoseRig::humanScale() const{ return static_cast<float>(scale); }
    float PoseRig::floorHeight() const{ return static_cast<float>(floor); }
    const HumanPose& PoseRig::restPose() const{ return rest; }
    const HumanPose& PoseRig::neutralPose() const{ return neutral; }
    Vector3 PoseRig::neutralHipsWorldPosition() const{ return neutralHips.toVector3(); }

    void PoseRig::fkPrecise(const HumanPose& pose, std::vector<Quatd>& rot, std::vector<Affine3d>& world) const{
        const size_t count = skel.bones.size();
        rot.resize(count);
        world.resize(count);
        for (int i : order){
            const SkeletonBone& bone = skel.bones[i];
            const int slot = boneSlot[i];
            const Quatd localRot = slot >= 0 ? Quatd(pose.rotations[slot]).normalized() : Quatd(bone.rotation).normalized();
            const Vec3d localPos = slot == 0 ? Vec3d(pose.hipsPosition) : Vec3d(bone.position);
            const Affine3d local = Affine3d::fromTRS(localPos, localRot, Vec3d(bone.scale));
            if (bone.parent >= 0){
                rot[i] = rot[bone.parent] * localRot;
                world[i] = world[bone.parent] * local;
            }else{
                rot[i] = localRot;
                world[i] = local;
            }
        }
    }

    void PoseRig::computeWorldPrecise(const HumanPose& pose, std::vector<Quatd>& worldRot, std::vector<Affine3d>& world) const{
        if (!built){
            worldRot.clear();
            world.clear();
            return;
        }
        fkPrecise(pose, worldRot, world);
    }

    void PoseRig::computeWorld(const HumanPose& pose, std::vector<Quaternion>& worldRot, std::vector<Vector3>& worldPos) const{
        std::vector<Quatd> rot;
        std::vector<Affine3d> world;
        computeWorldPrecise(pose, rot, world);
        worldRot.resize(rot.size());
        worldPos.resize(rot.size());
        for (size_t i = 0; i < rot.size(); i++){
            worldRot[i] = rot[i].normalized().toQuaternion();
            worldPos[i] = world[i].t.toVector3();
        }
    }

    void PoseRig::computeWorldMatrices(const HumanPose& pose, std::vector<Matrix4>& matrices) const{
        std::vector<Quatd> rot;
        std::vector<Affine3d> world;
        computeWorldPrecise(pose, rot, world);
        matrices.resize(world.size());
        for (size_t i = 0; i < world.size(); i++){
            Matrix4 m;
            for (int r = 0; r < 3; r++){
                for (int c = 0; c < 3; c++){
                    m.set(c, r, static_cast<float>(world[i].m[r][c]));
                }
                m.set(3, r, static_cast<float>(r == 0 ? world[i].t.x : (r == 1 ? world[i].t.y : world[i].t.z)));
                m.set(r, 3, 0.0f);
            }
            m.set(3, 3, 1.0f);
            matrices[i] = m;
        }
    }

    Quatd PoseRig::boneE(HumanBone bone, const Quatd& local) const{
        const Slot& slot = slots[slotIndex(bone)];
        return slot.left.inverse() * local * slot.neutral.inverse();
    }

    Quatd PoseRig::jointRotationFromLocal(HumanBone bone, const Quatd& local) const{
        if (!has(bone)){
            return Quatd();
        }
        return boneE(bone, local.normalized()).normalized();
    }

    Quatd PoseRig::localFromJointRotationPrecise(HumanBone bone, const Quatd& rotation) const{
        if (!has(bone)){
            return Quatd();
        }
        const Slot& slot = slots[slotIndex(bone)];
        return (slot.left * rotation * slot.neutral).normalized();
    }

    Quaternion PoseRig::localFromJointRotation(HumanBone bone, const Quatd& rotation) const{
        if (!has(bone)){
            return Quaternion();
        }
        return localFromJointRotationPrecise(bone, rotation).toQuaternion();
    }

    Quatd PoseRig::restWorldCanonical(int skeletonBone) const{
        if (!built || skeletonBone < 0 || (size_t)skeletonBone >= restCanonical.size()){
            return Quatd();
        }
        return restCanonical[(size_t)skeletonBone];
    }

    void PoseRig::anglesFromLocal(HumanBone bone, const Quaternion& local, float angles[DOFS_PER_BONE]) const{
        for (int k = 0; k < DOFS_PER_BONE; k++) angles[k] = 0.0f;
        if (!has(bone)){
            return;
        }
        jointAngles(bone, boneE(bone, Quatd(local).normalized()).normalized(), angles);
    }

    Quatd jointRotation(HumanBone bone, const float angles[DOFS_PER_BONE]){
        const BoneDofInfo& info = boneDofInfo(bone);
        Vec3d s;
        for (int k = 0; k < 2; k++){
            const DofInfo& dof = info.dofs[k];
            s += Vec3d(canonicalAxis(dof.axis)) * (degToRad(angles[k]) * dof.sign);
        }
        const Quatd swing = fromRotationVector(s);
        const Quatd twist = axisAngleRad(Vec3d(canonicalAxis(info.dofs[2].axis)), degToRad(angles[2]) * info.dofs[2].sign);
        return (swing * twist).normalized();
    }

    void jointAngles(HumanBone bone, const Quatd& rotation, float angles[DOFS_PER_BONE]){
        const BoneDofInfo& info = boneDofInfo(bone);
        Quatd swing, twist;
        swingTwist(rotation.normalized(), Vec3d(info.neutralDirection), swing, twist);
        if (swing.w < 0.0){
            swing = -swing;
            twist = -twist;
        }
        const Vec3d s = toRotationVector(swing);
        const Vec3d axis3(canonicalAxis(info.dofs[2].axis));
        const double t = 2.0 * std::atan2(twist.vec().dot(axis3), twist.w);
        for (int k = 0; k < 2; k++){
            const DofInfo& dof = info.dofs[k];
            angles[k] = static_cast<float>(dof.sign * radToDeg(s.dot(Vec3d(canonicalAxis(dof.axis)))));
        }
        angles[2] = static_cast<float>(info.dofs[2].sign * wrapDegrees(radToDeg(t)));
    }

    void PoseRig::slotFrame(HumanBone bone, Quaternion& left, Quaternion& neutral) const{
        if (!has(bone)){
            left = Quaternion();
            neutral = Quaternion();
            return;
        }
        const Slot& slot = slots[slotIndex(bone)];
        left = slot.left.normalized().toQuaternion();
        neutral = slot.neutral.normalized().toQuaternion();
    }

    Quaternion PoseRig::localFromAngles(HumanBone bone, const float angles[DOFS_PER_BONE]) const{
        if (!has(bone)){
            return Quaternion();
        }
        const Slot& slot = slots[slotIndex(bone)];
        return (slot.left * jointRotation(bone, angles) * slot.neutral).normalized().toQuaternion();
    }

    BodyPose PoseRig::toBodyPose(const HumanPose& pose) const{
        BodyPose body;
        if (!built){
            return body;
        }
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            if (slots[s].bone >= 0){
                anglesFromLocal(boneAt(s), pose.rotations[s], &body.angles[s * DOFS_PER_BONE]);
            }
        }
        const Vec3d displacement = character.inverse() * (hipsParent.apply(Vec3d(pose.hipsPosition)) - neutralHips);
        body.offset = Vector3(static_cast<float>(displacement.x / scale), 0.0f, static_cast<float>(displacement.z / scale));
        return body;
    }

    void PoseRig::applyBodyPoseRotations(const BodyPose& body, HumanPose& pose) const{
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            pose.rotations[s] = slots[s].bone >= 0 && built ? localFromAngles(boneAt(s), &body.angles[s * DOFS_PER_BONE]) : Quaternion();
        }
    }

    HumanPose PoseRig::fromBodyPose(const BodyPose& body, const PoseSkin* skinOverride, bool grounded, float lift) const{
        HumanPose pose = neutral;
        if (!built){
            return pose;
        }
        applyBodyPoseRotations(body, pose);
        const Vec3d horizontal = character * Vec3d(body.offset.x * scale, 0.0, body.offset.z * scale);
        setHipsWorldPosition(pose, (neutralHips + horizontal).toVector3());
        groundPose(pose, grounded ? 0.0f : lift, skinOverride ? skinOverride : skinPtr.get());
        return pose;
    }

    void PoseRig::setSkin(std::shared_ptr<PoseSkin> skin){
        skinPtr = std::move(skin);
        if (built){
            // a skin that puts the floor above the neutral hips invalidates the rig
            built = computeGrounding(nullptr);
        }
    }

    const PoseSkin* PoseRig::skin() const{ return skinPtr.get(); }

    Quatd PoseRig::neutralWorldCanonical(HumanBone bone) const{
        return has(bone) ? slots[slotIndex(bone)].neutral : Quatd();
    }

    void PoseRig::slotWorldCanonical(const HumanPose& pose, std::array<Quatd, HUMAN_BONE_COUNT>& world) const{
        // W_b = W_p * Q_b * local_b = W_p * N_p^-1 * left_b^-1 * local_b (hips: left^-1 * local)
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            const Slot& slot = slots[s];
            if (!built || slot.bone < 0){
                world[s] = Quatd();
                continue;
            }
            const Quatd local = Quatd(pose.rotations[s]).normalized();
            if (s == 0){
                world[s] = (slot.left.inverse() * local).normalized();
            }else{
                const int p = slotIndex(slot.parent);
                world[s] = (world[p] * slots[p].neutral.inverse() * slot.left.inverse() * local).normalized();
            }
        }
    }

    Quatd PoseRig::localFromSlotWorld(HumanBone bone, const Quatd& parentSlotWorld, const Quatd& world) const{
        if (!has(bone)){
            return Quatd();
        }
        const Slot& slot = slots[slotIndex(bone)];
        if (bone == HumanBone::Hips){
            return (slot.left * world).normalized();
        }
        const Slot& parent = slots[slotIndex(slot.parent)];
        // local = Q_b^-1 * W_p^-1 * W = left_b * N_p * W_p^-1 * W
        return (slot.left * parent.neutral * parentSlotWorld.inverse() * world).normalized();
    }

    Vector3 PoseRig::hipsWorldPosition(const HumanPose& pose) const{
        return hipsParent.apply(Vec3d(pose.hipsPosition)).toVector3();
    }

    void PoseRig::setHipsWorldPosition(HumanPose& pose, const Vector3& modelPosition) const{
        pose.hipsPosition = hipsParent.solveLinear(Vec3d(modelPosition) - hipsParent.t).toVector3();
    }

    float PoseRig::soleOffset(HumanBone bone) const{
        return bone == HumanBone::Count ? 0.0f : static_cast<float>(soles[slotIndex(bone)]);
    }

    double PoseRig::lowestPointPrecise(const HumanPose& pose, const PoseSkin* skin) const{
        if (!built){
            return 0.0;
        }
        std::vector<Quatd> rot;
        std::vector<Affine3d> world;
        fkPrecise(pose, rot, world);
        if (skin){
            const double lowest = skin->lowest(world);
            if (std::isfinite(lowest)){
                return lowest;
            }
        }
        double estimate = std::numeric_limits<double>::infinity();
        for (HumanBone b : SOLE_BONES){
            if (has(b)){
                const int s = slotIndex(b);
                estimate = std::min(estimate, world[slots[s].bone].t.y - soles[s]);
            }
        }
        return estimate;
    }

    void PoseRig::groundPose(HumanPose& pose, float lift, const PoseSkin* skin) const{
        if (!built){
            return;
        }
        moveLowestTo(pose, floor + static_cast<double>(lift) * scale, lowestPointPrecise(pose, skin), skin);
    }

    double PoseRig::moveLowestTo(HumanPose& pose, double target, double lowest, const PoseSkin* skin) const{
        if (!built){
            return lowest;
        }
        const double tolerance = 1e-12 * std::max(1.0, std::fabs(target));
        // One step is exact when the lowest point moves with the hips. A lowest skin vertex
        // weighted partly to bones outside the hips subtree moves less: the next steps divide by
        // the measured response (secant). A step that does not move it at all is undone.
        double response = 1.0;
        for (int iteration = 0; iteration < 8; iteration++){
            const double remaining = target - lowest;
            if (!std::isfinite(remaining) || std::fabs(remaining) < tolerance){
                break;
            }
            const double step = remaining / response;
            const Vector3 previous = pose.hipsPosition;
            pose.hipsPosition = (Vec3d(pose.hipsPosition) + hipsParent.solveLinear(Vec3d(0, step, 0))).toVector3();
            const double moved = lowestPointPrecise(pose, skin);
            if (std::fabs(moved - lowest) < 1e-9 * std::fabs(step)){
                pose.hipsPosition = previous;
                break;
            }
            response = clampd((moved - lowest) / step, 0.05, 1.0);
            lowest = moved;
        }
        return lowest;
    }

    BodyPose mirrorBodyPose(const BodyPose& body){
        BodyPose result;
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            const HumanBone bone = boneAt(s);
            const HumanBone mirror = humanBoneInfo(bone).mirror;
            for (int k = 0; k < DOFS_PER_BONE; k++){
                float value = body.angles[dofIndex(mirror, k)];
                if (mirror == bone && dofInfo(dofIndex(bone, k)).axis != CanonicalAxis::L){
                    value = -value;
                }
                result.angles[dofIndex(bone, k)] = value;
            }
        }
        result.offset = Vector3(-body.offset.x, body.offset.y, body.offset.z);
        return result;
    }

    // |(dof1, dof2)| in double, as both the cap and the check measure it
    static double swingLength(const float angles[DOFS_PER_BONE]){
        return std::sqrt(static_cast<double>(angles[0]) * angles[0] + static_cast<double>(angles[1]) * angles[1]);
    }

    bool boneWithinLimits(HumanBone bone, const float angles[DOFS_PER_BONE]){
        for (int k = 0; k < DOFS_PER_BONE; k++){
            const DofInfo& info = dofInfo(dofIndex(bone, k));
            if (!(angles[k] >= info.minDegrees && angles[k] <= info.maxDegrees)){
                return false;
            }
        }
        // the same cap clampBoneToLimits applies: a bone within limits is left unchanged by it
        return swingLength(angles) <= MAX_SWING_DEGREES;
    }

    unsigned clampBoneToLimits(HumanBone bone, float angles[DOFS_PER_BONE]){
        unsigned changed = 0;
        for (int k = 0; k < DOFS_PER_BONE; k++){
            const DofInfo& info = dofInfo(dofIndex(bone, k));
            const float value = angles[k];
            const float limited = std::isnan(value) ? 0.0f : std::min(std::max(value, info.minDegrees), info.maxDegrees);
            if (limited != value || std::isnan(value)){
                angles[k] = limited;
                changed |= 1u << k;
            }
        }
        // the swing (dof1, dof2) is at most MAX_SWING_DEGREES long: beyond 180 it is the same
        // rotation as a shorter swing the other way, and at 180 the twist is undefined (a twisted
        // key would not survive the frames' limit pass), so the cap is half a degree below.
        // Every limit range contains 0, so scaling stays inside.
        const double swing = swingLength(angles);
        if (swing > MAX_SWING_DEGREES){
            const double factor = MAX_SWING_DEGREES / swing;
            angles[0] = static_cast<float>(angles[0] * factor);
            angles[1] = static_cast<float>(angles[1] * factor);
            // rounding to float can land a hair above the cap: step toward 0 until the result
            // passes boneWithinLimits (one or two ulps; the direction is kept)
            while (swingLength(angles) > MAX_SWING_DEGREES){
                angles[0] = std::nextafter(angles[0], 0.0f);
                angles[1] = std::nextafter(angles[1], 0.0f);
            }
            changed |= 3u;
        }
        return changed;
    }

    std::vector<int> clampToLimits(BodyPose& body){
        std::vector<int> clamped;
        for (int b = 0; b < HUMAN_BONE_COUNT; b++){
            const unsigned changed = clampBoneToLimits(boneAt(b), &body.angles[b * DOFS_PER_BONE]);
            for (int k = 0; k < DOFS_PER_BONE; k++){
                if (changed & (1u << k)){
                    clamped.push_back(b * DOFS_PER_BONE + k);
                }
            }
        }
        return clamped;
    }

}
