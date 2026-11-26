// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R80-R82 (AC10): the transient gameplay paint layer

#include "VertexColorTestMesh.h"
#include "vertexcolor/VertexColorRuntimeLayer.h"

#include <gtest/gtest.h>

// (a named namespace, tests included: a unity build merges the test sources)
namespace vertex_color_runtime_test {
    using namespace vctest;

    const Vector4 WHITE(1.0f, 1.0f, 1.0f, 1.0f);
    const Vector4 RED(1.0f, 0.0f, 0.0f, 1.0f);

    // vertices along +X, 0.25 apart: 0, 0.25, ..., 2.0
    MeshView line() {
        MeshView m;
        for (int i = 0; i <= 8; i++) m.positions.push_back(Vector3(0.25f * (float)i, 0.0f, 0.0f));
        return m;
    }

    std::vector<Vector4> combined(const RuntimeLayer& layer, size_t count) {
        const std::vector<Vector4> authored(count, WHITE);
        std::vector<Vector4> out;
        layer.combine(authored, out);
        return out;
    }

    RuntimePaint redDot(float hold, float fade) {
        RuntimePaint p;
        p.radius = 0.1f;
        p.value = RED;
        p.hardness = 1.0f;
        p.hold = hold;
        p.fade = fade;
        return p;
    }

TEST(VertexColorRuntime, HoldThenFadeBackToTheAuthoredValue) {
    const MeshView m = line();
    RuntimeLayer layer;
    EXPECT_FALSE(layer.live());
    EXPECT_EQ(layer.paint(m, Vector3(0, 0, 0), redDot(1.0f, 1.0f), 10.0), 1u);
    EXPECT_TRUE(layer.live());
    EXPECT_TRUE(layer.dirty());
    EXPECT_TRUE(layer.update(10.0));
    std::vector<Vector4> out = combined(layer, m.vertexCount());
    EXPECT_FALSE(layer.dirty());
    EXPECT_EQ(out[0], RED);
    EXPECT_EQ(out[1], WHITE);

    EXPECT_FALSE(layer.update(10.5)) << "holding: nothing changes";
    EXPECT_TRUE(layer.update(11.5));
    out = combined(layer, m.vertexCount());
    EXPECT_FLOAT_EQ(out[0].y, 0.5f) << "halfway through the fade";
    EXPECT_EQ(out[0].x, 1.0f);

    EXPECT_TRUE(layer.update(12.0));
    out = combined(layer, m.vertexCount());
    EXPECT_EQ(out[0], WHITE);
    EXPECT_FALSE(layer.live()) << "nothing left to fade: no per-frame work";
    EXPECT_FALSE(layer.update(13.0));
}

TEST(VertexColorRuntime, RecomputesAtMostTenTimesASecond) {
    const MeshView m = line();
    RuntimeLayer layer;
    layer.paint(m, Vector3(0, 0, 0), redDot(0.0f, 1.0f), 0.0);
    EXPECT_TRUE(layer.update(0.0));
    combined(layer, m.vertexCount());
    EXPECT_FALSE(layer.update(0.05)) << "within 0.1 s of the last recompute";
    EXPECT_EQ(combined(layer, m.vertexCount())[0], RED);
    EXPECT_TRUE(layer.update(0.1));
    EXPECT_NEAR(combined(layer, m.vertexCount())[0].y, 0.1f, 1e-6f);
    EXPECT_FALSE(layer.update(0.15));
    EXPECT_TRUE(layer.update(0.25));
    EXPECT_NEAR(combined(layer, m.vertexCount())[0].y, 0.25f, 1e-6f);
}

TEST(VertexColorRuntime, HoldWithoutFadeVanishesAtTheEnd) {
    const MeshView m = line();
    RuntimeLayer layer;
    layer.paint(m, Vector3(0, 0, 0), redDot(0.5f, 0.0f), 0.0);
    layer.update(0.0);
    EXPECT_EQ(combined(layer, m.vertexCount())[0], RED);
    EXPECT_FALSE(layer.update(0.5));
    EXPECT_TRUE(layer.update(0.7));
    EXPECT_EQ(combined(layer, m.vertexCount())[0], WHITE);
    EXPECT_FALSE(layer.live());
}

TEST(VertexColorRuntime, NegativeHoldIsPermanent) {
    const MeshView m = line();
    RuntimeLayer layer;
    layer.paint(m, Vector3(0, 0, 0), redDot(-1.0f, 1.0f), 0.0);
    EXPECT_FALSE(layer.live()) << "a permanent mark needs no per-frame work";
    EXPECT_TRUE(layer.update(0.0)) << "the paint itself is a change";
    combined(layer, m.vertexCount());
    EXPECT_FALSE(layer.update(1000.0));
    EXPECT_EQ(combined(layer, m.vertexCount())[0], RED);
}

TEST(VertexColorRuntime, FalloffChannelsAndStrength) {
    const MeshView m = line();
    RuntimeLayer layer;
    RuntimePaint p;
    p.radius = 1.0f;
    p.value = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    p.channels = CHANNEL_G;
    p.hardness = 0.5f;
    p.strength = 0.8f;
    // vertices at 0, 0.25, 0.5 are inside the hard core, 0.75 on the slope, 1.0 on the edge (0)
    EXPECT_EQ(layer.paint(m, Vector3(0, 0, 0), p, 0.0), 4u);
    const std::vector<Vector4> out = combined(layer, m.vertexCount());
    for (int v = 0; v <= 2; v++) EXPECT_FLOAT_EQ(out[(size_t)v].y, 0.2f) << v;
    EXPECT_GT(out[3].y, 0.2f);
    EXPECT_LT(out[3].y, 1.0f);
    for (size_t v = 4; v < out.size(); v++) EXPECT_EQ(out[v], WHITE) << v;
    for (const Vector4& v : out) {
        EXPECT_EQ(v.x, 1.0f) << "unlisted channels are never written";
        EXPECT_EQ(v.z, 1.0f);
        EXPECT_EQ(v.w, 1.0f);
    }
}

TEST(VertexColorRuntime, OverlappingPaintsCombineInOneState) {
    const MeshView m = line();
    RuntimeLayer layer;
    RuntimePaint half = redDot(-1.0f, 0.0f);
    half.strength = 0.5f;
    half.value = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
    layer.paint(m, Vector3(0, 0, 0), half, 0.0);
    layer.paint(m, Vector3(0, 0, 0), half, 0.0);
    // two half-strength lerps towards black: 1 -> 0.5 -> 0.25
    EXPECT_FLOAT_EQ(combined(layer, m.vertexCount())[0].x, 0.25f);
}

TEST(VertexColorRuntime, MemoryStaysBoundedOver10kPaints) {
    MeshView m;
    addPlane(m, 4.0f, 20, 0.0f);
    RuntimeLayer layer;
    layer.paint(m, Vector3(0, 0, 0), redDot(0.2f, 0.3f), 0.0);
    const size_t first = layer.memoryBytes();
    EXPECT_GE(first, m.vertexCount() * 4 * sizeof(float));
    for (int i = 0; i < 10000; i++) {
        RuntimePaint p = redDot(i % 3 == 0 ? -1.0f : 0.5f, 0.5f);
        p.radius = 0.3f;
        const float x = -2.0f + 4.0f * (float)(i % 97) / 96.0f, z = -2.0f + 4.0f * (float)(i % 89) / 88.0f;
        layer.paint(m, Vector3(x, 0, z), p, 0.01 * i);
        layer.update(0.01 * i);
    }
    EXPECT_EQ(layer.memoryBytes(), first);
}

TEST(VertexColorRuntime, ClearDropsTheLayer) {
    const MeshView m = line();
    RuntimeLayer layer;
    layer.paint(m, Vector3(0, 0, 0), redDot(5.0f, 1.0f), 0.0);
    combined(layer, m.vertexCount());
    const size_t empty = RuntimeLayer().memoryBytes();
    EXPECT_GT(layer.memoryBytes(), empty);
    layer.clear();
    EXPECT_FALSE(layer.live());
    EXPECT_TRUE(layer.dirty()) << "the authored values have to go back up";
    EXPECT_EQ(layer.memoryBytes(), empty);
    EXPECT_EQ(combined(layer, m.vertexCount())[0], WHITE);
    EXPECT_FALSE(layer.update(1.0));
    EXPECT_EQ(layer.paint(m, Vector3(10, 0, 0), redDot(1.0f, 1.0f), 2.0), 0u);
}

} // namespace vertex_color_runtime_test
