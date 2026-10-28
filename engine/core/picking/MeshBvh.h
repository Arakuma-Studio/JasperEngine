// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The build (binned-SAH binary tree collapsed into an 8-wide tree with 8-bit quantized child
// boxes), the traversal and the serialized layout are adapted from OpenEngine's MeshBvh
// (Engine/Modules/MeshPicking/Include/MeshPicking/MeshBvh.h, Source/MeshBvh.cpp), Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License (NOTICE.md). Changed here: the
// SIMD path is chosen at run time (MeshBvhSimd_*.cpp), the decode and slab test are the same
// arithmetic on every path (bit-identical hit sets), quantization is verified conservative,
// triangles are culled per submesh, there is an all-hits traversal and a refit, and the build
// is iterative with a bounded depth.

#ifndef MESHBVH_H
#define MESHBVH_H

#include "Export.h"
#include "math/AABB.h"
#include "math/Vector3.h"
#include "picking/PickGeometry.h"

#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine {

    // Which side of a triangle a ray may hit: the faces its submesh draws
    enum class PickFaces : uint8_t {
        BOTH,   // double-sided (culling off), or a query that hits backfaces
        FRONT,  // back faces culled: only the side the triangle's winding faces
        BACK    // front faces culled
    };

    // A ray in the mesh's own space. t counts in units of `direction` (any length).
    struct MeshBvhRay {
        Vector3 origin;
        Vector3 direction;
        // hits past tMax are not reported
        float tMax = 3.402823466e+38f;
        // per submesh; a submesh past its end (or an empty span) is BOTH. FRONT is the side
        // the counter-clockwise winding faces (a caller mirrors it for CW meshes and
        // negative-determinant transforms)
        std::span<const PickFaces> faces;
    };

    struct MeshBvhHit {
        uint32_t triangle = 0;   // PickGeometry's triangle index
        uint32_t submesh = 0;
        float t = 0.0f;          // along MeshBvhRay::direction
        float u = 0.0f;          // barycentrics of corners 1 and 2 (corner 0 is 1 - u - v)
        float v = 0.0f;
        bool frontFacing = true; // the ray came from the counter-clockwise side
        // the triangle's corners, in the ray's space
        Vector3 v0;
        Vector3 v1;
        Vector3 v2;
    };

    // Möller-Trumbore. True for a hit with t in (0, tMax]; det > 0 when the ray meets the
    // counter-clockwise side.
    JASPERENGINE_API bool rayTriangle(const Vector3& origin, const Vector3& direction,
                                      const Vector3& v0, const Vector3& v1, const Vector3& v2,
                                      float& t, float& u, float& v, float& det);

    // Every triangle of a geometry, without a BVH: the path of small meshes and the reference
    // the BVH must match. The closest hit (ties: the lower triangle index).
    JASPERENGINE_API bool raycastTriangles(const PickGeometry& geometry, const MeshBvhRay& ray, MeshBvhHit& hit);
    // every hit, nearest first (ties by triangle), at most `limit`
    JASPERENGINE_API void raycastAllTriangles(const PickGeometry& geometry, const MeshBvhRay& ray, std::vector<MeshBvhHit>& hits,
                                              uint32_t limit);

    // The paths of the 8-child slab test (D3): chosen once from the CPU (AVX2 needs OSXSAVE,
    // AVX state enabled by the OS, and AVX2; arm64 always has NEON), or forced with
    // JASPER_PICK_SIMD=scalar|avx2|neon (a path the CPU or the build lacks is refused, logged)
    enum class PickSimdPath : uint8_t { SCALAR, AVX2, NEON };

    namespace picking {
        // what the CPU reports (x86); the AVX2 decision is a pure function of it so a test can
        // mask a bit (an injected stub) and see the path go
        struct CpuFeatures {
            bool osxsave = false;   // CPUID.1:ECX.27
            bool avx = false;       // CPUID.1:ECX.28
            bool ymmEnabled = false;// XCR0 bits 1 and 2: the OS saves SSE and AVX state
            bool avx2 = false;      // CPUID.7.0:EBX.5
        };
        JASPERENGINE_API CpuFeatures queryCpuFeatures();
        JASPERENGINE_API bool avx2Usable(const CpuFeatures& features);
    }

    JASPERENGINE_API PickSimdPath getPickSimdPath();
    // false (path unchanged) when this CPU or build cannot run it
    JASPERENGINE_API bool setPickSimdPath(PickSimdPath path);
    JASPERENGINE_API bool isPickSimdPathAvailable(PickSimdPath path);
    JASPERENGINE_API const char* pickSimdPathName(PickSimdPath path);

    class JASPERENGINE_API MeshBvh {
    public:
        // below this many triangles a mesh is tested brute force, without a tree
        static constexpr uint32_t BRUTE_FORCE_TRIANGLES = 256u;
        // triangles per leaf of the binary build (kept in the 8-wide leaves)
        static constexpr uint32_t LEAF_SIZE = 8u;
        // raycastAll keeps at most this many hits
        static constexpr uint32_t MAX_ALL_HITS = 64u;
        // a binary node this deep splits at the median, which bounds the tree depth
        static constexpr uint32_t MAX_SAH_DEPTH = 48u;

        // serialized layout: a change of Node, Triangle or the header bumps the version
        static constexpr uint32_t SERIAL_MAGIC = 0x48564245u; // "EBVH"
        static constexpr uint32_t SERIAL_VERSION = 1u;

        struct Triangle {
            Vector3 v0;
            Vector3 v1;
            Vector3 v2;
            uint32_t index = 0; // the geometry's triangle
        };

        // 8 children, each box 8-bit per axis relative to the node's box:
        //   childMin = boxMin + qMin * scale,  childMax = boxMin + qMax * scale
        // (one multiply and one add on every SIMD path). Built so the decoded box always
        // encloses the child. A slot with count > 0 is a leaf (first = first triangle), with
        // count 0 and its valid bit an inner node (first = node index, always greater than
        // this node's), without its valid bit empty (its box inverted: never hit).
        struct alignas(32) Node {
            float boxMin[3];
            float scale[3];          // boxMin + 255 * scale >= the node box's max
            uint8_t qMinX[8];
            uint8_t qMinY[8];
            uint8_t qMinZ[8];
            uint8_t qMaxX[8];
            uint8_t qMaxY[8];
            uint8_t qMaxZ[8];
            uint32_t first[8];
            uint16_t count[8];
            uint8_t valid;
            uint8_t pad[7];
        };
        static_assert(sizeof(Node) == 128, "MeshBvh::Node is two cache lines; the SIMD paths and the cache files assume it");
        static_assert(sizeof(Triangle) == 40, "MeshBvh::Triangle layout");

        MeshBvh() = default;

        // A tree over the geometry's triangles (copied in: the geometry need not outlive it).
        // Below BRUTE_FORCE_TRIANGLES the triangles are kept without a tree. Triangles with a
        // non-finite corner are left out.
        static MeshBvh build(const PickGeometry& geometry);

        bool raycast(const MeshBvhRay& ray, MeshBvhHit& hit) const;
        // every hit, nearest first (ties by triangle), at most MAX_ALL_HITS
        void raycastAll(const MeshBvhRay& ray, std::vector<MeshBvhHit>& hits) const;

        // New corner positions (the geometry's vertex order, e.g. a posed copy): the
        // triangles take them and the boxes are refitted bottom up; the topology is kept.
        void refit(std::span<const Vector3> positions, std::span<const uint32_t> indices);
        // the same with the corners already in leaf order (leafCorners of the geometry's
        // indices, made once per mesh): what a repeated pose refits with
        std::vector<uint32_t> leafCorners(std::span<const uint32_t> indices) const;
        void refitCorners(std::span<const Vector3> positions, std::span<const uint32_t> corners);

        bool empty() const { return triangles.empty(); }
        bool hasTree() const { return !nodes.empty(); }
        uint32_t getTriangleCount() const { return (uint32_t)triangles.size(); }
        size_t getNodeCount() const { return nodes.size(); }
        // what the tree keeps (triangles, nodes, submesh table)
        size_t memoryBytes() const;
        const AABB& getBounds() const { return bounds; }
        const std::vector<uint32_t>& getSubmeshFirstTriangle() const { return submeshFirstTriangle; }
        uint32_t submeshOf(uint32_t triangle) const;

        // The tree as bytes: the triangle order and the nodes, not the corner positions (a
        // load takes them from the geometry). Little-endian layout of our targets.
        void serialize(std::vector<uint8_t>& out) const;
        // False for anything malformed or not made from this geometry (counts, references,
        // bounds), leaving the tree empty: a caller rebuilds then.
        bool deserialize(std::span<const uint8_t> bytes, const PickGeometry& geometry);

    private:
        std::vector<Triangle> triangles;    // leaf order
        std::vector<Node> nodes;            // [0] the root; a child's index is above its parent's
        std::vector<uint32_t> submeshFirstTriangle;
        AABB bounds;

        bool raycastBrute(const MeshBvhRay& ray, MeshBvhHit& hit) const;
        // every node's boxes from its triangles; with `corners` (leafCorners order) the triangles
        // take their corners from `positions` first
        void refitNodes(std::span<const Vector3> positions = {}, std::span<const uint32_t> corners = {});
    };

}

#endif // MESHBVH_H
