// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Fixture rigs of the pose core tests (spec: pose animation phase 1, section 5), built in C++:
// - tposeRig(): Mixamo names, glTF axes (+Z forward, left = +X), an "Armature" root scaled 0.01
//   (bone translations in centimetres), Mixamo-style fingers and end bones
// - aposeRig(): Blender names, arms 45 degrees down, different proportions, an "Armature" root
//   rotated -90 degrees about X (Z-up inside), Blender-style bone rolls, twist bones between
//   slots and a 4-bone spine
// - minimalRig(): Unity names without chest, upper chest, neck, shoulders and toes, with a
//   non-slot bone standing in for the chest
// - backwardRig(): VRM names, the T-pose layout turned to face -Z
// - heroFixtureSkeleton() / robotFixtureSkeleton(): the exact bone lists of the generated
//   fixture models (tools/fixtures/pose-animation, hero_tpose.gltf / robot_apose.gltf)
// Each rig gets a synthetic PoseSkin (a few rigid vertices per bone, sole vertices at y = 0).

#ifndef POSE_TEST_RIGS_H
#define POSE_TEST_RIGS_H

#include "pose/HumanoidMap.h"
#include "pose/PoseMath.h"
#include "pose/PoseRig.h"
#include "pose/PoseSkin.h"

#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace posetest {

    using namespace jasperengine;
    using namespace jasperengine::pose;

    // Builds a skeleton from model-space rest positions/rotations (uniform scales)
    class SkeletonBuilder {
    public:
        int add(const std::string& name, const std::string& parent, const Vec3d& worldPos, const Quatd& worldRot, double localScale = 1.0){
            int p = -1;
            if (!parent.empty()){
                auto it = index.find(parent);
                if (it == index.end()){
                    throw std::runtime_error("unknown parent " + parent);
                }
                p = it->second;
            }
            const Vec3d parentPos = p >= 0 ? pos[p] : Vec3d();
            const Quatd parentRot = p >= 0 ? rot[p] : Quatd();
            const double parentScale = p >= 0 ? scale[p] : 1.0;
            SkeletonBone bone;
            bone.name = name;
            bone.parent = p;
            bone.position = (parentRot.inverse() * (worldPos - parentPos) / parentScale).toVector3();
            bone.rotation = (parentRot.inverse() * worldRot).normalized().toQuaternion();
            bone.scale = Vector3(static_cast<float>(localScale), static_cast<float>(localScale), static_cast<float>(localScale));
            skeleton.bones.push_back(bone);
            pos.push_back(worldPos);
            rot.push_back(worldRot);
            scale.push_back(parentScale * localScale);
            index[name] = static_cast<int>(skeleton.bones.size()) - 1;
            return index[name];
        }

        Skeleton skeleton;
        std::vector<Vec3d> pos;
        std::vector<Quatd> rot;
        std::vector<double> scale;
        std::map<std::string, int> index;
    };

    // a bone frame with local +Y along `dir`, rolled about it
    inline Quatd aim(const Vec3d& dir, double roll){
        return axisAngleRad(dir, roll) * fromToRotation(Vec3d(0, 1, 0), dir);
    }

    // a centre bone frame: a rotation about X is its own mirror image, so the rig stays symmetric
    inline Quatd centre(double angle){
        return axisAngleRad(Vec3d(1, 0, 0), angle);
    }

    // the reflection of a rotation across the YZ plane (left <-> right)
    inline Quatd mirrorRot(const Quatd& q){ return Quatd(q.w, q.x, -q.y, -q.z); }
    inline Vec3d mirrorPos(const Vec3d& p){ return Vec3d(-p.x, p.y, p.z); }

    // Adds a left/right symmetric chain: `bones` = {name suffix, parent suffix, left position};
    // names are prefixL + suffix (+ suffixL) / prefixR + suffix (+ suffixR); rotations aim at
    // the next position of `aimAt` (or along `endDir`)
    struct SidedBone {
        std::string name;        // full left name
        std::string rightName;   // full right name
        std::string parentLeft;  // full parent names ("" -> same for both: centreParent)
        std::string parentRight;
        Vec3d position;          // left side, model space
        Vec3d aimDir;            // direction of local +Y (left side)
        double roll;
    };

    inline void addSided(SkeletonBuilder& b, const std::vector<SidedBone>& bones, const std::function<Vec3d(const Vec3d&)>& placePos = nullptr,
                         const std::function<Quatd(const Quatd&)>& placeRot = nullptr){
        auto pp = [&](const Vec3d& p){ return placePos ? placePos(p) : p; };
        auto pr = [&](const Quatd& q){ return placeRot ? placeRot(q) : q; };
        for (const SidedBone& s : bones){
            const Quatd left = aim(s.aimDir, s.roll);
            b.add(s.name, s.parentLeft, pp(s.position), pr(left));
        }
        for (const SidedBone& s : bones){
            const Quatd left = aim(s.aimDir, s.roll);
            b.add(s.rightName, s.parentRight, pp(mirrorPos(s.position)), pr(mirrorRot(left)));
        }
    }

    inline Vec3d dirTo(const Vec3d& a, const Vec3d& b){ return (b - a).normalized(); }

    // ---- skins ----

    inline Matrix4 toMatrix4(const Affine3d& a){
        Matrix4 m;
        for (int r = 0; r < 3; r++){
            for (int c = 0; c < 3; c++){
                m.set(c, r, static_cast<float>(a.m[r][c]));
            }
        }
        m.set(3, 0, static_cast<float>(a.t.x));
        m.set(3, 1, static_cast<float>(a.t.y));
        m.set(3, 2, static_cast<float>(a.t.z));
        return m;
    }

    inline Affine3d inverseAffine(const Affine3d& a){
        Affine3d inv;
        const Vec3d c0 = a.solveLinear(Vec3d(1, 0, 0));
        const Vec3d c1 = a.solveLinear(Vec3d(0, 1, 0));
        const Vec3d c2 = a.solveLinear(Vec3d(0, 0, 1));
        for (int r = 0; r < 3; r++){
            inv.m[r][0] = r == 0 ? c0.x : (r == 1 ? c0.y : c0.z);
            inv.m[r][1] = r == 0 ? c1.x : (r == 1 ? c1.y : c1.z);
            inv.m[r][2] = r == 0 ? c2.x : (r == 1 ? c2.y : c2.z);
        }
        inv.t = -inv.applyLinear(a.t);
        return inv;
    }

    // A synthetic skin: rigid vertices along every slot bone's segments, sole vertices at y = 0
    // under the feet (heel on the foot, ball blended foot/toes, tip on the toes)
    inline std::shared_ptr<PoseSkin> makeSkin(const PoseRig& rig){
        auto skin = std::make_shared<PoseSkin>();
        std::vector<Quatd> rot;
        std::vector<Affine3d> world;
        rig.computeWorldPrecise(rig.restPose(), rot, world);
        const Skeleton& skel = rig.skeleton();
        for (const Affine3d& w : world){
            skin->inverseBind.push_back(toMatrix4(inverseAffine(w)));
        }
        auto vertex = [&](const Vec3d& p, int b0, float w0, int b1 = -1, float w1 = 0.0f){
            PoseSkinVertex v;
            v.position = p.toVector3();
            v.bones = {{b0, b1, -1, -1}};
            v.weights = {{w0, w1, 0.0f, 0.0f}};
            skin->vertices.push_back(v);
        };
        auto isFootOrToes = [&](int bone){
            const int slot = rig.slotOfBone(bone);
            return slot == boneIndex(HumanBone::LeftFoot) || slot == boneIndex(HumanBone::RightFoot) ||
                   slot == boneIndex(HumanBone::LeftToes) || slot == boneIndex(HumanBone::RightToes);
        };
        for (size_t i = 0; i < skel.bones.size(); i++){
            if (rig.slotOfBone(static_cast<int>(i)) < 0 || isFootOrToes(static_cast<int>(i))){
                continue;
            }
            for (size_t j = 0; j < skel.bones.size(); j++){
                if (skel.bones[j].parent != static_cast<int>(i)){
                    continue;
                }
                const Vec3d a = world[i].t;
                const Vec3d c = world[j].t;
                Vec3d side = (c - a).cross(Vec3d(0, 0, 1));
                if (side.length() < 1e-9) side = (c - a).cross(Vec3d(1, 0, 0));
                side = side.normalized() * 0.02;
                for (double t : {0.25, 0.5, 0.75}){
                    const Vec3d p = a + (c - a) * t;
                    vertex(p + side, static_cast<int>(i), 1.0f);
                    vertex(p - side, static_cast<int>(i), 1.0f);
                }
            }
        }
        for (HumanBone footSlot : {HumanBone::LeftFoot, HumanBone::RightFoot}){
            const HumanBone toesSlot = footSlot == HumanBone::LeftFoot ? HumanBone::LeftToes : HumanBone::RightToes;
            const int foot = rig.boneIndex(footSlot);
            const int toes = rig.boneIndex(toesSlot);
            const Vec3d f = world[foot].t;
            const Vec3d forward(rig.characterForward());
            const Vec3d ball = toes >= 0 ? world[toes].t : f + forward * 0.12;
            const Vec3d left(rig.characterLeft());
            for (double s : {-0.04, 0.04}){
                vertex(Vec3d(f.x, 0.0, f.z) - forward * 0.05 + left * s, foot, 1.0f);
                if (toes >= 0){
                    vertex(Vec3d(ball.x, 0.0, ball.z) + left * s, foot, 0.5f, toes, 0.5f);
                    vertex(Vec3d(ball.x, 0.0, ball.z) + forward * 0.07 + left * s, toes, 1.0f);
                }else{
                    vertex(Vec3d(ball.x, 0.0, ball.z) + left * s, foot, 1.0f);
                }
            }
            vertex(f + Vec3d(0, 0.04, 0), foot, 1.0f);
        }
        return skin;
    }

    // ---- rigs ----

    struct TestRig {
        std::string label;
        Skeleton skeleton;
        HumanoidMap expected;          // what auto-detect must find
        std::shared_ptr<PoseRig> rig;  // built from auto-detect, no skin
        std::shared_ptr<PoseRig> skinned;   // the same with makeSkin
        std::shared_ptr<PoseSkin> skin;
    };

    inline HumanoidMap mapOf(std::initializer_list<std::pair<HumanBone, std::string>> entries){
        HumanoidMap map;
        for (const auto& e : entries){
            map.bones[boneIndex(e.first)] = e.second;
        }
        return map;
    }

    inline TestRig finish(const std::string& label, const Skeleton& skeleton, const HumanoidMap& expected){
        TestRig t;
        t.label = label;
        t.skeleton = skeleton;
        t.expected = expected;
        std::string error;
        t.rig = std::make_shared<PoseRig>();
        if (!t.rig->build(skeleton, autoDetectHumanoid(skeleton), &error)){
            throw std::runtime_error(label + ": " + error);
        }
        t.skin = makeSkin(*t.rig);
        t.skinned = std::make_shared<PoseRig>();
        t.skinned->setSkin(t.skin);
        if (!t.skinned->build(skeleton, autoDetectHumanoid(skeleton), &error)){
            throw std::runtime_error(label + ": " + error);
        }
        return t;
    }

    // T-pose layout (left side), metres: shared by the Mixamo and the VRM rig
    struct TPoseLayout {
        Vec3d hips{0, 0.95, 0}, spine{0, 1.05, 0}, spine1{0, 1.17, 0}, spine2{0, 1.30, 0}, neck{0, 1.48, 0}, head{0, 1.56, 0}, headTop{0, 1.75, 0};
        Vec3d shoulder{0.06, 1.42, 0}, arm{0.18, 1.42, 0}, foreArm{0.46, 1.42, 0}, hand{0.72, 1.42, 0};
        Vec3d upLeg{0.09, 0.90, 0}, leg{0.09, 0.50, 0.01}, foot{0.09, 0.08, -0.02}, toeBase{0.09, 0.02, 0.11}, toeEnd{0.09, 0.02, 0.19};
    };

    inline Skeleton buildTPoseSkeleton(bool vrm){
        const TPoseLayout L;
        SkeletonBuilder b;
        // the VRM rig faces -Z: everything turned 180 degrees about Y
        const Quatd turn = vrm ? axisAngleRad(Vec3d(0, 1, 0), POSE_PI) : Quatd();
        auto P = [&](const Vec3d& p){ return turn * p; };
        auto R = [&](const Quatd& q){ return (turn * q).normalized(); };
        auto n = [&](const std::string& mixamo, const std::string& vrmName){ return vrm ? vrmName : "mixamorig:" + mixamo; };

        std::string root;
        if (!vrm){
            b.add("Armature", "", Vec3d(), Quatd(), 0.01);
            root = "Armature";
        }
        b.add(n("Hips", "J_Bip_C_Hips"), root, P(L.hips), R(centre(0.2)));
        b.add(n("Spine", "J_Bip_C_Spine"), n("Hips", "J_Bip_C_Hips"), P(L.spine), R(centre(-0.1)));
        b.add(n("Spine1", "J_Bip_C_Chest"), n("Spine", "J_Bip_C_Spine"), P(L.spine1), R(centre(0.15)));
        b.add(n("Spine2", "J_Bip_C_UpperChest"), n("Spine1", "J_Bip_C_Chest"), P(L.spine2), R(centre(-0.3)));
        b.add(n("Neck", "J_Bip_C_Neck"), n("Spine2", "J_Bip_C_UpperChest"), P(L.neck), R(centre(0.3)));
        b.add(n("Head", "J_Bip_C_Head"), n("Neck", "J_Bip_C_Neck"), P(L.head), R(centre(-0.25)));
        b.add(n("HeadTop_End", "J_Adj_C_HeadTop_End"), n("Head", "J_Bip_C_Head"), P(L.headTop), R(Quatd()));

        auto sided = [&](const std::string& mixamoPart, const std::string& vrmPart, bool left){
            if (vrm) return std::string("J_Bip_") + (left ? "L_" : "R_") + vrmPart;
            return "mixamorig:" + std::string(left ? "Left" : "Right") + mixamoPart;
        };
        const std::string chestTop = n("Spine2", "J_Bip_C_UpperChest");
        const std::string hips = n("Hips", "J_Bip_C_Hips");
        struct Part { std::string mixamo, vrmPart, parentMixamo, parentVrm; Vec3d pos, next; double roll; bool centreParent; };
        const std::vector<Part> parts = {
            {"Shoulder", "Shoulder", "", "", L.shoulder, L.arm, 0.4, true},
            {"Arm", "UpperArm", "Shoulder", "Shoulder", L.arm, L.foreArm, -0.3, false},
            {"ForeArm", "LowerArm", "Arm", "UpperArm", L.foreArm, L.hand, 0.5, false},
            {"Hand", "Hand", "ForeArm", "LowerArm", L.hand, L.hand + Vec3d(0.1, 0, 0), 0.1, false},
            {"UpLeg", "UpperLeg", "", "", L.upLeg, L.leg, 0.25, true},
            {"Leg", "LowerLeg", "UpLeg", "UpperLeg", L.leg, L.foot, -0.2, false},
            {"Foot", "Foot", "Leg", "LowerLeg", L.foot, L.toeBase, 0.35, false},
            {"ToeBase", "ToeBase", "Foot", "Foot", L.toeBase, L.toeEnd, 0.0, false},
        };
        for (int side = 0; side < 2; side++){
            const bool left = side == 0;
            for (const Part& part : parts){
                std::string parent = part.centreParent ? (part.mixamo == "Shoulder" ? chestTop : hips) : sided(part.parentMixamo, part.parentVrm, left);
                const Quatd leftRot = aim(dirTo(part.pos, part.next), part.roll);
                b.add(sided(part.mixamo, part.vrmPart, left), parent, P(left ? part.pos : mirrorPos(part.pos)), R(left ? leftRot : mirrorRot(leftRot)));
            }
            // end bones and fingers (never mapped)
            const std::string foot = sided("ToeBase", "ToeBase", left);
            b.add(vrm ? std::string("J_Bip_") + (left ? "L" : "R") + "_ToeBase_end" : "mixamorig:" + std::string(left ? "Left" : "Right") + "Toe_End",
                  foot, P(left ? L.toeEnd : mirrorPos(L.toeEnd)), R(Quatd()));
            const std::string hand = sided("Hand", "Hand", left);
            const char* fingers[] = {"Thumb", "Index", "Middle"};
            const Vec3d starts[] = {{0.745, 1.41, 0.03}, {0.81, 1.42, 0.025}, {0.815, 1.42, 0.0}};
            for (int f = 0; f < 3; f++){
                std::string parent = hand;
                for (int j = 1; j <= 3; j++){
                    const Vec3d p = starts[f] + Vec3d(0.035 * (j - 1), 0, 0);
                    const std::string name = vrm ? std::string("J_Bip_") + (left ? "L_" : "R_") + fingers[f] + std::to_string(j)
                                                 : "mixamorig:" + std::string(left ? "Left" : "Right") + "Hand" + fingers[f] + std::to_string(j);
                    b.add(name, parent, P(left ? p : mirrorPos(p)), R(Quatd()));
                    parent = name;
                }
            }
        }
        return b.skeleton;
    }

    inline HumanoidMap tposeExpected(bool vrm){
        using H = HumanBone;
        auto n = [&](const std::string& mixamo, const std::string& vrmName){ return vrm ? vrmName : "mixamorig:" + mixamo; };
        return mapOf({
            {H::Hips, n("Hips", "J_Bip_C_Hips")}, {H::Spine, n("Spine", "J_Bip_C_Spine")}, {H::Chest, n("Spine1", "J_Bip_C_Chest")},
            {H::UpperChest, n("Spine2", "J_Bip_C_UpperChest")}, {H::Neck, n("Neck", "J_Bip_C_Neck")}, {H::Head, n("Head", "J_Bip_C_Head")},
            {H::LeftShoulder, n("LeftShoulder", "J_Bip_L_Shoulder")}, {H::LeftUpperArm, n("LeftArm", "J_Bip_L_UpperArm")},
            {H::LeftLowerArm, n("LeftForeArm", "J_Bip_L_LowerArm")}, {H::LeftHand, n("LeftHand", "J_Bip_L_Hand")},
            {H::RightShoulder, n("RightShoulder", "J_Bip_R_Shoulder")}, {H::RightUpperArm, n("RightArm", "J_Bip_R_UpperArm")},
            {H::RightLowerArm, n("RightForeArm", "J_Bip_R_LowerArm")}, {H::RightHand, n("RightHand", "J_Bip_R_Hand")},
            {H::LeftUpperLeg, n("LeftUpLeg", "J_Bip_L_UpperLeg")}, {H::LeftLowerLeg, n("LeftLeg", "J_Bip_L_LowerLeg")},
            {H::LeftFoot, n("LeftFoot", "J_Bip_L_Foot")}, {H::LeftToes, n("LeftToeBase", "J_Bip_L_ToeBase")},
            {H::RightUpperLeg, n("RightUpLeg", "J_Bip_R_UpperLeg")}, {H::RightLowerLeg, n("RightLeg", "J_Bip_R_LowerLeg")},
            {H::RightFoot, n("RightFoot", "J_Bip_R_Foot")}, {H::RightToes, n("RightToeBase", "J_Bip_R_ToeBase")},
        });
    }

    inline const TestRig& tposeRig(){
        static const TestRig rig = finish("tpose-mixamo", buildTPoseSkeleton(false), tposeExpected(false));
        return rig;
    }

    inline const TestRig& backwardRig(){
        static const TestRig rig = finish("backward-vrm", buildTPoseSkeleton(true), tposeExpected(true));
        return rig;
    }

    inline const TestRig& aposeRig(){
        static const TestRig rig = [](){
            SkeletonBuilder b;
            // Blender armature: Z-up inside, turned to Y-up by the root
            b.add("Armature", "", Vec3d(), axisAngleRad(Vec3d(1, 0, 0), -POSE_PI / 2));
            const Vec3d pelvis(0, 0.88, 0.01), spine(0, 0.98, 0), spine1(0, 1.09, 0.005), spine2(0, 1.20, 0.01), spine3(0, 1.31, 0.005);
            const Vec3d neck(0, 1.44, 0.0), head(0, 1.52, 0.01), headTop(0, 1.71, 0.01);
            b.add("pelvis", "Armature", pelvis, aim(dirTo(pelvis, spine), 1.0));
            b.add("spine", "pelvis", spine, aim(dirTo(spine, spine1), 0.7));
            b.add("spine.001", "spine", spine1, aim(dirTo(spine1, spine2), -0.4));
            b.add("spine.002", "spine.001", spine2, aim(dirTo(spine2, spine3), 0.2));
            b.add("spine.003", "spine.002", spine3, aim(dirTo(spine3, neck), -0.6));
            b.add("neck", "spine.003", neck, aim(dirTo(neck, head), 0.9));
            b.add("head", "neck", head, aim(dirTo(head, headTop), -1.1));
            b.add("head_end", "head", headTop, Quatd());   // excluded ("end")
            b.add("jaw", "head", Vec3d(0, 1.50, 0.06), aim(Vec3d(0, -0.3, 1).normalized(), 0.0));   // extra non-slot bone

            const double s45 = std::sqrt(0.5);
            const Vec3d shoulder(0.05, 1.38, 0.0), upperArm(0.17, 1.38, -0.01);
            const Vec3d armDir(s45, -s45, 0);
            const Vec3d elbow = upperArm + armDir * 0.30, wrist = elbow + armDir * 0.27;
            const Vec3d thigh(0.10, 0.84, 0.0), knee(0.12, 0.46, 0.012), ankle(0.14, 0.08, -0.02), toe(0.14, 0.02, 0.10), toeTip(0.14, 0.02, 0.17);
            std::vector<SidedBone> bones = {
                {"shoulder.L", "shoulder.R", "spine.003", "spine.003", shoulder, dirTo(shoulder, upperArm), 0.8},
                {"upper_arm.L", "upper_arm.R", "shoulder.L", "shoulder.R", upperArm, armDir, -0.9},
                {"upper_arm_twist.L", "upper_arm_twist.R", "upper_arm.L", "upper_arm.R", (upperArm + elbow) * 0.5, armDir, -0.5},
                {"forearm.L", "forearm.R", "upper_arm_twist.L", "upper_arm_twist.R", elbow, armDir, 1.2},
                {"forearm_twist.L", "forearm_twist.R", "forearm.L", "forearm.R", (elbow + wrist) * 0.5, armDir, 1.4},
                {"hand.L", "hand.R", "forearm_twist.L", "forearm_twist.R", wrist, armDir, 0.3},
                {"f_middle.01.L", "f_middle.01.R", "hand.L", "hand.R", wrist + armDir * 0.09, armDir, 0.3},
                {"thigh.L", "thigh.R", "pelvis", "pelvis", thigh, dirTo(thigh, knee), 2.5},
                {"thigh_twist.L", "thigh_twist.R", "thigh.L", "thigh.R", (thigh + knee) * 0.5, dirTo(thigh, knee), 2.6},
                {"shin.L", "shin.R", "thigh_twist.L", "thigh_twist.R", knee, dirTo(knee, ankle), -2.8},
                {"foot.L", "foot.R", "shin.L", "shin.R", ankle, dirTo(ankle, toe), 0.4},
                {"toe.L", "toe.R", "foot.L", "foot.R", toe, dirTo(toe, toeTip), -0.2},
                {"toe_end.L", "toe_end.R", "toe.L", "toe.R", toeTip, dirTo(toe, toeTip), 0.0},
            };
            addSided(b, bones);
            using H = HumanBone;
            const HumanoidMap expected = mapOf({
                {H::Hips, "pelvis"}, {H::Spine, "spine"}, {H::Chest, "spine.002"}, {H::UpperChest, "spine.003"}, {H::Neck, "neck"}, {H::Head, "head"},
                {H::LeftShoulder, "shoulder.L"}, {H::LeftUpperArm, "upper_arm.L"}, {H::LeftLowerArm, "forearm.L"}, {H::LeftHand, "hand.L"},
                {H::RightShoulder, "shoulder.R"}, {H::RightUpperArm, "upper_arm.R"}, {H::RightLowerArm, "forearm.R"}, {H::RightHand, "hand.R"},
                {H::LeftUpperLeg, "thigh.L"}, {H::LeftLowerLeg, "shin.L"}, {H::LeftFoot, "foot.L"}, {H::LeftToes, "toe.L"},
                {H::RightUpperLeg, "thigh.R"}, {H::RightLowerLeg, "shin.R"}, {H::RightFoot, "foot.R"}, {H::RightToes, "toe.R"},
            });
            return finish("apose-blender", b.skeleton, expected);
        }();
        return rig;
    }

    inline const TestRig& minimalRig(){
        static const TestRig rig = [](){
            SkeletonBuilder b;
            const Vec3d hips(0, 1.0, 0), spine(0, 1.12, 0), torso(0, 1.35, 0), head(0, 1.55, 0), headTop(0, 1.75, 0);
            b.add("Hips", "", hips, aim(dirTo(hips, spine), 0.0));
            b.add("Spine", "Hips", spine, aim(dirTo(spine, torso), 0.3));
            b.add("Bone.003", "Spine", torso, aim(dirTo(torso, head), -0.2));   // non-slot stand-in for the chest
            b.add("Head", "Bone.003", head, aim(dirTo(head, headTop), 0.1));
            b.add("HeadTop", "Head", headTop, Quatd());
            const Vec3d upperArm(0.17, 1.45, 0), lowerArm(0.44, 1.45, 0), hand(0.70, 1.45, 0), handTip(0.78, 1.45, 0);
            const Vec3d upperLeg(0.10, 0.95, 0), lowerLeg(0.10, 0.52, 0.01), foot(0.10, 0.08, -0.02), footTip(0.10, 0.02, 0.15);
            std::vector<SidedBone> bones = {
                {"LeftUpperArm", "RightUpperArm", "Bone.003", "Bone.003", upperArm, dirTo(upperArm, lowerArm), 0.2},
                {"LeftLowerArm", "RightLowerArm", "LeftUpperArm", "RightUpperArm", lowerArm, dirTo(lowerArm, hand), -0.4},
                {"LeftHand", "RightHand", "LeftLowerArm", "RightLowerArm", hand, dirTo(hand, handTip), 0.6},
                {"LeftHandTip", "RightHandTip", "LeftHand", "RightHand", handTip, dirTo(hand, handTip), 0.0},
                {"LeftUpperLeg", "RightUpperLeg", "Hips", "Hips", upperLeg, dirTo(upperLeg, lowerLeg), 0.1},
                {"LeftLowerLeg", "RightLowerLeg", "LeftUpperLeg", "RightUpperLeg", lowerLeg, dirTo(lowerLeg, foot), -0.3},
                {"LeftFoot", "RightFoot", "LeftLowerLeg", "RightLowerLeg", foot, dirTo(foot, footTip), 0.2},
                {"LeftFootTip", "RightFootTip", "LeftFoot", "RightFoot", footTip, dirTo(foot, footTip), 0.0},
            };
            addSided(b, bones);
            using H = HumanBone;
            const HumanoidMap expected = mapOf({
                {H::Hips, "Hips"}, {H::Spine, "Spine"}, {H::Head, "Head"},
                {H::LeftUpperArm, "LeftUpperArm"}, {H::LeftLowerArm, "LeftLowerArm"}, {H::LeftHand, "LeftHand"},
                {H::RightUpperArm, "RightUpperArm"}, {H::RightLowerArm, "RightLowerArm"}, {H::RightHand, "RightHand"},
                {H::LeftUpperLeg, "LeftUpperLeg"}, {H::LeftLowerLeg, "LeftLowerLeg"}, {H::LeftFoot, "LeftFoot"},
                {H::RightUpperLeg, "RightUpperLeg"}, {H::RightLowerLeg, "RightLowerLeg"}, {H::RightFoot, "RightFoot"},
            });
            return finish("minimal-unity", b.skeleton, expected);
        }();
        return rig;
    }

    inline std::vector<const TestRig*> allRigs(){
        return {&tposeRig(), &aposeRig(), &minimalRig(), &backwardRig()};
    }

    // ---- name-list skeletons (hierarchy only matters for auto-detect) ----

    // {name, parent name}; positions are spread so the skeleton is not degenerate
    inline Skeleton skeletonFromList(const std::vector<std::pair<std::string, std::string>>& bones){
        Skeleton skeleton;
        std::map<std::string, int> index;
        for (const auto& entry : bones){
            SkeletonBone bone;
            bone.name = entry.first;
            bone.parent = entry.second.empty() ? -1 : index.at(entry.second);
            bone.position = Vector3(0, 0.1f, 0);
            skeleton.bones.push_back(bone);
            index[entry.first] = static_cast<int>(skeleton.bones.size()) - 1;
        }
        return skeleton;
    }

    // hero_tpose.gltf (A3 fixture report): 43 bones, depth first
    inline std::vector<std::pair<std::string, std::string>> heroFixtureBones(){
        std::vector<std::pair<std::string, std::string>> b;
        const std::string m = "mixamorig:";
        b.push_back({m + "Hips", ""});
        b.push_back({m + "Spine", m + "Hips"});
        b.push_back({m + "Spine1", m + "Spine"});
        b.push_back({m + "Spine2", m + "Spine1"});
        b.push_back({m + "Neck", m + "Spine2"});
        b.push_back({m + "Head", m + "Neck"});
        b.push_back({m + "HeadTop_End", m + "Head"});
        for (const char* side : {"Left", "Right"}){
            const std::string s = m + side;
            b.push_back({s + "Shoulder", m + "Spine2"});
            b.push_back({s + "Arm", s + "Shoulder"});
            b.push_back({s + "ForeArm", s + "Arm"});
            b.push_back({s + "Hand", s + "ForeArm"});
            for (const char* finger : {"Thumb", "Index", "Middle"}){
                std::string parent = s + "Hand";
                for (int j = 1; j <= 3; j++){
                    const std::string name = s + "Hand" + finger + std::to_string(j);
                    b.push_back({name, parent});
                    parent = name;
                }
            }
        }
        for (const char* side : {"Left", "Right"}){
            const std::string s = m + side;
            b.push_back({s + "UpLeg", m + "Hips"});
            b.push_back({s + "Leg", s + "UpLeg"});
            b.push_back({s + "Foot", s + "Leg"});
            b.push_back({s + "ToeBase", s + "Foot"});
            b.push_back({s + "Toe_End", s + "ToeBase"});
        }
        return b;
    }

    // robot_apose.gltf (A3 fixture report): 23 bones
    inline std::vector<std::pair<std::string, std::string>> robotFixtureBones(){
        std::vector<std::pair<std::string, std::string>> b = {
            {"pelvis", ""}, {"spine", "pelvis"}, {"spine.001", "spine"}, {"neck", "spine.001"}, {"head", "neck"},
        };
        for (const char* side : {"L", "R"}){
            const std::string s = std::string(".") + side;
            b.push_back({"shoulder" + s, "spine.001"});
            b.push_back({"upper_arm" + s, "shoulder" + s});
            b.push_back({"upper_arm_twist" + s, "upper_arm" + s});
            b.push_back({"forearm" + s, "upper_arm_twist" + s});
            b.push_back({"hand" + s, "forearm" + s});
        }
        for (const char* side : {"L", "R"}){
            const std::string s = std::string(".") + side;
            b.push_back({"thigh" + s, "pelvis"});
            b.push_back({"shin" + s, "thigh" + s});
            b.push_back({"foot" + s, "shin" + s});
            b.push_back({"toe" + s, "foot" + s});
        }
        return b;
    }

}

#endif // POSE_TEST_RIGS_H
