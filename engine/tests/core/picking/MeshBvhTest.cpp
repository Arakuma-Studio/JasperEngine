// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// MeshBvh (docs/specs/S26-scene-view.md R1, R2, R26, AC1, AC2): the tree matches the brute
// force on random rays on every SIMD path, the paths give bit-identical hits, the gather
// widens 16-bit indices, reads strided vertices and expands strips, and the serialized form
// round-trips and refuses anything else. Edge cases ported from OpenEngine's MeshBvhTests.cpp
// (Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License, NOTICE.md).

#include "picking/MeshBvh.h"
#include "picking/PickGeometry.h"
#include "component/MeshComponent.h"
#include "picking/PickingTestUtil.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    std::vector<PickSimdPath> availablePaths(){
        std::vector<PickSimdPath> paths;
        for (PickSimdPath path : {PickSimdPath::SCALAR, PickSimdPath::AVX2, PickSimdPath::NEON}){
            if (isPickSimdPathAvailable(path)){
                paths.push_back(path);
            }
        }
        return paths;
    }

    // restores the process's path after a test switches it
    class SimdPathScope {
    public:
        SimdPathScope(): saved(getPickSimdPath()){}
        ~SimdPathScope(){ setPickSimdPath(saved); }
    private:
        PickSimdPath saved;
    };

    MeshBvhRay makeRay(const TestRay& ray, std::span<const PickFaces> faces = {}){
        MeshBvhRay result;
        result.origin = ray.origin;
        result.direction = ray.direction;
        result.faces = faces;
        return result;
    }

    bool sameBits(float a, float b){
        return std::memcmp(&a, &b, sizeof(float)) == 0;
    }

    // a mesh with one submesh drawn from `positions` (an extra 2-float attribute when strided)
    void fillMesh(MeshComponent& mesh, const std::vector<Vector3>& positions, bool strided){
        mesh.buffer.clear();
        mesh.buffer.addAttribute(AttributeType::POSITION, 3);
        if (strided){
            mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
        }
        Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
        Attribute* uv = strided ? mesh.buffer.getAttribute(AttributeType::TEXCOORD1) : nullptr;
        for (const Vector3& p : positions){
            mesh.buffer.addVector3(position, p);
            if (uv){
                mesh.buffer.addVector2(uv, Vector2(0.25f, 0.75f));
            }
        }
        mesh.numSubmeshes = 1;
        mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    }

}

TEST(MeshBvh, MatchesBruteForceRandomRays){
    SimdPathScope scope;
    const uint32_t sizes[] = {1, 37, 255, 256, 1000, 4000, 16000, 50000, 120000, 200000};
    for (uint32_t m = 0; m < 10; m++){
        // two submeshes, one culling back faces: the cull path is part of what must match
        const PickGeometry geometry = randomGeometry(sizes[m], 1000 + m, 10.0f, 0.6f, 2);
        const MeshBvh bvh = MeshBvh::build(geometry);
        EXPECT_EQ(bvh.hasTree(), sizes[m] >= MeshBvh::BRUTE_FORCE_TRIANGLES) << sizes[m];
        const PickFaces faces[2] = {PickFaces::BOTH, PickFaces::FRONT};
        const std::vector<TestRay> rays = randomRays(2000, 77 + m);
        for (PickSimdPath path : availablePaths()){
            ASSERT_TRUE(setPickSimdPath(path));
            uint32_t hits = 0;
            for (const TestRay& testRay : rays){
                const MeshBvhRay ray = makeRay(testRay, faces);
                MeshBvhHit expected, actual;
                const bool expectedHit = raycastTriangles(geometry, ray, expected);
                const bool actualHit = bvh.raycast(ray, actual);
                ASSERT_EQ(actualHit, expectedHit) << "mesh " << sizes[m] << " path " << pickSimdPathName(path);
                if (!expectedHit){
                    continue;
                }
                hits++;
                ASSERT_EQ(actual.triangle, expected.triangle) << "mesh " << sizes[m] << " path " << pickSimdPathName(path);
                ASSERT_EQ(actual.submesh, expected.submesh);
                ASSERT_LE(std::fabs(actual.t - expected.t), 1e-5f * expected.t);
            }
            if (sizes[m] >= 1000){
                EXPECT_GT(hits, 100u) << "the rays should meet the bigger meshes";
            }
        }
    }
}

TEST(MeshBvh, SimdPathsBitIdentical){
    SimdPathScope scope;
    const PickGeometry geometry = randomGeometry(30000, 5, 10.0f, 0.8f);
    const MeshBvh bvh = MeshBvh::build(geometry);
    const std::vector<TestRay> rays = randomRays(1500, 9);

    ASSERT_TRUE(setPickSimdPath(PickSimdPath::SCALAR));
    std::vector<MeshBvhHit> reference;
    std::vector<std::vector<MeshBvhHit>> referenceAll;
    for (const TestRay& testRay : rays){
        MeshBvhHit hit;
        hit.triangle = UINT32_MAX;
        bvh.raycast(makeRay(testRay), hit);
        reference.push_back(hit);
        std::vector<MeshBvhHit> all;
        bvh.raycastAll(makeRay(testRay), all);
        referenceAll.push_back(all);
    }

    for (PickSimdPath path : availablePaths()){
        ASSERT_TRUE(setPickSimdPath(path));
        for (size_t i = 0; i < rays.size(); i++){
            MeshBvhHit hit;
            hit.triangle = UINT32_MAX;
            bvh.raycast(makeRay(rays[i]), hit);
            ASSERT_EQ(hit.triangle, reference[i].triangle) << pickSimdPathName(path);
            ASSERT_TRUE(sameBits(hit.t, reference[i].t) && sameBits(hit.u, reference[i].u) && sameBits(hit.v, reference[i].v))
                << pickSimdPathName(path);
            std::vector<MeshBvhHit> all;
            bvh.raycastAll(makeRay(rays[i]), all);
            ASSERT_EQ(all.size(), referenceAll[i].size()) << pickSimdPathName(path);
            for (size_t h = 0; h < all.size(); h++){
                ASSERT_EQ(all[h].triangle, referenceAll[i][h].triangle);
                ASSERT_TRUE(sameBits(all[h].t, referenceAll[i][h].t));
            }
        }
    }
}

TEST(MeshBvh, ScalarForcedOnAvx2Host){
    SimdPathScope scope;
    // the CI runs the suite a second time with JASPER_PICK_SIMD=scalar: the path is then scalar
    // from the start; otherwise it is forced here, and must answer as the native path does
    const char* forced = std::getenv("JASPER_PICK_SIMD");
    if (forced && std::string(forced) == "scalar"){
        EXPECT_EQ(getPickSimdPath(), PickSimdPath::SCALAR);
    }
    const PickSimdPath native = getPickSimdPath();
    const PickGeometry geometry = randomGeometry(8000, 21);
    const MeshBvh bvh = MeshBvh::build(geometry);
    const std::vector<TestRay> rays = randomRays(1000, 22);
    std::vector<MeshBvhHit> nativeHits(rays.size());
    for (size_t i = 0; i < rays.size(); i++){
        nativeHits[i].triangle = UINT32_MAX;
        bvh.raycast(makeRay(rays[i]), nativeHits[i]);
    }
    ASSERT_TRUE(setPickSimdPath(PickSimdPath::SCALAR));
    for (size_t i = 0; i < rays.size(); i++){
        MeshBvhHit hit;
        hit.triangle = UINT32_MAX;
        bvh.raycast(makeRay(rays[i]), hit);
        ASSERT_EQ(hit.triangle, nativeHits[i].triangle) << "native " << pickSimdPathName(native);
        ASSERT_TRUE(sameBits(hit.t, nativeHits[i].t));
    }
}

TEST(MeshBvh, Avx2UnavailableWhenCpuidMasked){
    picking::CpuFeatures features;
    features.osxsave = true;
    features.avx = true;
    features.ymmEnabled = true;
    features.avx2 = true;
    EXPECT_TRUE(picking::avx2Usable(features));
    // each bit missing (the CPU or the OS without it) rules AVX2 out
    picking::CpuFeatures masked = features;
    masked.avx2 = false;
    EXPECT_FALSE(picking::avx2Usable(masked));
    masked = features;
    masked.osxsave = false;
    EXPECT_FALSE(picking::avx2Usable(masked));
    masked = features;
    masked.ymmEnabled = false;
    EXPECT_FALSE(picking::avx2Usable(masked));
    masked = features;
    masked.avx = false;
    EXPECT_FALSE(picking::avx2Usable(masked));
    // what this host reports agrees with whether the path can be chosen
    // (not an #if inside the macro's arguments: MSVC refuses directives there)
#if defined(__x86_64__) || defined(_M_X64)
    const bool expectedAvx2 = picking::avx2Usable(picking::queryCpuFeatures());
#else
    const bool expectedAvx2 = false;
#endif
    EXPECT_EQ(isPickSimdPathAvailable(PickSimdPath::AVX2), expectedAvx2);
    EXPECT_TRUE(isPickSimdPathAvailable(PickSimdPath::SCALAR));
}

TEST(MeshBvh, SingleTriangle){
    PickGeometry geometry;
    geometry.positions = {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0)};
    geometry.indices = {0, 1, 2};
    geometry.submeshFirstTriangle = {0, 1};
    const MeshBvh bvh = MeshBvh::build(geometry);
    ASSERT_EQ(bvh.getTriangleCount(), 1u);

    MeshBvhRay ray;
    ray.origin = Vector3(0.25f, 0.25f, 5.0f);
    ray.direction = Vector3(0, 0, -1);
    MeshBvhHit hit;
    ASSERT_TRUE(bvh.raycast(ray, hit));
    EXPECT_EQ(hit.triangle, 0u);
    EXPECT_NEAR(hit.t, 5.0f, 1e-6f);
    EXPECT_NEAR(hit.u, 0.25f, 1e-6f);
    EXPECT_NEAR(hit.v, 0.25f, 1e-6f);
    EXPECT_TRUE(hit.frontFacing); // seen from +Z the corners run counter-clockwise

    // the culled side
    const PickFaces back[] = {PickFaces::BACK};
    ray.faces = back;
    EXPECT_FALSE(bvh.raycast(ray, hit));
    // beyond tMax
    ray.faces = {};
    ray.tMax = 4.0f;
    EXPECT_FALSE(bvh.raycast(ray, hit));
    // a miss next to it
    ray.tMax = 100.0f;
    ray.origin = Vector3(0.8f, 0.8f, 5.0f);
    EXPECT_FALSE(bvh.raycast(ray, hit));
}

TEST(MeshBvh, Empty){
    PickGeometry geometry;
    const MeshBvh bvh = MeshBvh::build(geometry);
    EXPECT_TRUE(bvh.empty());
    EXPECT_FALSE(bvh.hasTree());
    MeshBvhRay ray;
    ray.direction = Vector3(0, 0, 1);
    MeshBvhHit hit;
    EXPECT_FALSE(bvh.raycast(ray, hit));
    std::vector<MeshBvhHit> hits;
    bvh.raycastAll(ray, hits);
    EXPECT_TRUE(hits.empty());

    // degenerate and non-finite triangles only
    geometry.positions = {Vector3(0, 0, 0), Vector3(0, 0, 0), Vector3(0, 0, 0), Vector3(NAN, 0, 0)};
    geometry.indices = {0, 1, 2, 0, 3, 1};
    geometry.submeshFirstTriangle = {0, 2};
    const MeshBvh degenerate = MeshBvh::build(geometry);
    EXPECT_EQ(degenerate.getTriangleCount(), 1u); // the NaN one is left out
    EXPECT_FALSE(degenerate.raycast(ray, hit));
}

TEST(MeshBvh, AxisAlignedRay){
    SimdPathScope scope;
    // a flat grid: every ray along an axis has two zero direction components
    const PickGeometry geometry = gridGeometry(64, 10.0f);
    const MeshBvh bvh = MeshBvh::build(geometry);
    ASSERT_TRUE(bvh.hasTree());
    for (PickSimdPath path : availablePaths()){
        ASSERT_TRUE(setPickSimdPath(path));
        for (float x = -4.9f; x < 5.0f; x += 0.73f){
            for (float z = -4.9f; z < 5.0f; z += 0.81f){
                MeshBvhRay down;
                down.origin = Vector3(x, 3.0f, z);
                down.direction = Vector3(0, -1, 0);
                MeshBvhHit hit, expected;
                ASSERT_TRUE(bvh.raycast(down, hit)) << x << "," << z << " " << pickSimdPathName(path);
                ASSERT_TRUE(raycastTriangles(geometry, down, expected));
                EXPECT_EQ(hit.triangle, expected.triangle);
                EXPECT_NEAR(hit.t, 3.0f, 1e-5f);
            }
        }
        // in the grid's plane: parallel to every triangle
        MeshBvhRay along;
        along.origin = Vector3(-20.0f, 0.0f, 0.1f);
        along.direction = Vector3(1, 0, 0);
        MeshBvhHit hit;
        EXPECT_FALSE(bvh.raycast(along, hit));
        // beside the grid, straight down
        MeshBvhRay outside;
        outside.origin = Vector3(7.0f, 3.0f, 0.0f);
        outside.direction = Vector3(0, -1, 0);
        EXPECT_FALSE(bvh.raycast(outside, hit));
    }
}

TEST(MeshBvh, CustomStride){
    // positions interleaved with texture coordinates: a 20-byte stride
    const PickGeometry soup = randomGeometry(300, 3);
    MeshComponent mesh;
    fillMesh(mesh, soup.positions, true);
    ASSERT_EQ(mesh.buffer.getStride(), 20u);

    PickGeometry gathered;
    ASSERT_TRUE(picking::gather(mesh, gathered));
    ASSERT_EQ(gathered.getTriangleCount(), 300u);
    for (size_t i = 0; i < soup.positions.size(); i++){
        ASSERT_EQ(gathered.positions[gathered.indices[i]], soup.positions[i]);
    }
    const MeshBvh bvh = MeshBvh::build(gathered);
    for (const TestRay& testRay : randomRays(500, 4)){
        MeshBvhHit a, b;
        const bool hitA = bvh.raycast(makeRay(testRay), a);
        ASSERT_EQ(hitA, raycastTriangles(soup, makeRay(testRay), b));
        if (hitA){
            EXPECT_EQ(a.triangle, b.triangle);
        }
    }
}

TEST(MeshBvh, Uint16Indices){
    const PickGeometry grid = gridGeometry(20, 4.0f);
    MeshComponent mesh;
    fillMesh(mesh, grid.positions, false);
    Attribute* index = mesh.indices.getAttribute(AttributeType::INDEX);
    for (uint32_t i : grid.indices){
        mesh.indices.addUInt16(index, (uint16_t)i);
    }
    // an index past the vertices: that triangle stays, degenerate, and numbering holds
    mesh.indices.addUInt16(index, 0);
    mesh.indices.addUInt16(index, 9999);
    mesh.indices.addUInt16(index, 1);

    PickGeometry gathered;
    PickGeometryLayout layout;
    ASSERT_TRUE(picking::gather(mesh, gathered, &layout));
    ASSERT_EQ(gathered.getTriangleCount(), grid.getTriangleCount() + 1);
    for (size_t i = 0; i < grid.indices.size(); i++){
        ASSERT_EQ(gathered.indices[i], grid.indices[i]);
    }
    const uint32_t last = grid.getTriangleCount();
    EXPECT_EQ(gathered.indices[last * 3], gathered.indices[last * 3 + 1]);

    uint32_t submesh = 99, corners[3];
    ASSERT_TRUE(picking::triangleCorners(layout, 5, submesh, corners));
    EXPECT_EQ(submesh, 0u);
    EXPECT_EQ(corners[0], grid.indices[15]);
    EXPECT_EQ(corners[2], grid.indices[17]);

    MeshBvhRay ray;
    ray.origin = Vector3(0.3f, 2.0f, 0.3f);
    ray.direction = Vector3(0, -1, 0);
    MeshBvhHit hit;
    ASSERT_TRUE(MeshBvh::build(gathered).raycast(ray, hit));
    EXPECT_NEAR(hit.t, 2.0f, 1e-5f);
}

TEST(MeshBvh, TriangleStrip){
    // a strip zig-zagging along x: 0 2 4 ... on z = 0, 1 3 5 ... on z = 1
    std::vector<Vector3> strip;
    for (int i = 0; i < 12; i++){
        strip.push_back(Vector3((float)(i / 2), 0.0f, (float)(i % 2)));
    }
    MeshComponent mesh;
    fillMesh(mesh, strip, false);
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLE_STRIP;

    PickGeometry gathered;
    ASSERT_TRUE(picking::gather(mesh, gathered));
    ASSERT_EQ(gathered.getTriangleCount(), 10u);
    // every triangle of a strip faces the way its first one does (odd ones flipped)
    for (uint32_t t = 0; t < 10; t++){
        const Vector3& a = gathered.positions[gathered.indices[t * 3]];
        const Vector3& b = gathered.positions[gathered.indices[t * 3 + 1]];
        const Vector3& c = gathered.positions[gathered.indices[t * 3 + 2]];
        const Vector3 n = (b - a).crossProduct(c - a);
        EXPECT_GT(n.y, 0.0f) << "triangle " << t;
    }
    MeshBvhRay ray;
    ray.origin = Vector3(2.3f, 1.0f, 0.5f);
    ray.direction = Vector3(0, -1, 0);
    MeshBvhHit hit;
    ASSERT_TRUE(raycastTriangles(gathered, ray, hit));
    EXPECT_TRUE(hit.frontFacing);
    EXPECT_NEAR(hit.t, 1.0f, 1e-6f);
    EXPECT_TRUE(hit.triangle == 4u || hit.triangle == 5u);
}

TEST(MeshBvh, RaycastAllSortedAndCapped){
    // 100 stacked planes: one ray crosses them all
    PickGeometry geometry;
    for (int layer = 0; layer < 100; layer++){
        const float y = (float)layer * 0.1f;
        const uint32_t base = (uint32_t)geometry.positions.size();
        geometry.positions.insert(geometry.positions.end(), {Vector3(-1, y, -1), Vector3(-1, y, 1), Vector3(1, y, -1), Vector3(1, y, 1)});
        geometry.indices.insert(geometry.indices.end(), {base, base + 1, base + 2, base + 2, base + 1, base + 3});
    }
    geometry.submeshFirstTriangle = {0, 200};
    const MeshBvh bvh = MeshBvh::build(geometry);
    ASSERT_FALSE(bvh.hasTree()); // 200 triangles: brute force
    MeshBvhRay ray;
    ray.origin = Vector3(0.1f, 20.0f, 0.2f);
    ray.direction = Vector3(0, -1, 0);
    std::vector<MeshBvhHit> hits;
    bvh.raycastAll(ray, hits);
    ASSERT_EQ(hits.size(), MeshBvh::MAX_ALL_HITS);
    for (size_t i = 1; i < hits.size(); i++){
        EXPECT_LE(hits[i - 1].t, hits[i].t);
    }
    EXPECT_NEAR(hits.front().t, 20.0f - 9.9f, 1e-4f);

    // the same through a tree, and the brute-force reference
    PickGeometry bigger = geometry;
    for (int copy = 1; copy < 4; copy++){
        const uint32_t base = (uint32_t)bigger.positions.size();
        for (size_t i = 0; i < geometry.positions.size(); i++){
            bigger.positions.push_back(geometry.positions[i] + Vector3(5.0f * copy, 0, 0));
        }
        for (uint32_t index : geometry.indices){
            bigger.indices.push_back(index + base);
        }
    }
    bigger.submeshFirstTriangle = {0, 800};
    const MeshBvh tree = MeshBvh::build(bigger);
    ASSERT_TRUE(tree.hasTree());
    std::vector<MeshBvhHit> treeHits, expected;
    tree.raycastAll(ray, treeHits);
    raycastAllTriangles(bigger, ray, expected, MeshBvh::MAX_ALL_HITS);
    ASSERT_EQ(treeHits.size(), expected.size());
    for (size_t i = 0; i < treeHits.size(); i++){
        EXPECT_EQ(treeHits[i].triangle, expected[i].triangle);
    }
}

TEST(MeshBvh, RefitMatchesFreshBuild){
    const PickGeometry geometry = randomGeometry(6000, 41);
    MeshBvh bvh = MeshBvh::build(geometry);
    // every vertex moved
    PickGeometry moved = geometry;
    for (Vector3& p : moved.positions){
        p = Vector3(p.x * 1.3f + 2.0f, p.y * 0.7f - 1.0f, p.z + std::sin(p.x));
    }
    bvh.refit(moved.positions, moved.indices);
    for (const TestRay& testRay : randomRays(1500, 42, 14.0f)){
        MeshBvhHit a, b;
        const bool hitA = bvh.raycast(makeRay(testRay), a);
        ASSERT_EQ(hitA, raycastTriangles(moved, makeRay(testRay), b));
        if (hitA){
            ASSERT_EQ(a.triangle, b.triangle);
        }
    }
}

TEST(MeshBvh, SerializeRoundTrip){
    const PickGeometry geometry = randomGeometry(9000, 51);
    const MeshBvh bvh = MeshBvh::build(geometry);
    std::vector<uint8_t> bytes;
    bvh.serialize(bytes);
    MeshBvh loaded;
    ASSERT_TRUE(loaded.deserialize(bytes, geometry));
    EXPECT_EQ(loaded.getTriangleCount(), bvh.getTriangleCount());
    EXPECT_EQ(loaded.getNodeCount(), bvh.getNodeCount());
    std::vector<uint8_t> again;
    loaded.serialize(again);
    EXPECT_EQ(again, bytes);
    for (const TestRay& testRay : randomRays(500, 52)){
        MeshBvhHit a, b;
        const bool hitA = loaded.raycast(makeRay(testRay), a);
        ASSERT_EQ(hitA, bvh.raycast(makeRay(testRay), b));
        if (hitA){
            ASSERT_EQ(a.triangle, b.triangle);
            ASSERT_TRUE(sameBits(a.t, b.t));
        }
    }
}

TEST(MeshBvh, DeserializeRejectsMalformed){
    const PickGeometry geometry = randomGeometry(3000, 61);
    const MeshBvh bvh = MeshBvh::build(geometry);
    std::vector<uint8_t> bytes;
    bvh.serialize(bytes);

    MeshBvh loaded;
    // truncated
    std::vector<uint8_t> truncated(bytes.begin(), bytes.end() - 10);
    EXPECT_FALSE(loaded.deserialize(truncated, geometry));
    EXPECT_TRUE(loaded.empty());
    // another version
    std::vector<uint8_t> version = bytes;
    version[4] ^= 0xFF;
    EXPECT_FALSE(loaded.deserialize(version, geometry));
    // a triangle listed twice
    std::vector<uint8_t> twice = bytes;
    std::memcpy(twice.data() + 24 + 4, twice.data() + 24, 4);
    EXPECT_FALSE(loaded.deserialize(twice, geometry));
    // a child pointing back up (a cycle)
    std::vector<uint8_t> cycle = bytes;
    const size_t nodes = bytes.size() - bvh.getNodeCount() * sizeof(MeshBvh::Node);
    MeshBvh::Node root;
    std::memcpy(&root, cycle.data() + nodes, sizeof(root));
    for (int s = 0; s < 8; s++){
        if ((root.valid & (1u << s)) && root.count[s] == 0){
            root.first[s] = 0;
            break;
        }
    }
    std::memcpy(cycle.data() + nodes, &root, sizeof(root));
    EXPECT_FALSE(loaded.deserialize(cycle, geometry));
    // another geometry
    const PickGeometry other = randomGeometry(2999, 62);
    EXPECT_FALSE(loaded.deserialize(bytes, other));
    // and the untouched bytes still load
    EXPECT_TRUE(loaded.deserialize(bytes, geometry));
}

TEST(MeshBvh, CulledFacesAreSkipped){
    // two parallel squares: the near one seen from behind
    PickGeometry geometry;
    geometry.positions = {Vector3(-1, -1, 0), Vector3(1, -1, 0), Vector3(1, 1, 0), Vector3(-1, 1, 0),
                          Vector3(-1, -1, -2), Vector3(1, -1, -2), Vector3(1, 1, -2), Vector3(-1, 1, -2)};
    // near square faces -Z (clockwise from +Z), far one faces +Z
    geometry.indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7};
    geometry.submeshFirstTriangle = {0, 2, 4};
    MeshBvhRay ray;
    ray.origin = Vector3(0.2f, 0.1f, 5.0f);
    ray.direction = Vector3(0, 0, -1);
    MeshBvhHit hit;
    const PickFaces culled[] = {PickFaces::FRONT, PickFaces::FRONT};
    ray.faces = culled;
    ASSERT_TRUE(raycastTriangles(geometry, ray, hit));
    EXPECT_EQ(hit.submesh, 1u);
    EXPECT_NEAR(hit.t, 7.0f, 1e-5f);
    const PickFaces doubleSided[] = {PickFaces::BOTH, PickFaces::FRONT};
    ray.faces = doubleSided;
    ASSERT_TRUE(raycastTriangles(geometry, ray, hit));
    EXPECT_EQ(hit.submesh, 0u);
    EXPECT_FALSE(hit.frontFacing);
}
