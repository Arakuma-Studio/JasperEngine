// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R41-R43, R60: from_texture, AO / thickness, curvature and transfer

#include "VertexColorTestMesh.h"
#include "vertexcolor/VertexColorBake.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <span>

// (a named namespace, tests included: a unity build merges the test sources)
namespace vertex_color_bake_test {
    using namespace vctest;

    std::shared_ptr<Image> checker2x2() {
        auto image = std::make_shared<Image>();
        image->width = 2;
        image->height = 2;
        image->wrap = false;
        const Vector4 black(0.0f, 0.0f, 0.0f, 1.0f), white(1.0f, 1.0f, 1.0f, 1.0f);
        image->pixels = {black, white, white, black};
        return image;
    }

    // the clamped bilinear 2 x 2 checker, written out: x(1 - y) + (1 - x)y over texel coordinates
    float checkerAt(float u, float v) {
        const float x = std::clamp(u * 2.0f - 0.5f, 0.0f, 1.0f);
        const float y = std::clamp(v * 2.0f - 0.5f, 0.0f, 1.0f);
        return x * (1.0f - y) + (1.0f - x) * y;
    }

    // the mean of the checker over the part of triangle (a, b, c) where corner a weighs most, by a
    // fine Riemann sum (what the stratified samples approximate)
    float checkerCornerMean(const Vector2& a, const Vector2& b, const Vector2& c) {
        const int n = 600;
        double sum = 0.0;
        int count = 0;
        for (int i = 0; i < n; i++) {
            for (int j = 0; i + j < n; j++) {
                const float wb = ((float)i + 0.25f) / (float)n, wc = ((float)j + 0.25f) / (float)n;
                const float wa = 1.0f - wb - wc;
                if (wa < wb || wa < wc) continue;
                sum += checkerAt(a.x * wa + b.x * wb + c.x * wc, a.y * wa + b.y * wb + c.y * wc);
                count++;
            }
        }
        return (float)(sum / count);
    }

    // a unit quad over UV [0, 1]^2: triangles (0, 1, 2) and (0, 2, 3)
    MeshView uvQuad() {
        MeshView m;
        addVertex(m, Vector3(0, 0, 0), Vector2(0, 0));
        addVertex(m, Vector3(1, 0, 0), Vector2(1, 0));
        addVertex(m, Vector3(1, 1, 0), Vector2(1, 1));
        addVertex(m, Vector3(0, 1, 0), Vector2(0, 1));
        m.indices = {0, 1, 2, 0, 2, 3};
        build(m);
        return m;
    }

    Op textureOp(std::shared_ptr<const Image> image) {
        Op op;
        op.type = OpType::FromTexture;
        op.image = std::move(image);
        return op;
    }

    // a plane of 4 x 4 units (0.5 cells, normal +Y) with a closed box hovering over its centre
    MeshView planeUnderBox() {
        MeshView m;
        addPlane(m, 4.0f, 8, 0.0f);
        addBox(m, Vector3(-0.5f, 0.1f, -0.5f), Vector3(0.5f, 1.1f, 0.5f));
        build(m);
        return m;
    }

    uint32_t vertexAt(const MeshView& m, const Vector3& p) {
        for (uint32_t v = 0; v < m.vertexCount(); v++)
            if (m.positions[v] == p) return v;
        ADD_FAILURE() << "no vertex at " << p.x << " " << p.y << " " << p.z;
        return 0;
    }

    Op aoOp() {
        Op op;
        op.type = OpType::BakeAO;
        op.rays = 64;
        op.distance = 1.0f;
        op.seed = 7;
        return op;
    }

    // a 9 x 9 grid of points 0.25 apart over [-1, 1]^2 at y = 0, normals +Y, values naming the point
    std::shared_ptr<TransferSource> gridSource(const Vector3& normal = Vector3(0, 1, 0)) {
        auto source = std::make_shared<TransferSource>();
        for (int j = 0; j < 9; j++) {
            for (int i = 0; i < 9; i++) {
                source->positions.push_back(Vector3(-1.0f + 0.25f * (float)i, 0.0f, -1.0f + 0.25f * (float)j));
                source->normals.push_back(normal);
                source->values.push_back(Vector4((float)(j * 9 + i), 0.0f, 0.0f, 1.0f));
            }
        }
        return source;
    }

    Op transferOp(std::shared_ptr<const TransferSource> source) {
        Op op;
        op.type = OpType::Transfer;
        op.source = std::move(source);
        op.maxDistance = 0.1f;
        return op;
    }

    // 0.5-spaced target vertices over [-1, 1]^2 at y = 0.01: each sits 0.01 above a source point
    MeshView targetPlane() {
        MeshView m;
        addPlane(m, 2.0f, 4, 0.01f);
        build(m);
        return m;
    }

    uint32_t gridIndexBelow(const Vector3& p) {
        const int i = (int)std::lround((p.x + 1.0f) / 0.25f), j = (int)std::lround((p.z + 1.0f) / 0.25f);
        return (uint32_t)(j * 9 + i);
    }

    uint32_t nextRandom(uint32_t& state) {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
    float randomIn(uint32_t& state, float lo, float hi) { return lo + (hi - lo) * (float)nextRandom(state) / 16777216.0f; }

TEST(VertexColorBake, ImageSampleIsBilinear) {
    const std::shared_ptr<Image> image = checker2x2();
    EXPECT_EQ(image->sample(0.25f, 0.25f).x, 0.0f);   // texel centres
    EXPECT_EQ(image->sample(0.75f, 0.25f).x, 1.0f);
    EXPECT_EQ(image->sample(0.5f, 0.5f).x, 0.5f);
    EXPECT_EQ(image->sample(0.5f, 0.25f).x, 0.5f);
    EXPECT_EQ(image->sample(0.0f, 0.25f).x, 0.0f);    // clamped edge
    EXPECT_EQ(image->sample(-3.0f, 0.25f).x, 0.0f);
    image->wrap = true;
    EXPECT_EQ(image->sample(0.0f, 0.25f).x, 0.5f);    // wraps onto the last column
    EXPECT_EQ(image->sample(1.25f, 0.25f).x, 0.0f);
    EXPECT_EQ(image->sample(-0.25f, 0.25f).x, 1.0f);
    EXPECT_EQ(Image().sample(0.5f, 0.5f), Vector4(1.0f, 1.0f, 1.0f, 1.0f));
}

TEST(VertexColorBake, FromTextureAveragesEachCornersArea) {
    const MeshView m = uvQuad();
    bake::Field field;
    OpResult result;
    std::string error;
    ASSERT_TRUE(bake::fromTexture(m, textureOp(checker2x2()), ApplyOptions(), field, result, error)) << error;
    ASSERT_TRUE(field.perCorner);
    ASSERT_EQ(field.cornerValues.size(), 6u);
    for (uint32_t c = 0; c < 6; c++) {
        const uint32_t t = c / 3, k = c % 3;
        const Vector2& a = m.uv0[m.indices[t * 3 + k]];
        const Vector2& b = m.uv0[m.indices[t * 3 + (k + 1) % 3]];
        const Vector2& d = m.uv0[m.indices[t * 3 + (k + 2) % 3]];
        const float expected = checkerCornerMean(a, b, d);
        EXPECT_NEAR(field.cornerValues[c].x, expected, 0.02f) << "corner " << c;
        EXPECT_EQ(field.cornerValues[c].x, field.cornerValues[c].y);
        EXPECT_EQ(field.cornerValues[c].w, 1.0f);
    }
    // the black texels' corners (uv 0,0 and 1,1) are dark, the white one's light, and the two
    // mirror images in triangle 0 agree
    EXPECT_LT(field.cornerValues[0].x, 0.4f);
    EXPECT_GT(field.cornerValues[1].x, 0.6f);
    EXPECT_NEAR(field.cornerValues[0].x, field.cornerValues[2].x, 1e-5f);
    // vertex 0 has two corners of equal area: their mean
    EXPECT_NEAR(field.vertexValues[0].x, 0.5f * (field.cornerValues[0].x + field.cornerValues[3].x), 1e-6f);
    EXPECT_NEAR(field.vertexValues[1].x, field.cornerValues[1].x, 1e-6f);
    EXPECT_TRUE(field.valid.empty());
    // the texture varies across every corner's area
    EXPECT_GT(result.meanDeltaE, 1.0f);
    EXPECT_GE(result.p95DeltaE, result.meanDeltaE);
    EXPECT_EQ(result.underSampledFaces, 0u);
}

TEST(VertexColorBake, FromTextureOfAUniformTextureIsExact) {
    MeshView m = uvQuad();
    addVertex(m, Vector3(5, 5, 5), Vector2(0.5f, 0.5f));   // in no triangle
    build(m);
    auto image = std::make_shared<Image>();
    image->width = image->height = 64;
    image->pixels.assign(64 * 64, Vector4(0.25f, 0.5f, 0.75f, 1.0f));
    bake::Field field;
    OpResult result;
    std::string error;
    ASSERT_TRUE(bake::fromTexture(m, textureOp(image), ApplyOptions(), field, result, error)) << error;
    for (const Vector4& v : field.cornerValues) EXPECT_EQ(v, Vector4(0.25f, 0.5f, 0.75f, 1.0f));
    EXPECT_EQ(result.meanDeltaE, 0.0f);
    EXPECT_EQ(result.p95DeltaE, 0.0f);
    // each triangle covers 2048 texels: 683 per vertex
    EXPECT_EQ(result.underSampledFaces, 2u);
    ASSERT_EQ(field.valid.size(), 5u);
    EXPECT_EQ(field.valid, (std::vector<uint8_t>{1, 1, 1, 1, 0}));
}

TEST(VertexColorBake, FromTextureRefusesMissingInputs) {
    const MeshView m = uvQuad();
    bake::Field field;
    OpResult result;
    std::string error;
    EXPECT_FALSE(bake::fromTexture(m, textureOp(nullptr), ApplyOptions(), field, result, error));
    EXPECT_NE(error.find("no texture"), std::string::npos) << error;
    Op op = textureOp(checker2x2());
    op.uvSet = 1;
    EXPECT_FALSE(bake::fromTexture(m, op, ApplyOptions(), field, result, error));
    EXPECT_NE(error.find("no UV set 1"), std::string::npos) << error;

    std::atomic<bool> cancel{true};
    ApplyOptions options;
    options.cancel = &cancel;
    EXPECT_FALSE(bake::fromTexture(m, textureOp(checker2x2()), options, field, result, error));
    EXPECT_EQ(error, "cancelled");
}

TEST(VertexColorBake, AmbientOcclusionDarkensUnderABox) {
    const MeshView m = planeUnderBox();
    bake::Field field;
    std::string error;
    float last = -1.0f;
    ApplyOptions options;
    options.progress = [&](float p) {
        EXPECT_GE(p, last);
        last = p;
    };
    ASSERT_TRUE(bake::ambientOcclusion(m, aoOp(), options, false, field, error)) << error;
    EXPECT_EQ(last, 1.0f);
    ASSERT_EQ(field.vertexValues.size(), m.vertexCount());
    EXPECT_FALSE(field.perCorner);
    const Vector4 under = field.vertexValues[vertexAt(m, Vector3(0, 0, 0))];
    const Vector4 open = field.vertexValues[vertexAt(m, Vector3(-2, 0, -2))];
    const Vector4 roof = field.vertexValues[vertexAt(m, Vector3(0.5f, 1.1f, 0.5f))];
    EXPECT_LT(under.x, 0.2f);
    EXPECT_EQ(open.x, 1.0f);
    EXPECT_EQ(roof.x, 1.0f);
    EXPECT_EQ(under.x, under.w) << "grayscale in all four components";
    // every GPU vertex of a logical vertex has its value
    const uint32_t a = vertexAt(m, Vector3(-0.5f, 0.1f, -0.5f));
    for (uint32_t i = m.memberStart[m.logical[a]]; i < m.memberStart[m.logical[a] + 1]; i++)
        EXPECT_EQ(field.vertexValues[m.members[i]], field.vertexValues[a]);

    // the same seed gives the same rays
    bake::Field again;
    ASSERT_TRUE(bake::ambientOcclusion(m, aoOp(), ApplyOptions(), false, again, error)) << error;
    EXPECT_EQ(field.vertexValues, again.vertexValues);
    // a short distance misses the box
    Op shortRays = aoOp();
    shortRays.distance = 0.05f;
    ASSERT_TRUE(bake::ambientOcclusion(m, shortRays, ApplyOptions(), false, again, error)) << error;
    EXPECT_EQ(again.vertexValues[vertexAt(m, Vector3(0, 0, 0))].x, 1.0f);
}

TEST(VertexColorBake, AmbientOcclusionAsksTheSceneInWorldSpace) {
    MeshView m;
    addPlane(m, 4.0f, 2, 0.0f);
    build(m);
    ApplyOptions options;
    options.model = Matrix4::translateMatrix(10.0f, 0.0f, 0.0f);
    uint32_t calls = 0;
    options.sceneOccluded = [&](const Vector3& origin, const Vector3& direction, float maxDistance) {
        calls++;
        EXPECT_GE(origin.x, 7.99f);
        EXPECT_LE(origin.x, 12.01f);
        EXPECT_NEAR(std::sqrt(direction.squaredLength()), 1.0f, 1e-5f);
        EXPECT_GT(direction.y, 0.0f);
        EXPECT_FLOAT_EQ(maxDistance, 1.0f);
        return true;
    };
    Op op = aoOp();
    op.rays = 8;
    bake::Field field;
    std::string error;
    ASSERT_TRUE(bake::ambientOcclusion(m, op, options, false, field, error)) << error;
    EXPECT_EQ(calls, 0u) << "self scope does not ask the scene";
    for (const Vector4& v : field.vertexValues) EXPECT_EQ(v.x, 1.0f);
    op.sceneScope = true;
    ASSERT_TRUE(bake::ambientOcclusion(m, op, options, false, field, error)) << error;
    EXPECT_EQ(calls, 8u * 9u);
    for (const Vector4& v : field.vertexValues) EXPECT_EQ(v.x, 0.0f);
}

TEST(VertexColorBake, ThicknessIsLowOnThinParts) {
    const auto thicknessAtCentre = [](float gap) {
        MeshView m;
        addPlane(m, 4.0f, 4, gap);            // top, facing up
        addPlane(m, 4.0f, 4, 0.0f, true);     // bottom, facing down
        build(m);
        bake::Field field;
        std::string error;
        EXPECT_TRUE(bake::ambientOcclusion(m, aoOp(), ApplyOptions(), true, field, error)) << error;
        return field.vertexValues[vertexAt(m, Vector3(0, gap, 0))].x;
    };
    const float thin = thicknessAtCentre(0.1f);
    const float thick = thicknessAtCentre(0.9f);
    EXPECT_LT(thin, 0.3f);
    EXPECT_GT(thick, 0.6f);
}

TEST(VertexColorBake, AmbientOcclusionRefusesBadInput) {
    MeshView unbuilt;
    addPlane(unbuilt, 1.0f, 1, 0.0f);
    bake::Field field;
    std::string error;
    EXPECT_FALSE(bake::ambientOcclusion(unbuilt, aoOp(), ApplyOptions(), false, field, error));
    EXPECT_NE(error.find("not built"), std::string::npos) << error;
    const MeshView m = planeUnderBox();
    Op op = aoOp();
    op.rays = 0;
    EXPECT_FALSE(bake::ambientOcclusion(m, op, ApplyOptions(), false, field, error));
    EXPECT_NE(error.find("rays"), std::string::npos) << error;
    op = aoOp();
    op.distance = 0.0f;
    EXPECT_FALSE(bake::ambientOcclusion(m, op, ApplyOptions(), true, field, error));
    EXPECT_NE(error.find("bake_thickness: distance"), std::string::npos) << error;
}

TEST(VertexColorBake, CurvatureIsHalfOnFlatGroundAndSignedOnCorners) {
    Op op;
    op.type = OpType::BakeCurvature;
    std::string error;
    bake::Field field;

    MeshView flat;
    addPlane(flat, 2.0f, 4, 0.0f);
    build(flat);
    ASSERT_TRUE(bake::curvature(flat, op, field, error)) << error;
    for (const Vector4& v : field.vertexValues) EXPECT_EQ(v, Vector4(0.5f, 0.5f, 0.5f, 0.5f));

    MeshView cube;
    addBox(cube, Vector3(-1, -1, -1), Vector3(1, 1, 1));
    build(cube);
    ASSERT_TRUE(bake::curvature(cube, op, field, error)) << error;
    for (const Vector4& v : field.vertexValues) EXPECT_GT(v.x, 0.9f);

    MeshView room;
    addBox(room, Vector3(-1, -1, -1), Vector3(1, 1, 1), true);
    build(room);
    ASSERT_TRUE(bake::curvature(room, op, field, error)) << error;
    for (const Vector4& v : field.vertexValues) EXPECT_LT(v.x, 0.1f);

    // contrast scales the distance from 0.5; -1 flattens everything
    op.contrast = -0.9f;
    op.iterations = 3;
    ASSERT_TRUE(bake::curvature(cube, op, field, error)) << error;
    for (const Vector4& v : field.vertexValues) {
        EXPECT_GT(v.x, 0.5f);
        EXPECT_LT(v.x, 0.7f);
    }
    op.contrast = -1.0f;
    ASSERT_TRUE(bake::curvature(cube, op, field, error)) << error;
    for (const Vector4& v : field.vertexValues) EXPECT_EQ(v.x, 0.5f);
}

TEST(VertexColorBake, CurvatureBlurSpreadsAnEdge) {
    // a plane with one row raised into a ridge: the ridge is convex, the flat row past the slope's
    // foot concave, and rings of blur move the farther rows off 0.5
    MeshView m;
    addPlane(m, 4.0f, 8, 0.0f);
    for (Vector3& p : m.positions)
        if (p.z == 0.0f) p.y = 0.5f;
    build(m);
    Op op;
    op.type = OpType::BakeCurvature;
    op.iterations = 0;
    bake::Field sharp, blurred;
    std::string error;
    ASSERT_TRUE(bake::curvature(m, op, sharp, error)) << error;
    op.iterations = 2;
    ASSERT_TRUE(bake::curvature(m, op, blurred, error)) << error;
    const uint32_t ridge = vertexAt(m, Vector3(0, 0.5f, 0));
    const uint32_t toe = vertexAt(m, Vector3(0, 0, 1.0f));
    const uint32_t far = vertexAt(m, Vector3(0, 0, 1.5f));
    EXPECT_GT(sharp.vertexValues[ridge].x, 0.5f);
    EXPECT_LT(sharp.vertexValues[toe].x, 0.5f) << "where the slope's normals turn back up: concave";
    EXPECT_EQ(sharp.vertexValues[far].x, 0.5f);
    EXPECT_NE(blurred.vertexValues[far].x, 0.5f);
    EXPECT_LT(blurred.vertexValues[ridge].x, sharp.vertexValues[ridge].x);
}

TEST(VertexColorBake, TransferTakesTheNearestSourcePoint) {
    const MeshView m = targetPlane();
    bake::Field field;
    OpResult result;
    std::string error;
    ASSERT_TRUE(bake::transfer(m, transferOp(gridSource()), ApplyOptions(), field, result, error)) << error;
    ASSERT_EQ(field.valid.size(), m.vertexCount());
    for (uint32_t v = 0; v < m.vertexCount(); v++) {
        EXPECT_EQ(field.valid[v], 1);
        EXPECT_EQ(field.vertexValues[v].x, (float)gridIndexBelow(m.positions[v])) << v;
    }
    EXPECT_EQ(result.unmatched, 0u);
    EXPECT_NEAR(result.meanDistance, 0.01f, 1e-4f);
}

TEST(VertexColorBake, TransferUnmatchedKeepsOrTakesTheNearest) {
    const MeshView m = targetPlane();
    ApplyOptions options;
    options.model = Matrix4::translateMatrix(3.0f, 0.0f, 0.0f);   // 2 units right of the source
    Op op = transferOp(gridSource());
    bake::Field field;
    OpResult result;
    std::string error;
    ASSERT_TRUE(bake::transfer(m, op, options, field, result, error)) << error;
    EXPECT_EQ(result.unmatched, m.vertexCount());
    for (uint8_t valid : field.valid) EXPECT_EQ(valid, 0);
    EXPECT_EQ(result.meanDistance, 0.0f);

    op.unmatched = Unmatched::Nearest;
    ASSERT_TRUE(bake::transfer(m, op, options, field, result, error)) << error;
    EXPECT_EQ(result.unmatched, m.vertexCount()) << "still reported unmatched";
    for (uint32_t v = 0; v < m.vertexCount(); v++) {
        EXPECT_EQ(field.valid[v], 1);
        // the source's x = 1 column, same row
        const Vector3 p = m.positions[v];
        EXPECT_EQ(field.vertexValues[v].x, (float)gridIndexBelow(Vector3(1.0f, 0.0f, p.z))) << v;
    }
    EXPECT_GT(result.meanDistance, 1.9f);
}

TEST(VertexColorBake, TransferNormalFilterSkipsFacingAwayPoints) {
    const MeshView m = targetPlane();
    bake::Field field;
    OpResult result;
    std::string error;
    Op op = transferOp(gridSource(Vector3(0, -1, 0)));
    ASSERT_TRUE(bake::transfer(m, op, ApplyOptions(), field, result, error)) << error;
    EXPECT_EQ(result.unmatched, m.vertexCount());
    op.normalAngle = 180.0f;
    ASSERT_TRUE(bake::transfer(m, op, ApplyOptions(), field, result, error)) << error;
    EXPECT_EQ(result.unmatched, 0u);

    // a point facing away right under the vertex loses to a farther one facing the same way
    auto source = std::make_shared<TransferSource>();
    source->positions = {Vector3(0, 0, 0), Vector3(0.05f, 0, 0), Vector3(-0.05f, 0, 0)};
    source->normals = {Vector3(0, -1, 0), Vector3(0, 1, 0), Vector3(0, 2, 0)};   // not unit: normalized
    source->values = {Vector4(1, 0, 0, 1), Vector4(2, 0, 0, 1), Vector4(3, 0, 0, 1)};
    ASSERT_TRUE(bake::transfer(m, transferOp(source), ApplyOptions(), field, result, error)) << error;
    const uint32_t centre = vertexAt(m, Vector3(0, 0.01f, 0));
    EXPECT_EQ(field.valid[centre], 1);
    EXPECT_EQ(field.vertexValues[centre].x, 2.0f) << "equal distances: the lower index";
}

TEST(VertexColorBake, TransferGridMatchesBruteForce) {
    uint32_t seed = 12345;
    auto source = std::make_shared<TransferSource>();
    for (int i = 0; i < 3000; i++) {
        source->positions.push_back(Vector3(randomIn(seed, -3, 3), randomIn(seed, -1, 1), randomIn(seed, 0, 0.01f)));
        source->values.push_back(Vector4((float)i, 0, 0, 1));
    }
    // a duplicate point: the lower index wins
    source->positions.push_back(source->positions[17]);
    source->values.push_back(Vector4(3000.0f, 0, 0, 1));
    MeshView m;
    for (int i = 0; i < 400; i++) m.positions.push_back(Vector3(randomIn(seed, -5, 5), randomIn(seed, -2, 2), randomIn(seed, -1, 1)));
    m.positions.push_back(source->positions[17]);

    for (const float limit : {0.0f, 0.3f}) {
        for (const Unmatched mode : {Unmatched::Keep, Unmatched::Nearest}) {
            Op op = transferOp(source);
            op.maxDistance = limit;
            op.unmatched = mode;
            bake::Field field;
            OpResult result;
            std::string error;
            ASSERT_TRUE(bake::transfer(m, op, ApplyOptions(), field, result, error)) << error;
            uint32_t unmatched = 0;
            for (uint32_t v = 0; v < m.vertexCount(); v++) {
                float best = std::numeric_limits<float>::infinity();
                uint32_t bestIndex = 0;
                for (uint32_t i = 0; i < source->positions.size(); i++) {
                    const float d2 = (source->positions[i] - m.positions[v]).squaredLength();
                    if (d2 < best) {
                        best = d2;
                        bestIndex = i;
                    }
                }
                const bool within = best <= limit * limit;
                if (!within) unmatched++;
                if (within || mode == Unmatched::Nearest) {
                    ASSERT_EQ(field.valid[v], 1) << v;
                    EXPECT_EQ(field.vertexValues[v].x, (float)bestIndex) << "vertex " << v << " limit " << limit;
                } else {
                    EXPECT_EQ(field.valid[v], 0) << v;
                }
            }
            EXPECT_EQ(result.unmatched, unmatched);
        }
    }
}

// R10, R60: a source with corner values carries its hard edges. The source is a plane whose faces left
// of x = 0 are red over white vertices; the target the same surface with its vertices in another
// order and its quads cut along the other diagonal (a reimport's new topology)
TEST(VertexColorBake, TransferCarriesHardEdgesFromSourceCorners) {
    const Vector4 red(1, 0, 0, 1), white(1, 1, 1, 1);
    MeshView painted;
    addPlane(painted, 2.0f, 4, 0.0f);
    build(painted);
    auto source = std::make_shared<TransferSource>();
    source->positions = painted.positions;
    source->values.assign(painted.vertexCount(), white);
    source->indices = painted.indices;
    auto leftOf = [](const MeshView& m, uint32_t t) {
        return m.positions[m.indices[t * 3]].x + m.positions[m.indices[t * 3 + 1]].x + m.positions[m.indices[t * 3 + 2]].x < 0.0f;
    };
    for (uint32_t c = 0; c < (uint32_t)painted.indices.size(); c++) source->cornerValues.push_back(leftOf(painted, c / 3) ? red : white);

    MeshView m;
    const uint32_t n = painted.vertexCount(), row = 5;
    for (uint32_t v = 0; v < n; v++) addVertex(m, painted.positions[n - 1 - v]);
    for (uint32_t j = 0; j < 4; j++) {
        for (uint32_t i = 0; i < 4; i++) {
            const uint32_t a = n - 1 - (j * row + i), b = a - 1, c = a - row, d = c - 1;
            m.indices.insert(m.indices.end(), {a, c, d, a, d, b});
        }
    }
    build(m);

    Op op = transferOp(source);
    TargetData data;
    const OpsResult result = applyOps(data, m, std::span<const Op>(&op, 1), ApplyOptions());
    ASSERT_TRUE(result.ok) << result.error;
    for (uint32_t t = 0; t < m.triangleCount(); t++) {
        for (uint32_t k = 0; k < 3; k++) EXPECT_EQ(data.cornerValue(t * 3 + k, m.indices), leftOf(m, t) ? red : white) << "triangle " << t << " corner " << k;
    }
    // the overrides are the edge's: corners of the vertices on x = 0, one side of them
    ASSERT_FALSE(data.corners.empty());
    for (const CornerValue& corner : data.corners) EXPECT_EQ(m.positions[m.indices[corner.corner]].x, 0.0f) << corner.corner;
    EXPECT_EQ(result.ops[0].splitAdded, (uint32_t)data.corners.size());

    // without the source's corners each vertex takes one value: the edge is gone
    source->indices.clear();
    source->cornerValues.clear();
    TargetData smooth;
    ASSERT_TRUE(applyOps(smooth, m, std::span<const Op>(&op, 1), ApplyOptions()).ok);
    EXPECT_FALSE(smooth.hasOverrides());
}

TEST(VertexColorBake, TransferRefusesInconsistentSources) {
    const MeshView m = targetPlane();
    bake::Field field;
    OpResult result;
    std::string error;
    EXPECT_FALSE(bake::transfer(m, transferOp(nullptr), ApplyOptions(), field, result, error));
    auto source = gridSource();
    source->values.pop_back();
    EXPECT_FALSE(bake::transfer(m, transferOp(source), ApplyOptions(), field, result, error));
    EXPECT_NE(error.find("values"), std::string::npos) << error;
    source = gridSource();
    source->normals.pop_back();
    EXPECT_FALSE(bake::transfer(m, transferOp(source), ApplyOptions(), field, result, error));
    EXPECT_NE(error.find("normals"), std::string::npos) << error;
    source = gridSource();
    source->indices = {0, 1, 9};
    source->cornerValues = {Vector4(1, 0, 0, 1)};
    EXPECT_FALSE(bake::transfer(m, transferOp(source), ApplyOptions(), field, result, error));
    EXPECT_NE(error.find("corner values"), std::string::npos) << error;
    source->indices = {0, 1, 81};
    source->cornerValues.assign(3, Vector4(1, 0, 0, 1));
    EXPECT_FALSE(bake::transfer(m, transferOp(source), ApplyOptions(), field, result, error));
    EXPECT_NE(error.find("past its 81 positions"), std::string::npos) << error;
}

} // namespace vertex_color_bake_test
