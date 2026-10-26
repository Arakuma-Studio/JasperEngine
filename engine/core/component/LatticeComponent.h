// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LATTICE_COMPONENT_H
#define LATTICE_COMPONENT_H

#include "Engine.h"
#include "math/Vector3.h"
#include "action/Ease.h"
#include <string>
#include <vector>

namespace jasperengine{

    // How the offset field is interpolated between points (spec §3.2).
    enum class LatticeInterpolation{
        SHARP,  // trilinear: passes through the points, visible seams between cells
        SMOOTH, // trilinear with smoothstep weights: no seams, softer than cubic
        CUBIC   // uniform cubic B-spline: smoothest, follows the cage without passing through inner points
    };

    // What happens to vertices outside the cage (spec §3.3).
    enum class LatticeOutside{
        EXTEND, // the border deformation continues outward
        FADE    // fades to nothing over fadeDistance (local units)
    };

    // Face that stays still while jiggling (spec §6.2).
    enum class LatticeJiggleAnchor{
        NONE,
        NEGATIVE_X,
        POSITIVE_X,
        NEGATIVE_Y,
        POSITIVE_Y,
        NEGATIVE_Z,
        POSITIVE_Z
    };

    struct JASPERENGINE_API LatticePose{
        std::string name;
        std::vector<Vector3> offsets; // same layout as LatticeComponent::offsets
    };

    // A box-shaped cage of control points (spec §2.1). Local space is the unit cube
    // [-0.5, 0.5]^3, sized and placed by the entity's Transform. Point (i, j, k) has flat
    // index i + resolutionX * (j + resolutionY * k) and rest position
    // (i/(rx-1) - 0.5, j/(ry-1) - 0.5, k/(rz-1) - 0.5).
    struct JASPERENGINE_API LatticeComponent{
        bool enabled = true;

        int resolutionX = 3; // points per axis, 2..16
        int resolutionY = 3;
        int resolutionZ = 3;

        // local-space offsets from the rest positions, size resolutionX * resolutionY * resolutionZ
        std::vector<Vector3> offsets = std::vector<Vector3>(27, Vector3(0, 0, 0));

        LatticeInterpolation interpolation = LatticeInterpolation::CUBIC;
        LatticeOutside outside = LatticeOutside::FADE;
        float fadeDistance = 0.25f;

        std::vector<LatticePose> poses;

        bool jiggle = false;
        float jiggleStiffness = 60.0f;
        float jiggleDamping = 6.0f;
        float jiggleInertia = 1.0f;
        LatticeJiggleAnchor jiggleAnchor = LatticeJiggleAnchor::NONE;
        float jiggleMaxOffset = 0.5f;

        // --- runtime only (not serialized) ---

        // jiggle displacement added on top of offsets when packing (empty = none)
        std::vector<Vector3> dynamicOffsets;
        std::vector<Vector3> jiggleVelocities;
        Vector3 jiggleLastPosition = Vector3(0, 0, 0);
        Vector3 jiggleLastVelocity = Vector3(0, 0, 0);
        int jiggleWarmup = 0;
        bool jiggleSleeping = true;

        // pose tween state (LatticeSystem)
        std::vector<Vector3> tweenFrom;
        std::vector<Vector3> tweenTo;
        float tweenTime = 0.0f;
        float tweenDuration = 0.0f;
        EaseType tweenEase = EaseType::LINEAR;
        bool tweening = false;

        // bumped by anything that changes offsets, resolution or settings, so packing can skip
        // unchanged lattices (it also compares texels, this only short-circuits)
        uint32_t revision = 0;
    };

}

#endif //LATTICE_COMPONENT_H
