// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R11-R13 (AC3): split vertices, the LOD rule and the stream bytes

#include "VertexColorTestMesh.h"
#include "vertexcolor/VertexColorSplit.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

// (a named namespace, tests included: a unity build merges the test sources)
namespace vertex_color_split_test {
    using namespace vctest;

    const Vector4 WHITE(1.0f, 1.0f, 1.0f, 1.0f);
    const Vector4 RED(1.0f, 0.0f, 0.0f, 1.0f);
    const Vector4 BLUE(0.0f, 0.0f, 1.0f, 1.0f);

    // a fold: triangle 0 (0, 1, 2) faces +Z, triangle 1 (0, 2, 3) faces +X; vertex 4 is no LOD0 triangle's
    MeshView fold() {
        MeshView m;
        m.positions = {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1), Vector3(1, 1, 1)};
        m.indices = {0, 1, 2, 0, 2, 3};
        build(m);
        return m;
    }

    TargetData whiteTarget(const MeshView& m) {
        TargetData t;
        t.base.assign(m.vertexCount(), WHITE);
        return t;
    }

    std::vector<uint32_t> list(std::initializer_list<uint32_t> v) { return std::vector<uint32_t>(v); }

TEST(VertexColorSplit, NoOverridesKeepsTheSharedGeometry) {
    const MeshView m = fold();
    TargetData t = whiteTarget(m);
    t.base[2] = BLUE;
    const std::vector<std::vector<uint32_t>> lods = {list({0, 1, 3})};
    const SplitResult r = buildSplit(t, m, lods);
    EXPECT_FALSE(r.split);
    EXPECT_EQ(r.added, 0u);
    EXPECT_EQ(r.values, t.base);
    EXPECT_TRUE(r.indices.empty());
    EXPECT_TRUE(r.lodIndices.empty());
}

TEST(VertexColorSplit, FaceOverridesAddOneCopyPerDistinctVertexValue) {
    const MeshView m = fold();
    TargetData t = whiteTarget(m);
    // triangle 1 all red: its three vertices need a red copy (vertex 3 too: originals stay)
    t.corners = {CornerValue{3, RED}, CornerValue{4, RED}, CornerValue{5, RED}};
    const SplitResult r = buildSplit(t, m, {});
    ASSERT_TRUE(r.split);
    EXPECT_EQ(r.added, 3u);
    ASSERT_EQ(r.values.size(), 8u);
    EXPECT_EQ(r.indices, list({0, 1, 2, 5, 6, 7}));
    EXPECT_EQ(r.sourceVertex, list({0, 1, 2, 3, 4, 0, 2, 3}));
    for (uint32_t v = 0; v < 5; v++) EXPECT_EQ(r.values[v], WHITE) << v;
    for (uint32_t v = 5; v < 8; v++) EXPECT_EQ(r.values[v], RED) << v;
}

TEST(VertexColorSplit, CornersWithTheSameValueShareOneCopy) {
    // a fan around vertex 0: its corners in triangles 1 and 2 both red, in triangle 0 blue
    MeshView m;
    m.positions = {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 1, 0), Vector3(0, 1, 0), Vector3(-1, 1, 0)};
    m.indices = {0, 1, 2, 0, 2, 3, 0, 3, 4};
    build(m);
    TargetData t = whiteTarget(m);
    t.corners = {CornerValue{0, BLUE}, CornerValue{3, RED}, CornerValue{6, RED}};
    const SplitResult r = buildSplit(t, m, {});
    ASSERT_TRUE(r.split);
    EXPECT_EQ(r.added, 2u);
    EXPECT_EQ(r.indices, list({5, 1, 2, 6, 2, 3, 6, 3, 4}));
    EXPECT_EQ(r.values[5], BLUE);
    EXPECT_EQ(r.values[6], RED);
}

TEST(VertexColorSplit, LodCornerTakesTheClosestLod0Triangle) {
    const MeshView m = fold();
    TargetData t = whiteTarget(m);
    t.corners = {CornerValue{3, RED}};   // vertex 0 in triangle 1 (+X)
    const std::vector<std::vector<uint32_t>> lods = {
        list({0, 2, 3}),   // faces +X: triangle 1 is closest, red, the LOD0 copy is reused
        list({0, 3, 2}),   // faces -X: triangle 0 (dot 0) beats triangle 1 (dot -1), white
        list({1, 4, 2}),   // vertex 4 is in no LOD0 triangle: its base, the original vertex
    };
    const SplitResult r = buildSplit(t, m, lods);
    ASSERT_TRUE(r.split);
    EXPECT_EQ(r.added, 1u);
    EXPECT_EQ(r.indices, list({0, 1, 2, 5, 2, 3}));
    ASSERT_EQ(r.lodIndices.size(), 3u);
    EXPECT_EQ(r.lodIndices[0], list({5, 2, 3}));
    EXPECT_EQ(r.lodIndices[1], list({0, 3, 2}));
    EXPECT_EQ(r.lodIndices[2], list({1, 4, 2}));
}

TEST(VertexColorSplit, LodTieGoesToTheLowestLod0Triangle) {
    const MeshView m = fold();
    // LOD triangle (0, 1, 3) faces -Y: dot 0 with both LOD0 triangles
    const std::vector<std::vector<uint32_t>> lods = {list({0, 1, 3})};

    TargetData overrideInLater = whiteTarget(m);
    overrideInLater.corners = {CornerValue{3, RED}};   // triangle 1
    SplitResult r = buildSplit(overrideInLater, m, lods);
    EXPECT_EQ(r.lodIndices[0], list({0, 1, 3})) << "triangle 0's corner (white) wins the tie";

    TargetData overrideInFirst = whiteTarget(m);
    overrideInFirst.corners = {CornerValue{0, RED}};   // triangle 0
    r = buildSplit(overrideInFirst, m, lods);
    ASSERT_EQ(r.added, 1u);
    EXPECT_EQ(r.indices, list({5, 1, 2, 0, 2, 3}));
    EXPECT_EQ(r.lodIndices[0], list({5, 1, 3})) << "triangle 0's corner (red) wins the tie";
}

TEST(VertexColorSplit, LodCornersReuseTheLod0Copies) {
    // vertex 0's LOD0 corners are red (triangle 0) and blue (triangle 1); a LOD triangle facing
    // +X takes blue, one facing +Z red: both copies already exist, nothing more is added
    const MeshView m = fold();
    TargetData t = whiteTarget(m);
    t.corners = {CornerValue{0, RED}, CornerValue{3, BLUE}};
    const std::vector<std::vector<uint32_t>> lods = {list({0, 2, 3, 0, 1, 2})};
    const SplitResult r = buildSplit(t, m, lods);
    EXPECT_EQ(r.added, 2u);
    EXPECT_EQ(r.indices, list({5, 1, 2, 6, 2, 3}));
    EXPECT_EQ(r.lodIndices[0], list({6, 2, 3, 5, 1, 2}));
}

TEST(VertexColorSplit, SameDataGivesTheSameBuffers) {
    // R13: identical content must produce identical bytes, so the buffer pool shares them
    MeshView m;
    addPlane(m, 2.0f, 6, 0.0f);
    build(m);
    TargetData t = whiteTarget(m);
    for (uint32_t c = 0; c < m.indices.size(); c += 7) t.corners.push_back(CornerValue{c, Vector4((float)(c % 3) / 2.0f, 0.25f, 0.5f, 1.0f)});
    const std::vector<std::vector<uint32_t>> lods = {std::vector<uint32_t>(m.indices.begin(), m.indices.begin() + 30)};
    const SplitResult a = buildSplit(t, m, lods);
    const SplitResult b = buildSplit(t, m, lods);
    ASSERT_TRUE(a.split);
    EXPECT_EQ(a.indices, b.indices);
    EXPECT_EQ(a.lodIndices, b.lodIndices);
    EXPECT_EQ(a.sourceVertex, b.sourceVertex);
    EXPECT_EQ(packValues(a.values, Format::RGBA8), packValues(b.values, Format::RGBA8));
    EXPECT_EQ(packValues(a.values, Format::RGBA16F), packValues(b.values, Format::RGBA16F));
    // every output corner draws the value its corner holds
    for (uint32_t c = 0; c < m.indices.size(); c++) {
        Vector4 expected = t.base[m.indices[c]];
        for (const CornerValue& o : t.corners)
            if (o.corner == c) expected = o.value;
        EXPECT_EQ(a.values[a.indices[c]], expected) << c;
        EXPECT_EQ(a.sourceVertex[a.indices[c]], m.indices[c]) << c;
    }
}

TEST(VertexColorSplit, PackValuesBytes) {
    const std::vector<Vector4> values = {Vector4(0.0f, 0.5f, 1.0f, 1.5f),
                                         Vector4(-1.0f, std::numeric_limits<float>::quiet_NaN(), 0.2f, 1.0f / 255.0f)};
    EXPECT_EQ(packValues(values, Format::RGBA8), (std::vector<uint8_t>{0, 128, 255, 255, 0, 0, 51, 1}));

    const std::vector<Vector4> hdr = {Vector4(1.0f, 0.5f, -2.0f, 65504.0f)};
    EXPECT_EQ(packValues(hdr, Format::RGBA16F), (std::vector<uint8_t>{0x00, 0x3C, 0x00, 0x38, 0x00, 0xC0, 0xFF, 0x7B}));
    EXPECT_TRUE(packValues({}, Format::RGBA8).empty());
}

} // namespace vertex_color_split_test
