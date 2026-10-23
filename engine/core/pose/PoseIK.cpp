// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/PoseIK.h"

#include "pose/PoseMath.h"

#include <cmath>
#include <vector>

namespace jasperengine::pose {

    void limbBones(Limb limb, HumanBone& upper, HumanBone& lower, HumanBone& end){
        switch (limb){
            case Limb::LeftArm:  upper = HumanBone::LeftUpperArm;  lower = HumanBone::LeftLowerArm;  end = HumanBone::LeftHand; break;
            case Limb::RightArm: upper = HumanBone::RightUpperArm; lower = HumanBone::RightLowerArm; end = HumanBone::RightHand; break;
            case Limb::LeftLeg:  upper = HumanBone::LeftUpperLeg;  lower = HumanBone::LeftLowerLeg;  end = HumanBone::LeftFoot; break;
            case Limb::RightLeg: upper = HumanBone::RightUpperLeg; lower = HumanBone::RightLowerLeg; end = HumanBone::RightFoot; break;
        }
    }

    namespace {

        // the rotation taking the orthonormal pair (u, n) to (u2, n2)
        Quatd frameRotation(const Vec3d& u, const Vec3d& n, const Vec3d& u2, const Vec3d& n2){
            const Vec3d w = u.cross(n), w2 = u2.cross(n2);
            // R = F2 * F1^T with frames as columns
            double m[3][3];
            const Vec3d f1[3] = {u, n, w}, f2[3] = {u2, n2, w2};
            for (int r = 0; r < 3; r++){
                for (int col = 0; col < 3; col++){
                    double sum = 0.0;
                    for (int k = 0; k < 3; k++){
                        const double f2r = r == 0 ? f2[k].x : (r == 1 ? f2[k].y : f2[k].z);
                        const double f1c = col == 0 ? f1[k].x : (col == 1 ? f1[k].y : f1[k].z);
                        sum += f2r * f1c;
                    }
                    m[r][col] = sum;
                }
            }
            const double trace = m[0][0] + m[1][1] + m[2][2];
            Quatd q;
            if (trace > 0.0){
                const double s = std::sqrt(trace + 1.0) * 2.0;
                q = Quatd(0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s);
            }else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]){
                const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
                q = Quatd((m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s);
            }else if (m[1][1] > m[2][2]){
                const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
                q = Quatd((m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s);
            }else{
                const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
                q = Quatd((m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s);
            }
            return q.normalized();
        }

        // the bend plane normal the pole gives a limb along `dir` (unit): the pole is the
        // character's forward for legs (knees forward) and backward for arms (elbows back); when
        // the limb points along it, up for legs and down for arms
        Vec3d limbPoleNormal(const PoseRig& rig, Limb limb, const Vec3d& dir){
            const bool leg = limb == Limb::LeftLeg || limb == Limb::RightLeg;
            const Vec3d forward(rig.characterForward());
            const Vec3d up(rig.characterUp());
            Vec3d pole = leg ? forward : -forward;
            Vec3d normal = dir.cross(pole);
            if (normal.length() < 0.2){
                // blend in the secondary pole as the limb turns towards the main one
                const Vec3d secondary = leg ? up : -up;
                const double w = smoothstep(0.2, 0.05, normal.length());
                pole = (pole * (1.0 - w) + secondary * w).normalized();
                normal = dir.cross(pole);
            }
            if (normal.length() < 1e-9){
                normal = dir.cross(Vec3d(rig.characterLeft()));
            }
            return normal.normalized();
        }

    }

    bool solveLimbIK(const PoseRig& rig, HumanPose& pose, Limb limb, const Vector3& targetModel, bool keepEndWorldRotation){
        if (!rig.valid()){
            return false;
        }
        HumanBone upper = HumanBone::Count, lower = HumanBone::Count, end = HumanBone::Count;
        limbBones(limb, upper, lower, end);
        const int ia = rig.boneIndex(upper);
        const int ib = rig.boneIndex(lower);
        const int ic = rig.boneIndex(end);

        std::vector<Quatd> rot;
        std::vector<Affine3d> world;
        rig.computeWorldPrecise(pose, rot, world);

        const Vec3d a = world[ia].t;
        const Vec3d b = world[ib].t;
        const Vec3d c = world[ic].t;
        const Vec3d target(targetModel);
        const double lab = (b - a).length();
        const double lbc = (c - b).length();
        if (lab < 1e-12 || lbc < 1e-12){
            return false;
        }

        const double margin = 1e-4 * rig.humanScale();
        const double dist = (target - a).length();
        const double minReach = std::fabs(lab - lbc) + margin;
        const double maxReach = std::max(minReach, lab + lbc - margin);
        const double reach = clampd(dist, minReach, maxReach);

        // bend plane normal (n = (C - A) x (B - A), so n x (C - A) points to the joint's side):
        // the pole's for a (nearly) straight limb, the current one for a clearly bent limb, a
        // smooth blend in between. The current plane's weight also fades as that plane turns
        // away from the pole plane past 90 degrees (fully the pole's when opposite), so a slight
        // bend away from the pole side (float noise of a "straight" rest or neutral limb) takes
        // the pole's side, continuously; above BENT the limb keeps its own plane whatever its side.
        const Vec3d ac = c - a;
        const Vec3d ab = b - a;
        const Vec3d aim = dist > 1e-12 ? (target - a) / dist : ac.normalized();
        const Vec3d current = ac.cross(ab).normalized();
        const double bend = ac.cross(ab).length() / std::max(ac.length() * lab, 1e-300);   // sine of the angle at A
        const Vec3d poleNormal = limbPoleNormal(rig, limb, ac.length() > 1e-12 ? ac.normalized() : aim);
        const double STRAIGHT = 0.02, BENT = 0.1;   // sines: ~1.1 and ~5.7 degrees
        double w = 1.0;
        if (bend < BENT){
            w = smoothstep(STRAIGHT, BENT, bend) * smoothstep(-1.0, 0.0, current.dot(poleNormal));
        }
        Vec3d normal = (current * w + poleNormal * (1.0 - w)).normalized();
        if (normal.length() < 0.5){
            normal = poleNormal;
        }

        // the solved triangle: C* on the target line at the clamped reach, B* on the normal's
        // side. The plane follows the limb by the shortest rotation from C - A to T - A (as
        // fromTo(C - A, T - A) turns it), so it never spins when the target crosses the normal.
        Vec3d newNormal = fromToRotation(ac.length() > 1e-12 ? ac : aim, aim) * normal;
        newNormal = newNormal - aim * newNormal.dot(aim);
        if (newNormal.length() < 1e-9){
            newNormal = limbPoleNormal(rig, limb, aim);
        }
        newNormal = newNormal.normalized();
        const Vec3d side = newNormal.cross(aim);
        const double cosA = clampd((lab * lab + reach * reach - lbc * lbc) / (2.0 * lab * reach), -1.0, 1.0);
        const Vec3d newB = a + (aim * cosA + side * std::sqrt(std::max(0.0, 1.0 - cosA * cosA))) * lab;
        const Vec3d newC = a + aim * reach;

        // A: (B - A, normal) -> (B* - A, new normal); then B: shortest arc onto C* - B*
        Vec3d oldNormal = normal - ab.normalized() * normal.dot(ab.normalized());
        if (oldNormal.length() < 1e-9){
            oldNormal = newNormal - ab.normalized() * newNormal.dot(ab.normalized());
        }
        const Quatd aimDelta = frameRotation(ab.normalized(), oldNormal.normalized(), (newB - a).normalized(), newNormal);
        const Quatd bendDelta = fromToRotation(aimDelta * (c - b), newC - newB);

        auto parentRot = [&](int bone){
            const int parent = rig.skeleton().bones[bone].parent;
            return parent >= 0 ? rot[parent] : Quatd();
        };
        // world rotations: A' = aim * A, B' = bend * aim * B; A's parent is unchanged and B's
        // parent (A or a rest bone under it) turned with aim
        const Quatd localA = (parentRot(ia).inverse() * aimDelta * rot[ia]).normalized();
        const Quatd localB = (parentRot(ib).inverse() * aimDelta.inverse() * bendDelta * aimDelta * rot[ib]).normalized();
        pose.rotations[boneIndex(upper)] = localA.toQuaternion();
        pose.rotations[boneIndex(lower)] = localB.toQuaternion();
        if (keepEndWorldRotation){
            // C's parent turned with bend * aim: undo it
            const Quatd localC = (parentRot(ic).inverse() * (bendDelta * aimDelta).inverse() * rot[ic]).normalized();
            pose.rotations[boneIndex(end)] = localC.toQuaternion();
        }
        return true;
    }

    void moveHipsKeepFeet(const PoseRig& rig, HumanPose& pose, const Vector3& newHipsWorldModel){
        if (!rig.valid()){
            return;
        }
        std::vector<Quaternion> rot;
        std::vector<Vector3> pos;
        rig.computeWorld(pose, rot, pos);
        const Vector3 leftFoot = pos[rig.boneIndex(HumanBone::LeftFoot)];
        const Vector3 rightFoot = pos[rig.boneIndex(HumanBone::RightFoot)];
        rig.setHipsWorldPosition(pose, newHipsWorldModel);
        solveLimbIK(rig, pose, Limb::LeftLeg, leftFoot, true);
        solveLimbIK(rig, pose, Limb::RightLeg, rightFoot, true);
    }

    float lowestPoint(const PoseRig& rig, const HumanPose& pose){
        return static_cast<float>(rig.lowestPointPrecise(pose, rig.skin()));
    }

    void ground(const PoseRig& rig, HumanPose& pose, float lift){
        rig.groundPose(pose, lift, rig.skin());
    }

}
