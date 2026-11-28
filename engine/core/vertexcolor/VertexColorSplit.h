// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R11, R12: what the render path binds for one target. Without corner
// overrides: one value per GPU vertex (shared geometry). With overrides: split vertices, i.e. the
// original vertices followed by one copy per extra distinct (vertex, value) pair, and remapped index
// lists for LOD0 and every LOD level (the LOD rule of R12). Jobs-safe, no GPU.

#include "Export.h"
#include "vertexcolor/VertexColorMesh.h"
#include "vertexcolor/VertexColorTypes.h"

#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine::vertexcolor {

    struct SplitResult {
        bool split = false;                       // false: use `values` with the shared buffers
        // per output vertex (original count + added): its color value
        std::vector<Vector4> values;
        // per output vertex: the GPU vertex it copies (identity for the first vertexCount)
        std::vector<uint32_t> sourceVertex;
        // LOD0 triangle list over output vertices (empty when !split)
        std::vector<uint32_t> indices;
        // one list per entry of lodIndexLists, over output vertices (empty when !split)
        std::vector<std::vector<uint32_t>> lodIndices;
        uint32_t added = 0;                       // split_vertices (R12, R124)
    };

    // lodIndexLists: the LOD1+ triangle lists of the primitive over the same GPU vertices (S54)
    JASPERENGINE_API SplitResult buildSplit(const TargetData& data, const MeshView& mesh,
                                            std::span<const std::vector<uint32_t>> lodIndexLists);

    // the value bytes for the GPU stream of `values` in `format` (RGBA8: 4 B, RGBA16F: 8 B per vertex)
    JASPERENGINE_API std::vector<uint8_t> packValues(std::span<const Vector4> values, Format format);

}
