// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LATTICE_SOLVER_H
#define LATTICE_SOLVER_H

// The lattice math (spec §3). includes/lattice.glsl implements the same formulas on the
// GPU; a mismatch between the two is a bug.

#include "Engine.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "math/Matrix3.h"
#include "math/Matrix4.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"
#include <array>
#include <vector>

namespace jasperengine{ namespace lattice{

    // clamps each axis to [LATTICE_MIN_RESOLUTION, LATTICE_MAX_RESOLUTION]; returns rx*ry*rz
    JASPERENGINE_API int pointCount(int rx, int ry, int rz);
    JASPERENGINE_API int pointIndex(int rx, int ry, int i, int j, int k);
    // local-space rest position of point (i, j, k)
    JASPERENGINE_API Vector3 restPosition(int rx, int ry, int rz, int i, int j, int k);

    // spec §3.1; dynamicOffsets may be null or empty, else it is added to offsets
    JASPERENGINE_API void buildPaddedGrid(int rx, int ry, int rz, const std::vector<Vector3>& offsets,
                                          const std::vector<Vector3>* dynamicOffsets, std::vector<Vector3>& padded);
    // convenience: padded grid of a component (offsets + dynamicOffsets)
    JASPERENGINE_API void buildPaddedGrid(const LatticeComponent& lattice, std::vector<Vector3>& padded);

    struct JASPERENGINE_API Evaluation{
        Vector3 delta;    // offset added in normalized space
        Matrix3 jacobian; // d(u + delta)/du
        float stretch;    // det(jacobian)
    };

    // spec §3.2-3.4 at normalized coordinates u; weight is the binding weight times the vertex mask.
    // A disabled lattice, a padded grid of the wrong size or a NaN u give the rest evaluation
    // (delta 0, identity, stretch 1). Allocation free.
    JASPERENGINE_API Evaluation evaluate(const LatticeComponent& lattice, const std::vector<Vector3>& padded,
                                         const Vector3& u, float weight);

    // world point -> deformed world point through one lattice (latticeModel = its Transform::modelMatrix);
    // a lattice with a non-invertible model matrix leaves the point unchanged
    JASPERENGINE_API Vector3 deformWorldPoint(const LatticeComponent& lattice, const std::vector<Vector3>& padded,
                                              const Matrix4& latticeModel, const Vector3& worldPoint, float weight = 1.0f);
    // world-space Jacobian of deformWorldPoint at worldPoint (followers): Lm * J * Lm^-1 with Lm the
    // linear part of latticeModel; identity for a non-invertible model matrix
    JASPERENGINE_API Matrix3 worldJacobian(const LatticeComponent& lattice, const std::vector<Vector3>& padded,
                                           const Matrix4& latticeModel, const Vector3& worldPoint, float weight = 1.0f);

    // object space -> normalized: translate(0.5) * inverse(latticeModel) * objectModel (spec §5.3);
    // all NaN (isValid() false) when latticeModel is not invertible
    JASPERENGINE_API Matrix4 objectToNormalized(const Matrix4& latticeModel, const Matrix4& objectModel);

    struct JASPERENGINE_API ObjectSpaceLattice{
        const LatticeComponent* lattice = nullptr;
        const std::vector<Vector3>* padded = nullptr;
        Matrix4 objectToNormalized;
        Matrix4 normalizedToObject;
        float weight = 1.0f;
        LatticeMaskChannel mask = LatticeMaskChannel::NONE;
    };

    // one vertex through one binding (spec §3.4); normal, tangent, stretch, color may be null.
    // stretch is multiplied (stacked lattices multiply). color is RGBA in 0..1; without it the mask is 1.
    // A vertex the binding does not move is left bit-identical (its normal is not renormalized).
    JASPERENGINE_API void deformVertex(const ObjectSpaceLattice& binding, Vector3& position, Vector3* normal,
                                       Vector3* tangent, float* stretch, const Vector4* color);

    // largest |P| over the padded grid (bounds, spec §5.5)
    JASPERENGINE_API float maxPaddedOffset(const std::vector<Vector3>& padded);

    // cells (i, j, k), i in [0, rx-2], whose Jacobian determinant (weight 1, offsets + dynamicOffsets)
    // is <= 1e-6 at a corner or the centre (folded inside out or flattened); ignores `enabled`
    JASPERENGINE_API std::vector<std::array<int, 3>> findFoldedCells(const LatticeComponent& lattice);

}}

#endif //LATTICE_SOLVER_H
