// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R50-R54: element selection maths shared by the editor's Select tool,
// MCP (sample_mesh_colors select, selection targets) and Lua. Ids are sorted and unique.
// Vertex ids are logical vertices; Face ids triangles; Corner ids triangle * 3 + k.

#include "Export.h"
#include "vertexcolor/VertexColorMesh.h"
#include "vertexcolor/VertexColorOps.h"
#include "vertexcolor/VertexColorTypes.h"

#include <cstdint>
#include <vector>

namespace jasperengine::vertexcolor {

    struct Selection {
        ElementKind kind = ElementKind::Face;
        std::vector<uint32_t> ids;
        bool empty() const { return ids.empty(); }
    };

    JASPERENGINE_API Selection selectAll(const MeshView& mesh, ElementKind kind);
    JASPERENGINE_API Selection invert(const MeshView& mesh, const Selection& selection);
    // ids of another kind: Vertex -> Face (faces with every vertex selected; anyVertex: with one),
    // Face -> Vertex (their vertices), Face -> Corner, Corner -> Face (faces with a selected corner), ...
    JASPERENGINE_API Selection convert(const MeshView& mesh, const Selection& selection, ElementKind kind, bool anyVertex = false);
    // one ring of neighbours through edges (logical vertices / edge-adjacent faces)
    JASPERENGINE_API Selection grow(const MeshView& mesh, const Selection& selection);
    JASPERENGINE_API Selection shrink(const MeshView& mesh, const Selection& selection);
    // everything connected to the selection (across seams)
    JASPERENGINE_API Selection linked(const MeshView& mesh, const Selection& selection);
    // faces connected to `seed` through edges whose face normals differ by at most `angle` degrees
    JASPERENGINE_API Selection flatRegion(const MeshView& mesh, uint32_t seedTriangle, float angle);
    // elements whose value matches (Vertex: the logical vertex's corners all match; Face: coverage)
    JASPERENGINE_API Selection similarColor(const TargetData& data, const MeshView& mesh, ElementKind kind, const ColorMatch& match);
    // faces (or vertices) whose normal is within `angle` degrees of `direction` (local space)
    JASPERENGINE_API Selection byNormal(const MeshView& mesh, ElementKind kind, const Vector3& direction, float angle);
    JASPERENGINE_API Selection byChannelRange(const TargetData& data, const MeshView& mesh, ElementKind kind, int channel, float min, float max);
    // set operations (same kind)
    JASPERENGINE_API Selection unite(const Selection& a, const Selection& b);
    JASPERENGINE_API Selection subtract(const Selection& a, const Selection& b);

    // the average value of the selection's corners (R53 Copy Color)
    JASPERENGINE_API Vector4 averageValue(const TargetData& data, const MeshView& mesh, const Selection& selection);

    // a Target naming the selection's ids (what applyOps takes)
    JASPERENGINE_API Target toTarget(const Selection& selection);

}
