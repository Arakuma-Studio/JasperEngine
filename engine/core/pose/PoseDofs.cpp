// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/PoseDofs.h"

namespace jasperengine::pose {

    namespace {

        using A = CanonicalAxis;

        struct Table {
            BoneDofInfo bones[HUMAN_BONE_COUNT];
            Table();
        };

        // centre (spine chain): bend L+, side F-, twist U+
        BoneDofInfo centre(const char* dof1, const char* dof3, float bendMin, float bendMax,
                           float sideMin, float sideMax, float twistMin, float twistMax){
            return BoneDofInfo{Vector3(0, 1, 0), {
                {dof1, A::L, 1.0f, bendMin, bendMax},
                {"side", A::F, -1.0f, sideMin, sideMax},
                {dof3, A::U, 1.0f, twistMin, twistMax}}};
        }

        // the mirror of a left-side bone: same names and limits, L component of the direction
        // negated, signs of U/F-axis DOFs negated
        BoneDofInfo mirrored(const BoneDofInfo& left){
            BoneDofInfo right = left;
            right.neutralDirection.x = -left.neutralDirection.x;
            for (DofInfo& dof : right.dofs){
                if (dof.axis != A::L){
                    dof.sign = -dof.sign;
                }
            }
            return right;
        }

        // spec: pose animation phase 1, tables 4.5 (axes, signs) and joint limits
        Table::Table(){
            using B = HumanBone;
            auto set = [this](B bone, const BoneDofInfo& info){ bones[boneIndex(bone)] = info; };

            set(B::Hips,       centre("lean", "turn", -180, 180, -180, 180, -180, 180));
            set(B::Spine,      centre("bend", "twist", -25, 35, -25, 25, -25, 25));
            set(B::Chest,      centre("bend", "twist", -25, 35, -25, 25, -25, 25));
            set(B::UpperChest, centre("bend", "twist", -25, 35, -25, 25, -25, 25));
            set(B::Neck,       centre("bend", "twist", -40, 45, -35, 35, -55, 55));
            set(B::Head,       centre("bend", "twist", -40, 45, -35, 35, -55, 55));

            const BoneDofInfo shoulder{Vector3(1, 0, 0), {
                {"raise", A::F, 1.0f, -15, 35},
                {"forward", A::U, -1.0f, -20, 25},
                {"twist", A::L, 1.0f, -10, 10}}};
            const BoneDofInfo upperArm{Vector3(0, -1, 0), {
                {"raise", A::F, 1.0f, -40, 175},
                {"forward", A::L, -1.0f, -70, 175},
                {"twist", A::U, 1.0f, -95, 95}}};
            const BoneDofInfo lowerArm{Vector3(0, -1, 0), {
                {"bend", A::L, -1.0f, 0, 155},
                {"side", A::F, 1.0f, -12, 12},
                {"twist", A::U, 1.0f, -90, 90}}};
            const BoneDofInfo hand{Vector3(0, -1, 0), {
                {"bend", A::F, -1.0f, -75, 80},
                {"side", A::L, -1.0f, -25, 35},
                {"twist", A::U, 1.0f, -15, 15}}};
            const BoneDofInfo upperLeg{Vector3(0, -1, 0), {
                {"forward", A::L, -1.0f, -35, 125},
                {"out", A::F, 1.0f, -35, 70},
                {"twist", A::U, 1.0f, -45, 50}}};
            const BoneDofInfo lowerLeg{Vector3(0, -1, 0), {
                {"bend", A::L, 1.0f, 0, 155},
                {"side", A::F, 1.0f, -8, 8},
                {"twist", A::U, 1.0f, -12, 12}}};
            const BoneDofInfo foot{Vector3(0, 0, 1), {
                {"point", A::L, 1.0f, -35, 55},
                {"side", A::U, 1.0f, -25, 35},
                {"roll", A::F, -1.0f, -25, 30}}};
            const BoneDofInfo toes{Vector3(0, 0, 1), {
                {"bend", A::L, 1.0f, -60, 45},
                {"side", A::U, 1.0f, -6, 6},
                {"twist", A::F, -1.0f, -6, 6}}};

            set(B::LeftShoulder, shoulder);   set(B::RightShoulder, mirrored(shoulder));
            set(B::LeftUpperArm, upperArm);   set(B::RightUpperArm, mirrored(upperArm));
            set(B::LeftLowerArm, lowerArm);   set(B::RightLowerArm, mirrored(lowerArm));
            set(B::LeftHand, hand);           set(B::RightHand, mirrored(hand));
            set(B::LeftUpperLeg, upperLeg);   set(B::RightUpperLeg, mirrored(upperLeg));
            set(B::LeftLowerLeg, lowerLeg);   set(B::RightLowerLeg, mirrored(lowerLeg));
            set(B::LeftFoot, foot);           set(B::RightFoot, mirrored(foot));
            set(B::LeftToes, toes);           set(B::RightToes, mirrored(toes));
        }

        const Table& table(){
            static const Table instance;
            return instance;
        }

    }

    const BoneDofInfo& boneDofInfo(HumanBone bone){
        return table().bones[boneIndex(bone)];
    }

    const DofInfo& dofInfo(int dof){
        return table().bones[dof / DOFS_PER_BONE].dofs[dof % DOFS_PER_BONE];
    }

    std::string dofName(int dof){
        return std::string(humanBoneName(dofBone(dof))) + "." + dofInfo(dof).name;
    }

    bool dofFromName(const std::string& name, int& dof){
        const size_t dot = name.find('.');
        if (dot == std::string::npos){
            return false;
        }
        HumanBone bone;
        if (!humanBoneFromName(name.substr(0, dot), bone)){
            return false;
        }
        const std::string part = name.substr(dot + 1);
        const BoneDofInfo& info = boneDofInfo(bone);
        for (int k = 0; k < DOFS_PER_BONE; k++){
            if (part == info.dofs[k].name){
                dof = dofIndex(bone, k);
                return true;
            }
        }
        return false;
    }

    Vector3 canonicalAxis(CanonicalAxis axis){
        switch (axis){
            case CanonicalAxis::L: return Vector3(1, 0, 0);
            case CanonicalAxis::U: return Vector3(0, 1, 0);
            case CanonicalAxis::F: return Vector3(0, 0, 1);
        }
        return Vector3(0, 0, 0);
    }

}
