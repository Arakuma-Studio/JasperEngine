// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R2, R3, R5, R6: the data model.

#include "vertexcolor/VertexColorTypes.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>

using namespace jasperengine;

// (a named namespace, tests included: a unity build merges the test sources)
namespace vertex_color_types_test {
    using namespace jasperengine::vertexcolor;

TEST(VertexColorTypes, DescribeNamesNodeAndPrimitiveOrSubmesh) {
    TargetKey model;
    model.node = 3;
    model.primitive = 0;
    EXPECT_EQ(model.describe(), "node 3 primitive 0");
    TargetKey plain;
    plain.submesh = 1;
    EXPECT_EQ(plain.describe(), "submesh 1");
}

TEST(VertexColorTypes, QuantizeRgba8RoundsToStepsAndClamps) {
    // 0.3 * 255 = 76.5 rounds up; 0.1 * 255 = 25.5 rounds up too
    EXPECT_EQ(quantize(Vector4(0.3f, 0.1f, -0.5f, 2.0f), Format::RGBA8), Vector4(77.0f / 255.0f, 26.0f / 255.0f, 0.0f, 1.0f));
    EXPECT_EQ(quantize(Vector4(128.0f / 255.0f, 0.0f, 1.0f, 0.5f), Format::RGBA8), Vector4(128.0f / 255.0f, 0.0f, 1.0f, 128.0f / 255.0f));
}

TEST(VertexColorTypes, QuantizeRgba16FRoundTripsThroughHalf) {
    const Vector4 q = quantize(Vector4(1.0f / 3.0f, 65504.0f * 4.0f, -1e-9f, 0.1f), Format::RGBA16F);
    // 1/3 as a half: 0x3555 = 0.333251953125; beyond 65504 clamps to the largest half; a value
    // below the smallest subnormal stores as +0
    EXPECT_EQ(q.x, 0.333251953125f);
    EXPECT_EQ(q.y, 65504.0f);
    EXPECT_EQ(q.z, 0.0f);
    EXPECT_FALSE(std::signbit(q.z));
    EXPECT_EQ(q.w, 0.0999755859375f);
    // a subnormal half survives exactly
    const float subnormal = std::ldexp(1.0f, -20);
    EXPECT_EQ(quantize(Vector4(subnormal, 0, 0, 0), Format::RGBA16F).x, subnormal);
    // half of the smallest subnormal step rounds to even (0)
    EXPECT_EQ(quantize(Vector4(std::ldexp(1.0f, -25), 0, 0, 0), Format::RGBA16F).x, 0.0f);
    // 2049 sits between halves 2048 and 2050: to even, 2048
    EXPECT_EQ(quantize(Vector4(2049.0f, 2051.0f, 0, 0), Format::RGBA16F), Vector4(2048.0f, 2052.0f, 0, 0));
}

TEST(VertexColorTypes, QuantizeStoresNanAsZero) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(quantize(Vector4(nan, nan, 0.5f, 1.0f), Format::RGBA8), Vector4(0.0f, 0.0f, 128.0f / 255.0f, 1.0f));
    EXPECT_EQ(quantize(Vector4(nan, 0, 0, 0), Format::RGBA16F).x, 0.0f);
}

TEST(VertexColorTypes, SetCornerKeepsOverridesSortedAndDropsOnesEqualToBase) {
    TargetData data;
    data.base = {Vector4(1, 1, 1, 1), Vector4(0, 0, 0, 1), Vector4(1, 0, 0, 1)};
    const std::vector<uint32_t> indices = {0, 1, 2, 2, 1, 0};
    data.setCorner(4, Vector4(0, 1, 0, 1), indices);
    data.setCorner(1, Vector4(0, 0, 1, 1), indices);
    ASSERT_EQ(data.corners.size(), 2u);
    EXPECT_EQ(data.corners[0].corner, 1u);
    EXPECT_EQ(data.corners[1].corner, 4u);
    EXPECT_EQ(data.cornerValue(1, indices), Vector4(0, 0, 1, 1));
    EXPECT_EQ(data.cornerValue(2, indices), Vector4(1, 0, 0, 1));
    // equal to the base of its vertex (1): no override
    data.setCorner(4, Vector4(0, 0, 0, 1), indices);
    ASSERT_EQ(data.corners.size(), 1u);
    data.setCorner(1, Vector4(0, 0, 0, 1), indices);
    EXPECT_FALSE(data.hasOverrides());
    data.setCorner(3, Vector4(1, 0, 0, 1), indices);
    EXPECT_FALSE(data.hasOverrides());
}

TEST(VertexColorTypes, DataAddsTargetsInKeyOrderAndFindsThem) {
    Data data;
    TargetKey a, b, c;
    a.node = 2;
    a.primitive = 1;
    b.node = 0;
    b.primitive = 0;
    c.submesh = 1;
    data.add(a).base = {Vector4(1, 0, 0, 1)};
    data.add(b);
    data.add(c);
    ASSERT_EQ(data.targets.size(), 3u);
    // keys order by node first: the submesh key (node -1) leads
    EXPECT_EQ(data.targets[0].key, c);
    EXPECT_EQ(data.targets[1].key, b);
    EXPECT_EQ(data.targets[2].key, a);
    // adding an existing key returns it untouched
    EXPECT_EQ(data.add(a).base.size(), 1u);
    EXPECT_EQ(data.targets.size(), 3u);
    ASSERT_NE(data.find(b), nullptr);
    const Data& constData = data;
    EXPECT_EQ(constData.find(a)->base.size(), 1u);
    TargetKey missing;
    missing.node = 9;
    EXPECT_EQ(data.find(missing), nullptr);
}

TEST(VertexColorTypes, InitialValueIsNeverTransparentBlack) {
    const Vector4 custom(0.2f, 0.3f, 0.4f, 0.5f);
    const Vector4 asset(0.5f, 0.5f, 0.5f, 0.25f);
    EXPECT_EQ(initialValueFor(InitialValue::Asset, custom, nullptr), Vector4(1, 1, 1, 1));
    EXPECT_EQ(initialValueFor(InitialValue::Asset, custom, &asset), asset);
    EXPECT_EQ(initialValueFor(InitialValue::White, custom, &asset), Vector4(1, 1, 1, 1));
    EXPECT_EQ(initialValueFor(InitialValue::Black, custom, &asset), Vector4(0, 0, 0, 1));
    EXPECT_EQ(initialValueFor(InitialValue::Custom, custom, &asset), custom);
}

} // namespace vertex_color_types_test
