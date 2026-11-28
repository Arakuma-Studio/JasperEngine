// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R50-R54: element selection maths.

#include "VertexColorTestSupport.h"

#include "vertexcolor/VertexColorSelect.h"

namespace VertexColorSelectTest_cpp {

using namespace jasperengine;

// (a named namespace, tests included: a unity build merges the test sources)
namespace vertex_color_select_test {
    using namespace jasperengine::vertexcolor;
    using namespace jasperengine::vertexcolor::test;

    Selection make(ElementKind kind, std::vector<uint32_t> ids) {
        Selection s;
        s.kind = kind;
        s.ids = std::move(ids);
        return s;
    }

    // two quads that share no vertex: an island each
    MeshView twoIslands() {
        MeshView mesh;
        mesh.positions = {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 0, 1), Vector3(0, 0, 1),
                          Vector3(5, 0, 0), Vector3(6, 0, 0), Vector3(6, 0, 1), Vector3(5, 0, 1)};
        mesh.indices = {0, 3, 1, 1, 3, 2, 4, 7, 5, 5, 7, 6};
        return built(mesh);
    }

TEST(VertexColorSelect, AllInvertAndSetOperations) {
    const MeshView mesh = cube();
    EXPECT_EQ(selectAll(mesh, ElementKind::Vertex).ids.size(), 8u);
    EXPECT_EQ(selectAll(mesh, ElementKind::Face).ids.size(), 12u);
    EXPECT_EQ(selectAll(mesh, ElementKind::Corner).ids.size(), 36u);
    const Selection some = make(ElementKind::Face, {0, 5, 11});
    const Selection rest = invert(mesh, some);
    EXPECT_EQ(rest.ids.size(), 9u);
    EXPECT_TRUE(subtract(selectAll(mesh, ElementKind::Face), rest).ids == some.ids);
    EXPECT_EQ(unite(some, rest).ids.size(), 12u);
    EXPECT_EQ(unite(make(ElementKind::Face, {3, 1}), make(ElementKind::Face, {1, 2})).ids, std::vector<uint32_t>({1, 2, 3}));
}

TEST(VertexColorSelect, ConvertsBetweenKinds) {
    const MeshView mesh = quad();
    // vertices 0 and 1 of the quad are logical 0 and 1; triangle 0 is (0, 3, 1)
    const Selection vertices = make(ElementKind::Vertex, {0, 1, 3});
    EXPECT_EQ(convert(mesh, vertices, ElementKind::Face).ids, std::vector<uint32_t>({0}));
    EXPECT_EQ(convert(mesh, make(ElementKind::Vertex, {0}), ElementKind::Face, true).ids, std::vector<uint32_t>({0}));
    EXPECT_EQ(convert(mesh, make(ElementKind::Vertex, {1}), ElementKind::Face, true).ids, std::vector<uint32_t>({0, 1}));
    EXPECT_EQ(convert(mesh, make(ElementKind::Face, {1}), ElementKind::Vertex).ids, std::vector<uint32_t>({1, 2, 3}));
    EXPECT_EQ(convert(mesh, make(ElementKind::Face, {1}), ElementKind::Corner).ids, std::vector<uint32_t>({3, 4, 5}));
    EXPECT_EQ(convert(mesh, make(ElementKind::Corner, {2, 4}), ElementKind::Face).ids, std::vector<uint32_t>({0, 1}));
    EXPECT_EQ(convert(mesh, make(ElementKind::Vertex, {1}), ElementKind::Corner).ids, std::vector<uint32_t>({2, 3}));
}

TEST(VertexColorSelect, GrowAndShrinkByOneRing) {
    const MeshView mesh = grid(5, 1.0f);
    const uint32_t centre = mesh.logical[gridVertex(2, 2, 5)];
    const Selection one = make(ElementKind::Vertex, {centre});
    const Selection ring = grow(mesh, one);
    // the grid's diagonals make six edge neighbours
    EXPECT_EQ(ring.ids.size(), 1u + (mesh.neighborStart[centre + 1] - mesh.neighborStart[centre]));
    EXPECT_EQ(shrink(mesh, ring).ids, one.ids);
    EXPECT_TRUE(shrink(mesh, one).empty());

    const Selection face = make(ElementKind::Face, {10});
    const Selection grownFaces = grow(mesh, face);
    EXPECT_EQ(grownFaces.ids.size(), 4u);
    EXPECT_EQ(shrink(mesh, grownFaces).ids, face.ids);

    // corners grow with their logical vertices and never shrink into corners they did not have
    const Selection corners = convert(mesh, one, ElementKind::Corner);
    const Selection grownCorners = grow(mesh, corners);
    EXPECT_EQ(grownCorners.ids, convert(mesh, ring, ElementKind::Corner).ids);
    EXPECT_EQ(shrink(mesh, grownCorners).ids, corners.ids);
}

TEST(VertexColorSelect, LinkedStaysOnItsIslandAcrossSeams) {
    const MeshView islands = twoIslands();
    EXPECT_EQ(linked(islands, make(ElementKind::Face, {0})).ids, std::vector<uint32_t>({0, 1}));
    EXPECT_EQ(linked(islands, make(ElementKind::Vertex, {5})).ids, std::vector<uint32_t>({4, 5, 6, 7}));
    EXPECT_EQ(linked(islands, make(ElementKind::Corner, {7})).ids, std::vector<uint32_t>({6, 7, 8, 9, 10, 11}));
    // the cube's faces share no GPU vertex, only logical ones: one island
    const MeshView box = cube();
    EXPECT_EQ(linked(box, make(ElementKind::Face, {0})).ids.size(), 12u);
}

TEST(VertexColorSelect, FlatRegionStopsAtTheAngle) {
    const MeshView box = cube();
    // triangles 4 and 5 are the top face
    EXPECT_EQ(flatRegion(box, 4, 10.0f).ids, std::vector<uint32_t>({4, 5}));
    EXPECT_EQ(flatRegion(box, 4, 91.0f).ids.size(), 12u);
    EXPECT_TRUE(flatRegion(box, 99, 10.0f).empty());
    // a smooth sphere floods along gentle bends only
    const MeshView ball = sphere(12, 16);
    const size_t gentle = flatRegion(ball, 40, 5.0f).ids.size();
    const size_t wide = flatRegion(ball, 40, 40.0f).ids.size();
    EXPECT_LT(gentle, wide);
}

TEST(VertexColorSelect, SimilarColorUsesEveryCornerOrCoverage) {
    const MeshView mesh = quad();
    TargetData data;
    data.base = {Vector4(1, 0, 0, 1), Vector4(1, 0, 0, 1), Vector4(0, 0, 1, 1), Vector4(1, 0, 0, 1)};
    // corner 4 (triangle 1, vertex 3) is blue: vertex 3 no longer matches as a whole
    data.setCorner(4, Vector4(0, 0, 1, 1), mesh.indices);
    ColorMatch red;
    red.value = Vector4(1, 0, 0, 1);
    EXPECT_EQ(similarColor(data, mesh, ElementKind::Vertex, red).ids, std::vector<uint32_t>({0, 1}));
    EXPECT_EQ(similarColor(data, mesh, ElementKind::Face, red).ids, std::vector<uint32_t>({0}));
    red.coverage = 0.3f;
    EXPECT_EQ(similarColor(data, mesh, ElementKind::Face, red).ids, std::vector<uint32_t>({0, 1}));
    EXPECT_EQ(similarColor(data, mesh, ElementKind::Corner, red).ids, std::vector<uint32_t>({0, 1, 2, 3}));
}

TEST(VertexColorSelect, ByNormalAndByChannelRange) {
    const MeshView box = cube();
    EXPECT_EQ(byNormal(box, ElementKind::Face, Vector3(0, 1, 0), 10.0f).ids, std::vector<uint32_t>({4, 5}));
    EXPECT_EQ(byNormal(box, ElementKind::Vertex, Vector3(0, 1, 0), 60.0f).ids.size(), 4u);
    EXPECT_EQ(byNormal(box, ElementKind::Corner, Vector3(0, -1, 0), 10.0f).ids, std::vector<uint32_t>({18, 19, 20, 21, 22, 23}));

    const MeshView mesh = quad();
    TargetData data;
    data.base = {Vector4(0.1f, 0, 0, 1), Vector4(0.5f, 0, 0, 1), Vector4(0.9f, 0, 0, 1), Vector4(0.5f, 0, 0, 1)};
    EXPECT_EQ(byChannelRange(data, mesh, ElementKind::Vertex, 0, 0.4f, 0.6f).ids, std::vector<uint32_t>({1, 3}));
    // triangle 0 averages 0.1, 0.5, 0.5
    EXPECT_EQ(byChannelRange(data, mesh, ElementKind::Face, 0, 0.0f, 0.4f).ids, std::vector<uint32_t>({0}));
}

TEST(VertexColorSelect, AverageValueAndTarget) {
    const MeshView mesh = quad();
    TargetData data;
    data.base = {Vector4(0, 0, 0, 1), Vector4(1, 1, 1, 1), Vector4(1, 1, 1, 1), Vector4(0, 0, 0, 1)};
    // triangle 0's corners: black, black, white
    const Vector4 mean = averageValue(data, mesh, make(ElementKind::Face, {0}));
    EXPECT_FLOAT_EQ(mean.x, 1.0f / 3.0f);
    EXPECT_FLOAT_EQ(mean.w, 1.0f);
    const Target target = toTarget(make(ElementKind::Corner, {1, 2}));
    EXPECT_EQ(target.kind, TargetKind::Corners);
    EXPECT_EQ(target.ids, std::vector<uint32_t>({1, 2}));
    EXPECT_EQ(toTarget(make(ElementKind::Vertex, {})).kind, TargetKind::Vertices);
}

} // namespace vertex_color_select_test

}  // namespace VertexColorSelectTest_cpp
