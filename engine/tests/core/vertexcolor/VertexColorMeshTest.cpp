// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R4, R9, D5, D6, R108: the mesh view (logical vertices, adjacency,
// the grid) and the fingerprint.

#include "VertexColorTestSupport.h"

#include <algorithm>

using namespace jasperengine;

// (a named namespace, tests included: a unity build merges the test sources)
namespace vertex_color_mesh_test {
    using namespace jasperengine::vertexcolor;
    using namespace jasperengine::vertexcolor::test;

TEST(VertexColorMesh, WeldsCubeSeamsIntoEightLogicalVertices) {
    const MeshView mesh = cube();
    EXPECT_EQ(mesh.vertexCount(), 24u);
    EXPECT_EQ(mesh.logicalCount(), 8u);
    for (uint32_t l = 0; l < 8; l++) {
        EXPECT_EQ(mesh.memberStart[l + 1] - mesh.memberStart[l], 3u) << "logical " << l;
        for (uint32_t m = mesh.memberStart[l]; m < mesh.memberStart[l + 1]; m++) EXPECT_EQ(mesh.logical[mesh.members[m]], l);
    }
    // 12 edges and 6 face diagonals, both ways
    EXPECT_EQ(mesh.neighbors.size(), 36u);
    // every triangle meets three others through its edges, across the seams
    for (uint32_t t = 0; t < mesh.triangleCount(); t++) EXPECT_EQ(mesh.faceNeighborStart[t + 1] - mesh.faceNeighborStart[t], 3u) << "triangle " << t;
    // the corner's normal is the area-weighted mean of its three faces
    for (uint32_t l = 0; l < 8; l++) {
        const Vector3& p = mesh.logicalPositions[l];
        const Vector3& n = mesh.logicalNormals[l];
        const float k = 1.0f / std::sqrt(3.0f);
        EXPECT_NEAR(n.x, p.x > 0 ? k : -k, 1e-6f);
        EXPECT_NEAR(n.y, p.y > 0 ? k : -k, 1e-6f);
        EXPECT_NEAR(n.z, p.z > 0 ? k : -k, 1e-6f);
    }
    for (uint32_t t = 0; t < mesh.triangleCount(); t++) EXPECT_FLOAT_EQ(mesh.faceAreas[t], 0.5f);
}

TEST(VertexColorMesh, WeldsTheSphereSeamAndPoles) {
    const MeshView mesh = sphere(6, 8);
    EXPECT_EQ(mesh.vertexCount(), 63u);
    // 5 rings of 8 plus the two poles
    EXPECT_EQ(mesh.logicalCount(), 42u);
    EXPECT_EQ(mesh.triangleCount(), 80u);
    // the north pole: 9 GPU vertices, 8 neighbours
    const uint32_t pole = mesh.logical[0];
    EXPECT_EQ(mesh.memberStart[pole + 1] - mesh.memberStart[pole], 9u);
    EXPECT_EQ(mesh.neighborStart[pole + 1] - mesh.neighborStart[pole], 8u);
    for (uint32_t t = 0; t < mesh.triangleCount(); t++) {
        const Vector3 centre = (mesh.positions[mesh.indices[t * 3]] + mesh.positions[mesh.indices[t * 3 + 1]] + mesh.positions[mesh.indices[t * 3 + 2]]) / 3.0f;
        EXPECT_GT(mesh.faceNormals[t].dotProduct(centre), 0.0f) << "triangle " << t << " winds inwards";
        EXPECT_EQ(mesh.faceNeighborStart[t + 1] - mesh.faceNeighborStart[t], 3u);
    }
}

TEST(VertexColorMesh, WeldsWithinAMillionthOfTheDiagonalOnly) {
    MeshView mesh;
    const float diagonal = std::sqrt(2.0f);
    mesh.positions = {Vector3(0, 0, 0), Vector3(1, 1, 0), Vector3(0.5f, 0.5f, 0), Vector3(0.5f + 0.5e-6f * diagonal, 0.5f, 0),
                      Vector3(0.5f, 0.5f + 5e-6f * diagonal, 0)};
    mesh.indices = {0, 1, 2, 0, 1, 3, 0, 1, 4};
    ASSERT_TRUE(mesh.build());
    EXPECT_EQ(mesh.logical[2], mesh.logical[3]);
    EXPECT_NE(mesh.logical[2], mesh.logical[4]);
    EXPECT_EQ(mesh.logicalCount(), 4u);
}

TEST(VertexColorMesh, BuildRefusesInconsistentInputs) {
    std::string error;
    MeshView a;
    a.positions = {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 0, 1)};
    a.indices = {0, 1, 3};
    EXPECT_FALSE(a.build(&error));
    EXPECT_NE(error.find("past the 3 vertices"), std::string::npos) << error;
    MeshView b = a;
    b.indices = {0, 1};
    EXPECT_FALSE(b.build(&error));
    EXPECT_NE(error.find("multiple of 3"), std::string::npos) << error;
    MeshView c = a;
    c.indices = {0, 1, 2};
    c.normals = {Vector3(0, 1, 0)};
    EXPECT_FALSE(c.build(&error));
    EXPECT_NE(error.find("normals has 1 entries"), std::string::npos) << error;
    MeshView d = a;
    d.indices = {0, 1, 2};
    d.assetColors = {Vector4(1, 1, 1, 1), Vector4(1, 1, 1, 1)};
    EXPECT_FALSE(d.build(&error));
    MeshView empty;
    EXPECT_TRUE(empty.build(&error));
    EXPECT_TRUE(empty.built());
    EXPECT_EQ(empty.logicalCount(), 0u);
}

TEST(VertexColorMesh, QueriesMatchABruteForceScan) {
    const MeshView mesh = sphere(12, 16);
    const Vector3 centres[] = {Vector3(0, 1, 0), Vector3(0.7f, 0, 0.7f), Vector3(0, 0, 0), Vector3(2, 2, 2), Vector3(-0.3f, -0.9f, 0.1f)};
    const float radii[] = {0.05f, 0.3f, 0.99f, 1.5f};
    for (const Vector3& centre : centres) {
        for (float radius : radii) {
            std::vector<uint32_t> found = {12345};
            mesh.queryLogical(centre, radius, found);
            std::vector<uint32_t> expected = {12345};
            for (uint32_t l = 0; l < mesh.logicalCount(); l++) {
                const Vector3 d = mesh.logicalPositions[l] - centre;
                if (d.x * d.x + d.y * d.y + d.z * d.z <= radius * radius) expected.push_back(l);
            }
            EXPECT_EQ(found, expected) << "centre " << centre.x << "," << centre.y << "," << centre.z << " radius " << radius;
            std::vector<uint32_t> boxed;
            const AABB box(centre - Vector3(radius, radius, radius), centre + Vector3(radius, radius, radius));
            mesh.queryLogicalBox(box, boxed);
            std::vector<uint32_t> boxExpected;
            for (uint32_t l = 0; l < mesh.logicalCount(); l++) {
                if (box.contains(mesh.logicalPositions[l])) boxExpected.push_back(l);
            }
            EXPECT_EQ(boxed, boxExpected);
        }
    }
}

TEST(VertexColorMesh, FingerprintTracksTopologyAndCoarsePositions) {
    const MeshView mesh = quad();
    const Fingerprint fp = mesh.fingerprint();
    EXPECT_EQ(fp.vertexCount, 4u);
    EXPECT_EQ(fp.indexCount, 6u);
    // FNV-1a over the little-endian index bytes: the same number on every platform
    EXPECT_EQ(hex(fp.topologyHash), "a88238442f71ed01");
    EXPECT_EQ(hex(fp.positionHash), "a37e266d72cb045b");
    EXPECT_EQ(quad().fingerprint(), fp);

    MeshView moved = mesh;
    moved.positions[2].x += 1e-6f;   // below 1e-4 of the bounds: the same coarse positions
    EXPECT_EQ(moved.fingerprint(), fp);
    moved.positions[2].x += 1e-2f;
    EXPECT_TRUE(moved.fingerprint().sameTopology(fp));
    EXPECT_NE(moved.fingerprint().positionHash, fp.positionHash);

    MeshView flipped = mesh;
    std::swap(flipped.indices[0], flipped.indices[1]);
    EXPECT_FALSE(flipped.fingerprint().sameTopology(fp));
}

TEST(VertexColorMesh, ValueAtInterpolatesTheCornerValues) {
    const MeshView mesh = quad();
    TargetData data;
    data.base = {Vector4(1, 0, 0, 1), Vector4(0, 1, 0, 1), Vector4(0, 0, 1, 1), Vector4(1, 1, 1, 1)};
    // triangle 0 is vertices 0, 3, 1
    EXPECT_EQ(valueAt(data, mesh, 0, Vector3(1, 0, 0)), Vector4(1, 0, 0, 1));
    const Vector4 mid = valueAt(data, mesh, 0, Vector3(0.5f, 0.25f, 0.25f));
    EXPECT_FLOAT_EQ(mid.x, 0.75f);
    EXPECT_FLOAT_EQ(mid.y, 0.5f);
    EXPECT_FLOAT_EQ(mid.z, 0.25f);
    // a corner override wins over its vertex's base value (R108 reads stored corners)
    data.setCorner(1, Vector4(0, 0, 0, 0), mesh.indices);
    EXPECT_EQ(valueAt(data, mesh, 0, Vector3(0, 1, 0)), Vector4(0, 0, 0, 0));
    EXPECT_EQ(valueAt(data, mesh, 1, Vector3(0, 1, 0)), Vector4(1, 1, 1, 1));
}

} // namespace vertex_color_mesh_test
