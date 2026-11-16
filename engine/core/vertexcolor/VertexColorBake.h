// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R41-R43, R60: the generators that compute a value field from the
// geometry (or a texture, or another mesh). applyOps (VertexColorOps.cpp) calls these, then writes the
// field into the op's target with its channels, blend and strength, like any other op. Internal to the
// vertexcolor module; tests call them directly.

#include "vertexcolor/VertexColorOps.h"

#include <string>
#include <vector>

namespace jasperengine::vertexcolor::bake {

    // a value per GPU vertex, and optionally per corner (perCorner: cornerValues has one entry per
    // triangle corner, indices.size(); the op engine then writes corners where a logical vertex's
    // corners disagree, R41 keepSeams)
    struct Field {
        std::vector<Vector4> vertexValues;
        std::vector<Vector4> cornerValues;
        bool perCorner = false;
        // the corner values are exact (transfer): written as they are, never clustered by ΔE (R41)
        bool exactCorners = false;
        // which GPU vertices / corners have a value (transfer may leave some unmatched: Unmatched::Keep)
        std::vector<uint8_t> valid;    // per GPU vertex; empty: all valid
    };

    // R41: op.image sampled over each corner's area (op.samples stratified bilinear samples, UV set
    // op.uvSet). Fills result.meanDeltaE / p95DeltaE (texture vs stored field) and underSampledFaces.
    JASPERENGINE_API bool fromTexture(const MeshView& mesh, const Op& op, const ApplyOptions& options,
                                      Field& out, OpResult& result, std::string& error);

    // R42: 1 - occlusion (thickness: along -normal), grayscale in all four components. Self occlusion
    // through a BVH of the mesh; op.sceneScope adds options.sceneOccluded (world space).
    JASPERENGINE_API bool ambientOcclusion(const MeshView& mesh, const Op& op, const ApplyOptions& options, bool thickness,
                                           Field& out, std::string& error);

    // R43: signed convexity remapped to 0..1 (0.5 flat), blurred op.iterations rings
    JASPERENGINE_API bool curvature(const MeshView& mesh, const Op& op, Field& out, std::string& error);

    // R60: nearest point of op.source (world space) within op.maxDistance and op.normalAngle.
    // Fills result.unmatched and meanDistance; `valid` marks matched vertices. A source with corner
    // values gives each corner the value of a source corner at the same place whose triangle holds
    // the corner's side (the one nearest a point inside the corner's triangle), so hard edges carry
    // over (exactCorners); its vertex's value when no source vertex is in reach.
    JASPERENGINE_API bool transfer(const MeshView& mesh, const Op& op, const ApplyOptions& options,
                                   Field& out, OpResult& result, std::string& error);

}
