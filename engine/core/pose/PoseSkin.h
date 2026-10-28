// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A CPU skinning sample of a character mesh, used for the exact lowest point of a pose (spec:
// pose animation phase 1, section 4.10). Phase 2 builds it from the engine's mesh buffers.

#ifndef POSE_SKIN_H
#define POSE_SKIN_H

#include "Export.h"
#include "math/Matrix4.h"
#include "math/Vector3.h"
#include "pose/PoseMath.h"

#include <array>
#include <cstddef>
#include <vector>

namespace jasperengine::pose {

    struct PoseSkinVertex {
        Vector3 position;                            // bind-pose (mesh) position
        std::array<int, 4> bones{{-1, -1, -1, -1}};  // skeleton bone indices, -1 = unused
        std::array<float, 4> weights{{0, 0, 0, 0}};
    };

    class JASPERENGINE_API PoseSkin {
    public:
        static constexpr size_t MAX_VERTICES = 6000;

        std::vector<PoseSkinVertex> vertices;   // at most MAX_VERTICES
        std::vector<Matrix4> inverseBind;       // per skeleton bone (identity for bones that skin nothing)

        // The deterministic sampling stride over `totalVertices` skinned vertices (all skinned
        // meshes concatenated): take every stride-th vertex starting at 0, at most MAX_VERTICES
        static size_t sampleStride(size_t totalVertices);

        // Lowest up (model +Y) component over the samples of sum w_j * (World_j * InvBind_j) * v,
        // `worldMatrices` per skeleton bone in model space (PoseRig::computeWorldMatrices).
        // Weights are used as given (not renormalised). Returns +infinity when there are no
        // samples.
        float lowest(const std::vector<Matrix4>& worldMatrices) const;
        // the same in double precision from double world transforms (what PoseRig uses)
        double lowest(const std::vector<Affine3d>& world) const;
    };

}

#endif // POSE_SKIN_H
