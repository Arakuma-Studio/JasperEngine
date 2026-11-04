// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PICKGEOMETRY_H
#define PICKGEOMETRY_H

#include "Export.h"
#include "math/Vector3.h"
#include "render/Render.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine {

    struct MeshComponent;
    class Buffer;

    // The triangles a mesh draws, in its local space, as the picking BVH takes them: the
    // positions of every vertex source its submeshes read, and a triangle list over them.
    // Built on the main thread (gather) from what RenderSystem binds: each submesh's index
    // range at LOD0 (never Submesh::lodIndices), 16-bit indices widened, strips expanded into
    // their triangles (alternate ones flipped, so every triangle keeps the strip's facing), and
    // non-indexed submeshes as consecutive vertices. A corner past its source's vertices makes
    // the triangle degenerate (all corners vertex 0), never dropped: triangle numbers stay
    // those of the draw. POINTS and LINES submeshes have no triangles.
    struct JASPERENGINE_API PickGeometry {
        std::vector<Vector3> positions;
        std::vector<uint32_t> indices;              // 3 per triangle
        // per submesh its first triangle, plus the total at the end (size = submeshes + 1)
        std::vector<uint32_t> submeshFirstTriangle;

        uint32_t getTriangleCount() const { return (uint32_t)(indices.size() / 3); }
        uint32_t getSubmeshCount() const { return submeshFirstTriangle.empty() ? 0u : (uint32_t)submeshFirstTriangle.size() - 1u; }
        uint32_t submeshOf(uint32_t triangle) const;
        size_t memoryBytes() const;
        void clear();
    };

    // Where the triangles of one submesh come from (the buffers RenderSystem binds for it)
    struct PickSubmeshSource {
        const Buffer* positionBuffer = nullptr;
        size_t positionOffset = 0;      // bytes, of vertex 0
        uint32_t positionStride = 0;
        uint32_t vertexCount = 0;       // vertices the source holds
        const Buffer* indexBuffer = nullptr;   // null: non-indexed
        size_t indexOffset = 0;         // bytes
        AttributeDataType indexType = AttributeDataType::UNSIGNED_INT;
        uint32_t drawCount = 0;         // indices (indexed) or vertices drawn
        PrimitiveType primitive = PrimitiveType::TRIANGLES;
        bool valid = false;             // float positions found
        uint32_t base = 0;              // where its vertices start in PickGeometry::positions
    };

    // The buffers and the triangle layout behind a gather, valid while the mesh is unchanged
    // (same frame, main thread): posing reads the skin attributes through it, a hit reads its
    // corners' normals.
    struct PickGeometryLayout {
        // one per submesh (a tilemap has one per tile rect)
        std::vector<PickSubmeshSource> submeshes;
        bool complete = false;          // every triangle submesh had float positions
    };

    namespace picking {

        // The triangles of a submesh drawn with `count` elements
        inline uint32_t primitiveTriangles(PrimitiveType primitive, uint32_t count){
            if (primitive == PrimitiveType::TRIANGLES) return count / 3u;
            if (primitive == PrimitiveType::TRIANGLE_STRIP) return count >= 3u ? count - 2u : 0u;
            return 0u;
        }

        // The sources of the mesh's submeshes (no copy). False when the mesh has no CPU float
        // positions to pick (complete is false then: it picks by its bounds).
        JASPERENGINE_API bool resolveSources(const MeshComponent& mesh, PickGeometryLayout& layout);
        // Triangles the mesh draws, from its sources (no copy)
        JASPERENGINE_API uint32_t countTriangles(const PickGeometryLayout& layout);

        // Fills `geometry` (cleared first) from the mesh; false without CPU float positions
        JASPERENGINE_API bool gather(const MeshComponent& mesh, PickGeometry& geometry, PickGeometryLayout* layout = nullptr);

        // The source vertices of a triangle (in its submesh's source, before `base`), as the
        // gather numbers them; false for a triangle past the mesh's
        JASPERENGINE_API bool triangleCorners(const PickGeometryLayout& layout, uint32_t triangle, uint32_t& submesh, uint32_t corners[3]);

        // 64-bit content hash of what the gather reads (index ranges, position sources and
        // primitive types of every submesh), without copying it. 0 is never returned.
        JASPERENGINE_API uint64_t hashSources(const PickGeometryLayout& layout);
        // Seeded word hash of bytes (the hash above, also used for pose epochs)
        JASPERENGINE_API uint64_t hashBytes(const void* data, size_t size, uint64_t seed);

    }

}

#endif // PICKGEOMETRY_H
