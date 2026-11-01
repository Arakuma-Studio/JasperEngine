// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SYNC4-ABSORB(retarget): upstream's anim-retarget engine tests (humanoid clips, mirror, foot IK, .anim v2 /
// .humanoid v2), built against the merged engine core; revisit with the retarget absorption

// Humanoid retargeting (anim-retarget spec 14, tests 3-10): a clip converted on one test rig and
// realised on another rig gives the same joint angles and neutral-relative frames, hips height
// scales with humanScale, a slot the target lacks folds into the next present one, Mirror, Foot
// IK on longer legs, extra bones across bone axis conventions, twist corrections, and the .anim
// v2 / .humanoid v2 file formats with their byte-identical v1 round trips.

#include <gtest/gtest.h>

#include "../pose/TestRigs.h"

#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/HumanoidClip.h"
#include "animation/KeyframeSampling.h"
#include "pose/HumanExtraBones.h"
#include "pose/HumanoidMapIO.h"
#include "pose/PoseDofs.h"
#include "pose/PoseIK.h"
#include "pose/PoseRig.h"

#include <cmath>
#include <memory>
#include <string>

using namespace jasperengine;
using namespace jasperengine::pose;
using namespace posetest;

namespace {

    double degreesBetween(const Quatd& a, const Quatd& b){
        return radToDeg(angleBetween(a.normalized(), b.normalized()));
    }

    double degreesBetween(const Vec3d& a, const Vec3d& b){
        return radToDeg(angleBetween(a.normalized(), b.normalized()));
    }

    std::vector<float> xyzw(const Quaternion& q){
        std::vector<float> out(4);
        quaternionToXYZW(q, out.data());
        return out;
    }

    // a source of a test rig: bones addressed by their names, no bridges
    anim::HumanoidSource sourceOf(const std::shared_ptr<PoseRig>& rig){
        anim::HumanoidSource source;
        source.modelPath = "models/test.gltf";
        source.rig = rig;
        source.humanoid = rig->humanoid();
        source.extra = autoDetectExtraBones(rig->skeleton(), rig->humanoid());
        const Skeleton& skeleton = rig->skeleton();
        source.bridges.assign(skeleton.bones.size(), Matrix4());
        for (const SkeletonBone& bone : skeleton.bones){
            source.bonePaths.push_back(bone.name);
            source.restRotation.push_back(bone.rotation);
            source.restPosition.push_back(bone.position);
            source.restScale.push_back(bone.scale);
        }
        for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++) source.extraBones.push_back(findSkeletonBone(skeleton, source.extra.bones[(size_t)x]));
        return source;
    }

    // a target of a test rig: entity i + 1 for skeleton bone i
    std::shared_ptr<anim::HumanoidTarget> targetOf(const std::shared_ptr<PoseRig>& rig){
        ModelRigInfo info;
        info.rig = rig;
        info.humanoid = rig->humanoid();
        info.extra = autoDetectExtraBones(rig->skeleton(), rig->humanoid());
        std::vector<Entity> entities;
        for (size_t i = 0; i < rig->skeleton().bones.size(); i++) entities.push_back((Entity)(i + 1));
        auto target = std::make_shared<anim::HumanoidTarget>();
        EXPECT_TRUE(anim::buildHumanoidTarget(info, entities, std::vector<Matrix4>(entities.size(), Matrix4()), 1000, "models/target.gltf", *target));
        return target;
    }

    std::shared_ptr<PoseRig> buildRig(const Skeleton& skeleton, const std::array<float, HUMAN_BONE_COUNT>* twist = nullptr){
        auto rig = std::make_shared<PoseRig>();
        std::string error;
        EXPECT_TRUE(rig->build(skeleton, autoDetectHumanoid(skeleton), &error, twist)) << error;
        return rig;
    }

    // a generic clip of one source pose at t = 0 (and `second` at t = 1 when given): a rotation
    // track per mapped slot bone (and every animated extra bone), the hips translation
    ClipData clipOfPoses(const PoseRig& rig, const std::vector<std::pair<float, HumanPose>>& poses){
        ClipData data;
        data.wrap = ClipWrapMode::Loop;
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            if (!rig.has(boneAt(s))) continue;
            ClipTrackData track;
            track.path = rig.skeleton().bones[(size_t)rig.boneIndex(boneAt(s))].name;
            track.channel = ClipChannel::Rotation;
            for (const auto& pose : poses){
                ClipKey key;
                key.time = pose.first;
                key.value = xyzw(pose.second.rotations[(size_t)s]);
                track.keys.push_back(key);
            }
            data.tracks.push_back(track);
        }
        ClipTrackData hips;
        hips.path = rig.skeleton().bones[(size_t)rig.boneIndex(HumanBone::Hips)].name;
        hips.channel = ClipChannel::Position;
        for (const auto& pose : poses){
            ClipKey key;
            key.time = pose.first;
            key.value = {pose.second.hipsPosition.x, pose.second.hipsPosition.y, pose.second.hipsPosition.z};
            hips.keys.push_back(key);
        }
        data.tracks.push_back(hips);
        anim::normalize(data);
        return data;
    }

    // converts on `source`, realises at `time` on `target`
    pose::HumanPose retarget(const ClipData& generic, const anim::HumanoidSource& source, const std::shared_ptr<anim::HumanoidTarget>& target,
                             float time, bool mirror = false, bool footIK = false, anim::HumanoidBinding* keep = nullptr){
        ClipData humanoid;
        std::vector<ClipWarning> warnings;
        std::string error;
        EXPECT_TRUE(anim::convertToHumanoid(generic, source, humanoid, warnings, &error)) << error;
        std::shared_ptr<const AnimationClip> clip = AnimationClip::build(humanoid);
        anim::HumanoidBinding binding;
        EXPECT_TRUE(anim::bindHumanoidClip(*clip, target, binding));
        binding.mirror = mirror;
        binding.footIK = footIK;
        anim::sampleHumanoidBinding(*clip, binding, time, nullptr);
        if (keep) *keep = binding;
        return binding.pose;
    }

    BodyPose bodyWith(std::initializer_list<std::pair<const char*, float>> dofs){
        BodyPose body;
        for (const auto& entry : dofs){
            int dof = -1;
            EXPECT_TRUE(dofFromName(entry.first, dof)) << entry.first;
            if (dof >= 0) body.angles[(size_t)dof] = entry.second;
        }
        return body;
    }

    // G_b = W_b * N_b^-1: the neutral-relative canonical frame of every present slot
    std::array<Quatd, HUMAN_BONE_COUNT> frames(const PoseRig& rig, const HumanPose& pose){
        std::array<Quatd, HUMAN_BONE_COUNT> world;
        rig.slotWorldCanonical(pose, world);
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            world[(size_t)s] = (world[(size_t)s] * rig.neutralWorldCanonical(boneAt(s)).inverse()).normalized();
        }
        return world;
    }

    // a copy of `skeleton` with every bone below the hips' children moved by `factor` (a scaled
    // rig); legs only when `legsOnly`
    Skeleton scaled(const Skeleton& skeleton, double factor, bool legsOnly){
        Skeleton out = skeleton;
        for (SkeletonBone& bone : out.bones){
            const std::vector<std::string> tokens = humanoidNameTokens(bone.name);
            bool leg = false;
            for (const std::string& token : tokens) leg = leg || token == "leg" || token == "foot" || token == "toe" || token == "up";
            if (legsOnly && !leg) continue;
            if (legsOnly && bone.name.find("UpLeg") != std::string::npos) continue;   // the hip joint stays
            bone.position = Vector3(bone.position.x * (float)factor, bone.position.y * (float)factor, bone.position.z * (float)factor);
        }
        return out;
    }

}

// ---- test 3: cross-rig angles ----

TEST(HumanoidRetarget, CrossRigAnglesMatch){
    const std::shared_ptr<PoseRig> sourceRig = tposeRig().skinned;
    const BodyPose body = bodyWith({{"left_upper_arm.raise", 60.0f}, {"left_lower_arm.bend", 30.0f}, {"spine.bend", 10.0f}});
    const HumanPose sourcePose = sourceRig->fromBodyPose(body, nullptr, true, 0.0f);
    const ClipData generic = clipOfPoses(*sourceRig, {{0.0f, sourcePose}});
    const anim::HumanoidSource source = sourceOf(sourceRig);
    const std::array<Quatd, HUMAN_BONE_COUNT> sourceFrames = frames(*sourceRig, sourcePose);

    for (const TestRig* rig : {&aposeRig(), &backwardRig(), &skewedRig()}){
        SCOPED_TRACE(rig->label);
        const std::shared_ptr<anim::HumanoidTarget> target = targetOf(rig->skinned);
        const HumanPose pose = retarget(generic, source, target, 0.0f);
        const PoseRig& targetRig = *rig->skinned;
        for (const char* name : {"left_upper_arm", "left_lower_arm", "spine"}){
            HumanBone bone;
            ASSERT_TRUE(humanBoneFromName(name, bone));
            if (!targetRig.has(bone)) continue;
            float expected[3], actual[3];
            sourceRig->anglesFromLocal(bone, sourcePose.rotations[(size_t)boneIndex(bone)], expected);
            targetRig.anglesFromLocal(bone, pose.rotations[(size_t)boneIndex(bone)], actual);
            for (int k = 0; k < 3; k++) EXPECT_NEAR(actual[k], expected[k], 0.05) << name << " dof " << k;
        }
        // the left forearm's direction in the canonical frame (G * neutral direction)
        const std::array<Quatd, HUMAN_BONE_COUNT> targetFrames = frames(targetRig, pose);
        const Vec3d direction(boneDofInfo(HumanBone::LeftLowerArm).neutralDirection);
        const int forearm = boneIndex(HumanBone::LeftLowerArm);
        EXPECT_LT(degreesBetween(targetFrames[(size_t)forearm] * direction, sourceFrames[(size_t)forearm] * direction), 0.1);
    }
}

// ---- test 4: hips scaling ----

TEST(HumanoidRetarget, HipsDisplacementScalesWithHumanScale){
    const std::shared_ptr<PoseRig> sourceRig = tposeRig().rig;
    const std::shared_ptr<PoseRig> halfRig = buildRig(scaled(tposeRig().skeleton, 0.5, false));
    ASSERT_NEAR(halfRig->humanScale(), sourceRig->humanScale() * 0.5, 1e-4);

    // a source hips displaced by (0, 0.05, 0.3) humanScale units along the canonical axes
    HumanPose pose = sourceRig->neutralPose();
    const Vec3d offset(0.0, 0.05, 0.3);
    const Vec3d sourceModel = Vec3d(sourceRig->neutralHipsWorldPosition()) + Quatd(sourceRig->characterRotation()) * (offset * sourceRig->humanScale());
    sourceRig->setHipsWorldPosition(pose, sourceModel.toVector3());
    const ClipData generic = clipOfPoses(*sourceRig, {{0.0f, pose}});
    const HumanPose result = retarget(generic, sourceOf(sourceRig), targetOf(halfRig), 0.0f);

    const Vec3d displacement = Vec3d(halfRig->hipsWorldPosition(result)) - Vec3d(halfRig->neutralHipsWorldPosition());
    const Vec3d sourceDisplacement = sourceModel - Vec3d(sourceRig->neutralHipsWorldPosition());
    const Vec3d expected = Quatd(halfRig->characterRotation()) * (Quatd(sourceRig->characterRotation()).inverse() * sourceDisplacement * 0.5);
    EXPECT_NEAR(displacement.x, expected.x, 1e-5);
    EXPECT_NEAR(displacement.y, expected.y, 1e-5);
    EXPECT_NEAR(displacement.z, expected.z, 1e-5);
    EXPECT_NEAR(displacement.length(), sourceDisplacement.length() * 0.5, 1e-5);
}

// ---- test 5: missing slot fold ----

TEST(HumanoidRetarget, MissingSlotsFoldIntoTheNextPresentSlot){
    const std::shared_ptr<PoseRig> sourceRig = tposeRig().rig;
    ASSERT_TRUE(sourceRig->has(HumanBone::Chest));
    // chest E = 20 degrees about the canonical left axis, the rest neutral
    HumanPose pose = sourceRig->neutralPose();
    pose.rotations[(size_t)boneIndex(HumanBone::Chest)] =
        sourceRig->localFromJointRotation(HumanBone::Chest, axisAngleRad(Vec3d(1, 0, 0), degToRad(20.0)));
    const ClipData generic = clipOfPoses(*sourceRig, {{0.0f, pose}});

    const std::shared_ptr<PoseRig> minimal = minimalRig().rig;
    ASSERT_FALSE(minimal->has(HumanBone::Chest));
    ASSERT_FALSE(minimal->has(HumanBone::Neck));
    anim::HumanoidBinding binding;
    const HumanPose result = retarget(generic, sourceOf(sourceRig), targetOf(minimal), 0.0f, false, false, &binding);
    const std::array<Quatd, HUMAN_BONE_COUNT> source = frames(*sourceRig, pose);
    const std::array<Quatd, HUMAN_BONE_COUNT> target = frames(*minimal, result);
    for (HumanBone bone : {HumanBone::Head, HumanBone::LeftUpperArm, HumanBone::RightUpperArm, HumanBone::Spine}){
        EXPECT_LT(degreesBetween(target[(size_t)boneIndex(bone)], source[(size_t)boneIndex(bone)]), 0.01) << humanBoneName(bone);
    }
    bool reported = false;
    for (const std::string& line : binding.folded) reported = reported || line == "chest folded into head";
    EXPECT_TRUE(reported);
}

// ---- test 6: mirror ----

TEST(HumanoidRetarget, MirrorRaisesTheOtherArm){
    const std::shared_ptr<PoseRig> rig = tposeRig().rig;
    const BodyPose body = bodyWith({{"left_upper_arm.raise", 60.0f}, {"left_upper_arm.forward", 20.0f}, {"left_upper_arm.twist", 25.0f},
                                    {"spine.side", 8.0f}, {"spine.twist", 12.0f}});
    const HumanPose pose = rig->fromBodyPose(body, nullptr, true, 0.0f);
    const ClipData generic = clipOfPoses(*rig, {{0.0f, pose}});
    const HumanPose mirrored = retarget(generic, sourceOf(rig), targetOf(rig), 0.0f, true);
    const BodyPose expected = mirrorBodyPose(rig->toBodyPose(pose));
    const BodyPose actual = rig->toBodyPose(mirrored);
    for (int dof = 0; dof < DOF_COUNT; dof++){
        if (!rig->has(dofBone(dof))) continue;
        EXPECT_NEAR(actual.angles[(size_t)dof], expected.angles[(size_t)dof], 0.05) << dofName(dof);
    }

    // twice: the identity
    anim::HumanoidSample sample;
    sample.E[(size_t)boneIndex(HumanBone::LeftUpperArm)] = Quaternion(0.9f, 0.1f, 0.3f, -0.2f).normalized();
    sample.hasE[(size_t)boneIndex(HumanBone::LeftUpperArm)] = true;
    sample.hips = Vector3(0.1f, 0.2f, 0.3f);
    sample.hasHips = true;
    sample.feet[0] = Vector3(0.2f, -0.9f, 0.1f);
    sample.hasFeet[0] = true;
    sample.extra[(size_t)extraIndex(HumanExtraBone::LeftIndexProximal)] = Quaternion(0.8f, 0.2f, 0.1f, 0.3f).normalized();
    sample.hasExtra[(size_t)extraIndex(HumanExtraBone::LeftIndexProximal)] = true;
    anim::HumanoidSample once, twice;
    anim::mirrorHumanoidSample(sample, once);
    EXPECT_TRUE(once.hasE[(size_t)boneIndex(HumanBone::RightUpperArm)]);
    EXPECT_FALSE(once.hasE[(size_t)boneIndex(HumanBone::LeftUpperArm)]);
    EXPECT_TRUE(once.hasFeet[1]);
    anim::mirrorHumanoidSample(once, twice);
    EXPECT_EQ(twice.hasE, sample.hasE);
    EXPECT_EQ(twice.hasExtra, sample.hasExtra);
    const Quaternion& back = twice.E[(size_t)boneIndex(HumanBone::LeftUpperArm)];
    const Quaternion& original = sample.E[(size_t)boneIndex(HumanBone::LeftUpperArm)];
    EXPECT_EQ(back.w, original.w);
    EXPECT_EQ(back.y, original.y);
    EXPECT_EQ(twice.hips.x, sample.hips.x);
    EXPECT_EQ(twice.feet[0].x, sample.feet[0].x);
}

// ---- test 7: foot IK ----

TEST(HumanoidRetarget, FootIKReachesTheGoalsOnLongerLegs){
    const std::shared_ptr<PoseRig> sourceRig = tposeRig().skinned;
    const std::shared_ptr<PoseRig> longLegs = buildRig(scaled(tposeRig().skeleton, 1.2, true));
    ASSERT_GT(longLegs->humanScale(), sourceRig->humanScale() * 1.05);
    // a walk: the legs swing, the knees bend, the hips go forward and bob
    std::vector<std::pair<float, HumanPose>> poses;
    for (int k = 0; k <= 8; k++){
        const float t = k / 8.0f;
        const float phase = (float)(2.0 * POSE_PI * t);
        BodyPose body = bodyWith({{"left_upper_leg.forward", 25.0f * std::sin(phase)}, {"right_upper_leg.forward", -25.0f * std::sin(phase)},
                                  {"left_lower_leg.bend", 20.0f + 15.0f * std::cos(phase)}, {"right_lower_leg.bend", 20.0f - 15.0f * std::cos(phase)}});
        body.offset = Vector3(0.0f, 0.0f, 0.4f * t);
        poses.push_back({t, sourceRig->fromBodyPose(body, nullptr, true, 0.0f)});
    }
    const ClipData generic = clipOfPoses(*sourceRig, poses);
    const anim::HumanoidSource source = sourceOf(sourceRig);
    ClipData humanoid;
    std::vector<ClipWarning> warnings;
    std::string error;
    ASSERT_TRUE(anim::convertToHumanoid(generic, source, humanoid, warnings, &error)) << error;
    ASSERT_GE(humanoid.findHumanoidTrack(ClipChannel::HumanoidPosition, HUMANOID_POSITION_LEFT_FOOT), 0);
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(humanoid);
    const std::shared_ptr<anim::HumanoidTarget> target = targetOf(longLegs);
    anim::HumanoidBinding plain, solved;
    ASSERT_TRUE(anim::bindHumanoidClip(*clip, target, plain));
    ASSERT_TRUE(anim::bindHumanoidClip(*clip, target, solved));
    solved.footIK = true;
    const double scale = longLegs->humanScale();
    int checked = 0;
    for (int k = 0; k <= 16; k++){
        const float t = k / 16.0f;
        anim::sampleHumanoidBinding(*clip, plain, t, nullptr);
        anim::sampleHumanoidBinding(*clip, solved, t, nullptr);
        std::vector<Quaternion> rotPlain, rotSolved;
        std::vector<Vector3> posPlain, posSolved;
        longLegs->computeWorld(plain.pose, rotPlain, posPlain);
        longLegs->computeWorld(solved.pose, rotSolved, posSolved);
        for (int side = 0; side < 2; side++){
            const HumanBone foot = side == 0 ? HumanBone::LeftFoot : HumanBone::RightFoot;
            const HumanBone upper = side == 0 ? HumanBone::LeftUpperLeg : HumanBone::RightUpperLeg;
            const Vector3 goalValue = solved.sample.feet[side];
            const Vec3d goal = Vec3d(longLegs->neutralHipsWorldPosition()) + Quatd(longLegs->characterRotation()) * (Vec3d(goalValue) * scale);
            const int footBone = longLegs->boneIndex(foot);
            const int hipBone = longLegs->boneIndex(upper);
            // within reach: the leg's length is the sum of its bone lengths
            const double reach = (Vec3d(longLegs->skeleton().bones[(size_t)longLegs->boneIndex(side == 0 ? HumanBone::LeftLowerLeg : HumanBone::RightLowerLeg)].position).length() +
                                  Vec3d(longLegs->skeleton().bones[(size_t)footBone].position).length()) * 0.999;
            if ((goal - Vec3d(posSolved[(size_t)hipBone])).length() > reach) continue;
            EXPECT_LT((Vec3d(posSolved[(size_t)footBone]) - goal).length(), 1e-3 * scale) << "t " << t << " side " << side;
            EXPECT_LT(degreesBetween(Quatd(rotSolved[(size_t)footBone]), Quatd(rotPlain[(size_t)footBone])), 0.05);
            checked++;
        }
    }
    EXPECT_GT(checked, 10);
}

// ---- test 8: extra bones across axis conventions ----

TEST(HumanoidRetarget, ExtraBonesCancelAxisConventions){
    const TestRig& base = tposeRig();
    const std::shared_ptr<PoseRig> sourceRig = base.rig;
    const anim::HumanoidSource source = sourceOf(sourceRig);
    const int index = source.extraBones[(size_t)extraIndex(HumanExtraBone::LeftIndexProximal)];
    ASSERT_GE(index, 0) << "the T-pose rig has Mixamo fingers";

    // the target: the same bones, the index proximal's rest frame turned 90 degrees about its own
    // axis (toward its child), its child's rest counter-turned (the same world geometry)
    Skeleton turned = base.skeleton;
    int child = -1;
    for (size_t i = 0; i < turned.bones.size(); i++){
        if (turned.bones[i].parent == index) child = (int)i;
    }
    ASSERT_GE(child, 0);
    const Vec3d axis = Vec3d(turned.bones[(size_t)child].position).normalized();
    const Quatd spin = axisAngleRad(axis, degToRad(90.0));
    turned.bones[(size_t)index].rotation = (Quatd(turned.bones[(size_t)index].rotation) * spin).normalized().toQuaternion();
    turned.bones[(size_t)child].position = (spin.inverse() * Vec3d(turned.bones[(size_t)child].position)).toVector3();
    turned.bones[(size_t)child].rotation = (spin.inverse() * Quatd(turned.bones[(size_t)child].rotation)).normalized().toQuaternion();
    const std::shared_ptr<PoseRig> targetRig = buildRig(turned);

    // a 30 degree curl on the source (about the bone-local axis across the finger)
    const Quatd rest(base.skeleton.bones[(size_t)index].rotation);
    const Vec3d across = axis.cross(Vec3d(0, 0, 1)).normalized();
    const Quatd curled = rest * axisAngleRad(across.length() > 0.5 ? across : Vec3d(1, 0, 0), degToRad(30.0));
    ClipData generic;
    ClipTrackData track;
    track.path = base.skeleton.bones[(size_t)index].name;
    track.channel = ClipChannel::Rotation;
    ClipKey key;
    key.value = xyzw(curled.toQuaternion());
    track.keys.push_back(key);
    generic.tracks.push_back(track);

    anim::HumanoidBinding binding;
    retarget(generic, source, targetOf(targetRig), 0.0f, false, false, &binding);
    // the fingertip (the child) in the canonical frame: the same on both rigs
    auto tip = [](const PoseRig& rig, const Skeleton& skeleton, int bone, int tipBone, const Quatd& local){
        std::vector<Quatd> rot;
        std::vector<Affine3d> world;
        rig.computeWorldPrecise(rig.neutralPose(), rot, world);
        const int parent = skeleton.bones[(size_t)bone].parent;
        const Quatd boneWorld = rot[(size_t)parent] * local;
        const Vec3d bonePos = world[(size_t)bone].t;
        return bonePos + boneWorld * Vec3d(skeleton.bones[(size_t)tipBone].position);
    };
    const Quatd targetLocal(binding.extraLocal[(size_t)extraIndex(HumanExtraBone::LeftIndexProximal)]);
    bool output = false;
    for (const anim::HumanoidOutput& entry : binding.outputs){
        output = output || (entry.kind == anim::HumanoidOutput::Kind::Extra && entry.index == extraIndex(HumanExtraBone::LeftIndexProximal));
    }
    ASSERT_TRUE(output);
    const Vec3d expected = tip(*sourceRig, base.skeleton, index, child, curled);
    const Vec3d actual = tip(*targetRig, turned, index, child, targetLocal);
    const Vec3d hand = Vec3d(sourceRig->restPose().hipsPosition);   // any common point
    EXPECT_LT(degreesBetween(actual - hand, expected - hand), 0.1);
    EXPECT_LT((actual - expected).length(), 1e-4);
}

// ---- test 9: twist correction ----

TEST(HumanoidRetarget, TwistCorrectionTurnsTheNeutralPose){
    const TestRig& base = tposeRig();
    std::array<float, HUMAN_BONE_COUNT> twist{};
    twist[(size_t)boneIndex(HumanBone::LeftLowerArm)] = 90.0f;
    const std::shared_ptr<PoseRig> plain = buildRig(base.skeleton);
    const std::shared_ptr<PoseRig> twisted = buildRig(base.skeleton, &twist);
    EXPECT_FLOAT_EQ(twisted->twist(HumanBone::LeftLowerArm), 90.0f);
    const Vec3d direction(boneDofInfo(HumanBone::LeftLowerArm).neutralDirection);
    const Quatd expected = axisAngleRad(direction, degToRad(90.0)) * plain->neutralWorldCanonical(HumanBone::LeftLowerArm);
    EXPECT_LT(degreesBetween(twisted->neutralWorldCanonical(HumanBone::LeftLowerArm), expected), 1e-4);

    // a humanoid clip with E = I shows it (the realised forearm is the twisted neutral)
    anim::HumanoidSample sample;
    for (int s = 0; s < HUMAN_BONE_COUNT; s++) sample.hasE[(size_t)s] = true;
    HumanPose realised;
    std::array<Quaternion, HUMAN_EXTRA_BONE_COUNT> extra;
    anim::realiseHumanoid(*targetOf(twisted), sample, false, false, realised, extra);
    std::array<Quatd, HUMAN_BONE_COUNT> world;
    twisted->slotWorldCanonical(realised, world);
    EXPECT_LT(degreesBetween(world[(size_t)boneIndex(HumanBone::LeftLowerArm)], expected), 1e-3);
    // and the pose system's neutral pose (what a .poseanim realises) too
    std::array<Quatd, HUMAN_BONE_COUNT> neutral;
    twisted->slotWorldCanonical(twisted->neutralPose(), neutral);
    EXPECT_LT(degreesBetween(neutral[(size_t)boneIndex(HumanBone::LeftLowerArm)], expected), 1e-3);
    // the untouched slots are as before
    EXPECT_LT(degreesBetween(twisted->neutralWorldCanonical(HumanBone::RightLowerArm), plain->neutralWorldCanonical(HumanBone::RightLowerArm)), 1e-6);
}

// ---- round trip on the source rig (spec H1), synthetic ----

TEST(HumanoidRetarget, SourceRigRoundTripIsExact){
    const std::shared_ptr<PoseRig> rig = aposeRig().skinned;
    const BodyPose a = bodyWith({{"left_upper_arm.raise", 40.0f}, {"right_lower_arm.bend", 70.0f}, {"neck.bend", -15.0f}, {"left_foot.point", 10.0f}});
    const BodyPose b = bodyWith({{"left_upper_arm.raise", -10.0f}, {"right_lower_arm.bend", 20.0f}, {"head.twist", 25.0f}, {"hips.turn", 30.0f}});
    const HumanPose first = rig->fromBodyPose(a, nullptr, true, 0.0f);
    const HumanPose second = rig->fromBodyPose(b, nullptr, false, 0.1f);
    const ClipData generic = clipOfPoses(*rig, {{0.0f, first}, {1.0f, second}});
    std::shared_ptr<const AnimationClip> genericClip = AnimationClip::build(generic);
    const anim::HumanoidSource source = sourceOf(rig);
    const std::shared_ptr<anim::HumanoidTarget> target = targetOf(rig);
    anim::HumanoidBinding binding;
    retarget(generic, source, target, 0.0f, false, false, &binding);
    ClipData humanoid;
    std::vector<ClipWarning> warnings;
    ASSERT_TRUE(anim::convertToHumanoid(generic, source, humanoid, warnings, nullptr));
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(humanoid);
    ASSERT_TRUE(anim::bindHumanoidClip(*clip, target, binding));
    for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}){
        anim::sampleHumanoidBinding(*clip, binding, t, nullptr);
        std::vector<anim::TrackSample> samples;
        anim::sampleClipTracks(*genericClip, t, samples);
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            if (!rig->has(boneAt(s))) continue;
            const int track = genericClip->findTrack(rig->skeleton().bones[(size_t)rig->boneIndex(boneAt(s))].name, ClipChannel::Rotation);
            ASSERT_GE(track, 0);
            EXPECT_LT(degreesBetween(Quatd(binding.pose.rotations[(size_t)s]), Quatd(samples[(size_t)track].rotation)), 1e-3)
                << humanBoneName(boneAt(s)) << " t " << t;
        }
        const int hips = genericClip->findTrack(rig->skeleton().bones[(size_t)rig->boneIndex(HumanBone::Hips)].name, ClipChannel::Position);
        EXPECT_LT((Vec3d(binding.pose.hipsPosition) - Vec3d(samples[(size_t)hips].vector)).length(), 1e-4 * rig->humanScale());
    }
}

// ---- test 10: file formats ----

TEST(HumanoidClipIO, VersionOneClipsStayByteIdentical){
    ClipData data;
    data.wrap = ClipWrapMode::Loop;
    ClipTrackData track;
    track.path = "Hero/Arm";
    track.channel = ClipChannel::Rotation;
    ClipKey key;
    key.value = {0, 0, 0, 1};
    track.keys.push_back(key);
    data.tracks.push_back(track);
    const std::string text = anim::writeClip(data);
    EXPECT_NE(text.find("\"version\": 1,"), std::string::npos);
    anim::ClipParseResult parsed = anim::parseClip(text);
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(anim::writeClip(*parsed.data), text);
}

TEST(HumanoidClipIO, HumanoidTracksWriteVersionTwo){
    const std::string text =
        "{\"format\": \"jasperengine.anim\", \"version\": 2, \"frame_rate\": 30, \"wrap\": \"loop\",\n"
        " \"humanoid\": {\"source_model\": \"models/hero.gltf\", \"human_scale\": 0.982},\n"
        " \"tracks\": [\n"
        "  {\"path\": \"Hero/Hat\", \"channel\": \"rotation\", \"keys\": [[0, [0, 0, 0, 1]]]},\n"
        "  {\"channel\": \"humanoid_position\", \"bone\": \"hips\", \"keys\": [[0, [0, -0.02, 0]], [0.5, [0, 0.01, 0.15]]]},\n"
        "  {\"channel\": \"humanoid\", \"bone\": \"left_upper_arm\", \"keys\": [[0, {\"angles\": [12, 5, 0]}], [1, {\"angles\": [80, 0, 10]}]]},\n"
        "  {\"channel\": \"humanoid\", \"bone\": \"hips\", \"keys\": [[0, [0, 0, 0, 1]]]},\n"
        "  {\"channel\": \"humanoid\", \"bone\": \"left_index_proximal\", \"keys\": [[0, [0.2588, 0, 0, 0.9659]]]}\n"
        " ]}";
    anim::ClipParseResult parsed = anim::parseClip(text);
    ASSERT_TRUE(parsed.ok()) << parsed.error.toString();
    const ClipData& data = *parsed.data;
    ASSERT_TRUE(data.isHumanoid());
    EXPECT_EQ(data.humanoid.sourceModel, "models/hero.gltf");
    EXPECT_FLOAT_EQ(data.humanoid.humanScale, 0.982f);
    // path tracks first, then humanoid (by channel, then bone)
    ASSERT_EQ(data.tracks.size(), 5u);
    EXPECT_EQ(data.tracks[0].path, "Hero/Hat");
    EXPECT_EQ(data.tracks[1].channel, ClipChannel::Humanoid);
    EXPECT_EQ(data.tracks[1].humanBone, boneIndex(HumanBone::Hips));
    EXPECT_EQ(data.tracks[2].humanBone, boneIndex(HumanBone::LeftUpperArm));
    EXPECT_EQ(data.tracks[3].humanBone, HUMAN_BONE_COUNT + extraIndex(HumanExtraBone::LeftIndexProximal));
    EXPECT_EQ(data.tracks[4].channel, ClipChannel::HumanoidPosition);
    // {angles} became E
    const float angles[3] = {80, 0, 10};
    const Quatd E = jointRotation(HumanBone::LeftUpperArm, angles);
    const std::vector<float>& value = data.tracks[2].keys[1].value;
    EXPECT_LT(degreesBetween(Quatd(value[3], value[0], value[1], value[2]), E), 1e-3);

    const std::string written = anim::writeClip(data);
    EXPECT_NE(written.find("\"version\": 2,"), std::string::npos);
    EXPECT_EQ(written.find("angles"), std::string::npos) << "angles are written back as quaternions";
    EXPECT_NE(written.find("{\"channel\": \"humanoid\", \"bone\": \"left_upper_arm\", \"keys\": ["), std::string::npos) << written;
    EXPECT_NE(written.find("\"humanoid\": {\"source_model\": \"models/hero.gltf\", \"human_scale\": 0.982},"), std::string::npos) << written;
    anim::ClipParseResult again = anim::parseClip(written);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(anim::writeClip(*again.data), written);
    // the runtime finds humanoid tracks by bone
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data);
    EXPECT_TRUE(clip->isHumanoid());
    EXPECT_GE(clip->findTrack("left_upper_arm", ClipChannel::Humanoid), 0);
    EXPECT_EQ(clip->findTrack("", ClipChannel::Humanoid), -1);
}

TEST(HumanoidClipIO, HumanoidTrackWithPathIsRefused){
    anim::ClipParseResult parsed = anim::parseClip(
        "{\"format\": \"jasperengine.anim\", \"version\": 1, \"tracks\": [\n"
        "  {\"path\": \"Hero\", \"channel\": \"humanoid\", \"bone\": \"head\", \"keys\": [[0, [0, 0, 0, 1]]]},\n"
        "  {\"channel\": \"humanoid\", \"bone\": \"tail\", \"keys\": [[0, [0, 0, 0, 1]]]},\n"
        "  {\"channel\": \"humanoid\", \"bone\": \"head\", \"keys\": [[0, [0, 0, 0, 1]]]}\n"
        " ]}");
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.data->tracks.size(), 1u);
    bool path = false, unknown = false, version = false;
    for (const ClipWarning& warning : parsed.warnings){
        path = path || (warning.path == "tracks[0].path" && warning.message.find("track dropped") != std::string::npos);
        unknown = unknown || warning.path == "tracks[1].bone";
        version = version || (warning.path == "version" && warning.message.find("version 2") != std::string::npos);
    }
    EXPECT_TRUE(path);
    EXPECT_TRUE(unknown);
    EXPECT_TRUE(version) << "a version 1 file with humanoid tracks is saved as version 2";
}

TEST(HumanoidSidecarIO, VersionOneRoundTripsAndVersionTwoParses){
    HumanoidOverrides only;
    only.bones[(size_t)boneIndex(HumanBone::LeftUpperArm)] = std::string("upper_arm.L");
    const std::string v1 = writeHumanoidOverrides(only);
    EXPECT_EQ(v1, "{\n  \"format\": \"jasperengine.humanoid\",\n  \"version\": 1,\n  \"bones\": {\n    \"left_upper_arm\": \"upper_arm.L\"\n  }\n}\n");
    EXPECT_EQ(writeHumanoidOverrides(HumanoidOverrides()), "{\n  \"format\": \"jasperengine.humanoid\",\n  \"version\": 1,\n  \"bones\": {}\n}\n");

    const std::string text =
        "{\n  \"format\": \"jasperengine.humanoid\",\n  \"version\": 2,\n  \"animation_type\": \"humanoid\",\n"
        "  \"bones\": {\n    \"left_upper_arm\": \"upper_arm.L\"\n  },\n"
        "  \"extra_bones\": {\n    \"left_index_proximal\": \"f_index.01.L\",\n    \"jaw\": \"\"\n  },\n"
        "  \"twist\": {\n    \"left_lower_arm\": 15,\n    \"right_lower_arm\": -15\n  }\n}\n";
    HumanoidOverrides parsed;
    std::vector<std::string> warnings;
    std::string error;
    ASSERT_TRUE(parseHumanoidOverrides(text, parsed, warnings, &error)) << error;
    EXPECT_TRUE(warnings.empty());
    EXPECT_TRUE(parsed.isHumanoid());
    EXPECT_EQ(parsed.extraBones[(size_t)extraIndex(HumanExtraBone::LeftIndexProximal)], std::optional<std::string>("f_index.01.L"));
    EXPECT_EQ(parsed.extraBones[(size_t)extraIndex(HumanExtraBone::Jaw)], std::optional<std::string>(""));
    EXPECT_EQ(parsed.twist[(size_t)boneIndex(HumanBone::RightLowerArm)], std::optional<float>(-15.0f));
    EXPECT_EQ(writeHumanoidOverrides(parsed), text);

    // unknown extra names and out-of-range twists warn
    ASSERT_TRUE(parseHumanoidOverrides("{\"format\": \"jasperengine.humanoid\", \"version\": 2, \"extra_bones\": {\"tail\": \"x\"}, "
                                       "\"twist\": {\"head\": 400}}", parsed, warnings, &error));
    ASSERT_EQ(warnings.size(), 2u);
    EXPECT_NE(warnings[0].find("tail"), std::string::npos);
    EXPECT_EQ(parsed.twist[(size_t)boneIndex(HumanBone::Head)], std::optional<float>(180.0f));
}

TEST(HumanExtraBones, MixamoFingersAutoDetect){
    const Skeleton& skeleton = tposeRig().skeleton;
    const HumanoidMap map = autoDetectHumanoid(skeleton);
    std::vector<std::string> notes;
    const HumanExtraMap extra = autoDetectExtraBones(skeleton, map, &notes);
    EXPECT_GE(extra.mappedCount(0, HUMAN_FINGER_BONE_COUNT), 12);
    // proximal, intermediate and distal by depth, never the hand or a tip
    for (int side = 0; side < 2; side++){
        for (int finger = 0; finger < 5; finger++){
            const std::string& proximal = extra.bones[(size_t)(side * 15 + finger * 3)];
            if (proximal.empty()) continue;
            const int p = findSkeletonBone(skeleton, proximal);
            const int i = findSkeletonBone(skeleton, extra.bones[(size_t)(side * 15 + finger * 3 + 1)]);
            ASSERT_GE(p, 0);
            if (i >= 0){
                EXPECT_EQ(skeleton.bones[(size_t)i].parent, p) << proximal;
            }
            EXPECT_EQ(proximal.find("4"), std::string::npos);
        }
    }
    std::string error;
    EXPECT_TRUE(validateExtraBones(skeleton, map, extra, &error)) << error;
    // a finger under the wrong hand is refused
    HumanExtraMap wrong = extra;
    std::swap(wrong.bones[(size_t)extraIndex(HumanExtraBone::LeftIndexProximal)], wrong.bones[(size_t)extraIndex(HumanExtraBone::RightIndexProximal)]);
    if (!wrong.bones[(size_t)extraIndex(HumanExtraBone::LeftIndexProximal)].empty()){
        EXPECT_FALSE(validateExtraBones(skeleton, map, wrong, &error));
    }
}
