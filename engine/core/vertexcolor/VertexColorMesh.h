// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R4, D5, D6: the geometry an op works on, in the primitive's local
// space and bind pose (D12), with the derived logical vertices, adjacency and the uniform grid.
// Built once per mesh load (editor: on a job when a tool arms, R96) and shared read-only.

#include "Export.h"
#include "math/AABB.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "vertexcolor/VertexColorTypes.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::vertexcolor {

    struct JASPERENGINE_API MeshView {
        // ---- inputs (per GPU vertex unless said)
        std::vector<Vector3> positions;
        std::vector<Vector3> normals;          // empty: face normals are used
        std::vector<Vector2> uv0;              // empty when absent
        std::vector<Vector2> uv1;
        std::vector<uint32_t> indices;         // triangle list
        std::vector<Vector4> assetColors;      // the mesh's own COLOR_0, linear; empty when none

        // ---- derived by build()
        // GPU vertex -> logical vertex (R4: same position within 1e-6 of the bounds diagonal)
        std::vector<uint32_t> logical;
        // logical -> its GPU vertices (CSR: members[memberStart[l] .. memberStart[l + 1]])
        std::vector<uint32_t> memberStart;
        std::vector<uint32_t> members;
        // GPU vertex -> the corners using it (CSR)
        std::vector<uint32_t> vertexCornerStart;
        std::vector<uint32_t> vertexCorners;
        // logical -> edge-adjacent logical vertices (CSR), across seams
        std::vector<uint32_t> neighborStart;
        std::vector<uint32_t> neighbors;
        // triangle -> triangles sharing an edge through logical vertices (CSR)
        std::vector<uint32_t> faceNeighborStart;
        std::vector<uint32_t> faceNeighbors;
        std::vector<Vector3> faceNormals;      // per triangle, unit (zero for degenerate)
        std::vector<float> faceAreas;
        std::vector<Vector3> logicalPositions;
        std::vector<Vector3> logicalNormals;   // corner-angle-weighted average of the member normals, unit
        AABB bounds;

        // derives everything above from the inputs; false (with `error`) on inconsistent inputs
        // (index out of range, index count not a multiple of 3, normals/uv/color size mismatch)
        bool build(std::string* error = nullptr);
        bool built() const { return !logical.empty() || positions.empty(); }

        uint32_t vertexCount() const { return (uint32_t)positions.size(); }
        uint32_t triangleCount() const { return (uint32_t)(indices.size() / 3); }
        uint32_t logicalCount() const { return memberStart.empty() ? 0u : (uint32_t)(memberStart.size() - 1); }

        // logical vertices whose position is within `radius` of `center` (local space), appended to out
        void queryLogical(const Vector3& center, float radius, std::vector<uint32_t>& out) const;
        // logical vertices inside an AABB (local space), appended to out
        void queryLogicalBox(const AABB& box, std::vector<uint32_t>& out) const;

        // the R9 fingerprint of this geometry
        Fingerprint fingerprint() const;

    private:
        // the uniform grid over logical vertices (D6)
        Vector3 gridOrigin;
        float gridCell = 1.0f;
        uint32_t gridDim[3] = {0, 0, 0};
        std::vector<uint32_t> gridStart;
        std::vector<uint32_t> gridItems;
        void buildGrid();
    };

    // the value interpolated at a point of a triangle (R108): corner values, barycentric weights
    JASPERENGINE_API Vector4 valueAt(const TargetData& data, const MeshView& mesh, uint32_t triangle, const Vector3& barycentric);

}
