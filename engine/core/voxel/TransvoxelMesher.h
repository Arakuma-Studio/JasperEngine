// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TRANSVOXEL_MESHER_H
#define TRANSVOXEL_MESHER_H

// R9-R14, V5: Lengyel's modified Marching Cubes and transition cells over one block. Pure and
// thread-safe: the output is plain arrays with fixed-point positions, computed once, so a vertex
// two blocks share is bit-identical in both (R11).

#include "Export.h"
#include "VoxelCoverage.h"
#include "VoxelStore.h"
#include "VoxelTypes.h"

#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine::voxel {

    // Fixed-point positions are in 1/256 voxel, in the volume's frame (V5)
    inline constexpr int FIXED_ONE = 256;

    // One function, so equal inputs give equal bits on the GPU too (V5)
    inline float fixedToLocal(int32_t fixed, float voxelSize) {
        return static_cast<float>(fixed) * voxelSize / static_cast<float>(FIXED_ONE);
    }

    // R49: the vertex layout, 32 bytes. Upload converts position in place through fixedToLocal.
    struct MeshVertex {
        int32_t position[3];
        float normal[3];
        uint16_t slots;      // four 4-bit slots, the first one in the low nibble
        uint16_t spare;
        uint8_t weights[4];  // over `slots`, sum 255
    };
    static_assert(sizeof(MeshVertex) == 32, "R49: the vertex is 32 bytes");

    enum class MeshMode : uint8_t { Full, CollisionOnly };

    struct MeshRequest {
        BlockKey key;
        uint8_t sideMask = 0;              // bit per Side: that neighbour is one level coarser
        MeshMode mode = MeshMode::Full;
        const Coverage* coverage = nullptr;
    };

    struct MeshOut {
        std::vector<MeshVertex> vertices;
        std::vector<uint16_t> indices;
        int32_t boundsMin[3] = {0, 0, 0};
        int32_t boundsMax[3] = {0, 0, 0};
        int triangleCount = 0;
        bool usedBlockMaterialSet = false;   // R14's fallback ran
        void clear();
        bool empty() const { return indices.empty(); }
        size_t byteSize() const { return vertices.size() * sizeof(MeshVertex) + indices.size() * sizeof(uint16_t); }
    };

    // Test option of R14: a lower limit than 65,535 forces the fallback on an ordinary block
    struct MeshOptions {
        int maxVerticesPerCellSets = 65535;
    };

    // Pure and thread-safe. Returns false only for a key outside the volume.
    JASPERENGINE_API bool meshBlock(const ReadView& view, const MeshRequest& request, MeshOut& out, const MeshOptions& options = {});

    struct AuditResult {
        bool watertight = true;
        int64_t unbalancedEdges = 0;
        int64_t edgesChecked = 0;
    };

    // V6: welds by fixed-point position and requires every directed edge to balance with its reverse.
    // Degenerate triangles preserve the balance. outerShell non-null (an Open volume): edges lying in
    // that box's outer planes, in fixed point, are not checked.
    JASPERENGINE_API AuditResult auditWatertight(std::span<const MeshOut* const> meshes, const IntBox* outerShell = nullptr);

}

#endif
