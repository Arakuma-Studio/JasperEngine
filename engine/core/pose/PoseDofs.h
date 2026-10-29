// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The joint-angle degrees of freedom (spec: pose animation phase 1, section 4.5): per slot, the
// neutral bone direction, two swing DOFs and one twist DOF about canonical character axes, their
// sign conventions and their joint limits. Right-side slots are derived from the left ones (same
// names and limits, L component of the direction negated, signs of U/F-axis DOFs negated), so a
// left and a right bone with the same values are exact mirror images.

#ifndef POSE_DOFS_H
#define POSE_DOFS_H

#include "Export.h"
#include "math/Vector3.h"
#include "pose/HumanBones.h"
#include "pose/PoseTypes.h"

#include <string>

namespace jasperengine::pose {

    // Canonical character frame: L = +X (character's left), U = +Y (up), F = +Z (forward)
    enum class CanonicalAxis : uint8_t { L = 0, U, F };

    struct DofInfo {
        const char* name;       // e.g. "raise"; the full DOF name is "<slot>.<name>"
        CanonicalAxis axis;     // swing component axis (dofs 0, 1) or twist axis (dof 2)
        float sign;             // +1 or -1: value = sign * angle about the positive axis
        float minDegrees;       // joint limits
        float maxDegrees;
    };

    struct BoneDofInfo {
        Vector3 neutralDirection;   // canonical direction of the bone in the neutral standing pose
        DofInfo dofs[DOFS_PER_BONE];   // [0], [1] swing components, [2] twist about neutralDirection's axis
    };

    JASPERENGINE_API const BoneDofInfo& boneDofInfo(HumanBone bone);
    JASPERENGINE_API const DofInfo& dofInfo(int dof);              // dof = bone * 3 + k
    JASPERENGINE_API std::string dofName(int dof);                 // "left_upper_arm.raise"
    JASPERENGINE_API bool dofFromName(const std::string& name, int& dof);
    JASPERENGINE_API Vector3 canonicalAxis(CanonicalAxis axis);    // unit vector

    inline int dofIndex(HumanBone bone, int k){ return boneIndex(bone) * DOFS_PER_BONE + k; }
    inline HumanBone dofBone(int dof){ return boneAt(dof / DOFS_PER_BONE); }

}

#endif // POSE_DOFS_H
