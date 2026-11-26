// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R3, R4, R20-R28, R30-R34, R44-R49, R54, D3-D6, AC1, AC2, AC8: the
// op engine on fixed meshes.

#include "VertexColorTestSupport.h"

#include "vertexcolor/VertexColorInternal.h"

#include <algorithm>
#include <atomic>
#include <memory>

using namespace jasperengine;

// (a named namespace, tests included: a unity build merges the test sources)
namespace vertex_color_ops_test {
    using namespace jasperengine::vertexcolor;
    using namespace jasperengine::vertexcolor::test;

    const Vector4 kRed(1, 0, 0, 1);
    const Vector4 kGreen(0, 1, 0, 1);
    const Vector4 kBlue(0, 0, 1, 1);
    const Vector4 kWhite(1, 1, 1, 1);
    const Vector4 kBlack(0, 0, 0, 1);

    void expectNear(const Vector4& a, const Vector4& b, float tolerance = 2e-3f) {
        EXPECT_NEAR(a.x, b.x, tolerance);
        EXPECT_NEAR(a.y, b.y, tolerance);
        EXPECT_NEAR(a.z, b.z, tolerance);
        EXPECT_NEAR(a.w, b.w, tolerance);
    }

    Vector4 logicalValue(const TargetData& data, const MeshView& mesh, uint32_t l) {
        std::vector<Vector4> base, corners;
        detail::denseValues(data, mesh, base, corners);
        return detail::logicalMean(mesh, base, corners, l);
    }

    // the logical vertex of the grid at (x, z)
    uint32_t gridLogical(const MeshView& mesh, int x, int z) {
        return mesh.logical[gridVertex(x, z)];
    }

    Op strokeOp(std::vector<Vector3> points, float radius, const Vector4& value) {
        Op op;
        op.type = OpType::Stroke;
        for (const Vector3& p : points) op.points.push_back(StrokePoint{p, 1.0f});
        op.radius = radius;
        op.value = value;
        return op;
    }

// ---- R3, R4, R24: elements and overrides

TEST(VertexColorOps, FillAllWritesEveryVertexWithoutOverrides) {
    const MeshView mesh = quad();
    TargetData data;
    const OpsResult result = apply(data, mesh, {fillOp(kRed)}, localOptions(Format::RGBA8));
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(data.base.size(), 4u);
    for (const Vector4& v : data.base) EXPECT_EQ(v, kRed);
    EXPECT_FALSE(data.hasOverrides());
    ASSERT_EQ(result.ops.size(), 1u);
    EXPECT_EQ(result.ops[0].inTarget, 4u);
    EXPECT_EQ(result.ops[0].changedVertices, 4u);
    EXPECT_EQ(result.ops[0].changedCorners, 0u);
    EXPECT_EQ(result.hash, hashTarget(data));
    EXPECT_EQ(hex(result.hash), "2dca8505c7855da7");
    EXPECT_TRUE(result.warnings.empty());
}

TEST(VertexColorOps, FaceTargetCreatesCornerOverridesOnSmoothMesh) {
    const MeshView mesh = sphere();
    TargetData data;
    Op op;
    op.type = OpType::Set;
    op.value = kRed;
    op.target = idsTarget(TargetKind::Faces, {10});
    const OpsResult result = apply(data, mesh, {op}, localOptions(Format::RGBA8));
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(data.corners.size(), 3u);
    for (uint32_t k = 0; k < 3; k++) {
        EXPECT_EQ(data.corners[k].corner, 30u + k);
        EXPECT_EQ(data.corners[k].value, kRed);
    }
    for (const Vector4& v : data.base) EXPECT_EQ(v, kWhite);
    EXPECT_EQ(result.ops[0].splitAdded, 3u);
    EXPECT_EQ(result.ops[0].changedCorners, 3u);
    EXPECT_EQ(result.ops[0].changedVertices, 3u);
    ASSERT_EQ(result.warnings.size(), 1u);
    EXPECT_EQ(result.warnings[0].code, "split_vertices_added");
}

TEST(VertexColorOps, VertexTargetClearsOverrides) {
    const MeshView mesh = sphere();
    TargetData data;
    Op face;
    face.type = OpType::Set;
    face.value = kRed;
    face.target = idsTarget(TargetKind::Faces, {10});
    ASSERT_TRUE(apply(data, mesh, {face}, localOptions(Format::RGBA8)).ok);
    ASSERT_EQ(data.corners.size(), 3u);
    // R4: setting the logical vertices of those corners sets every corner of every member
    std::vector<uint32_t> logicals;
    for (uint32_t k = 0; k < 3; k++) logicals.push_back(mesh.logical[mesh.indices[30 + k]]);
    Op vertices;
    vertices.type = OpType::Set;
    vertices.value = kGreen;
    vertices.target = idsTarget(TargetKind::Vertices, logicals);
    const OpsResult result = apply(data, mesh, {vertices}, localOptions(Format::RGBA8));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(data.hasOverrides());
    EXPECT_EQ(result.ops[0].changedCorners, 3u);
    EXPECT_EQ(result.ops[0].splitAdded, 0u);
    EXPECT_EQ(result.ops[0].changedVertices, 3u);
    for (uint32_t l : logicals) {
        for (uint32_t m = mesh.memberStart[l]; m < mesh.memberStart[l + 1]; m++) EXPECT_EQ(data.base[mesh.members[m]], kGreen);
    }
    EXPECT_TRUE(result.warnings.empty()) << "the target had overrides before: no split warning";
}

TEST(VertexColorOps, CornerWriteEqualToBaseLeavesNoOverride) {
    const MeshView mesh = cube();
    TargetData data;
    Op op;
    op.type = OpType::Set;
    op.value = kWhite;
    op.target = idsTarget(TargetKind::Corners, {0, 5, 17});
    const OpsResult result = apply(data, mesh, {op}, localOptions(Format::RGBA8));
    ASSERT_TRUE(result.ok);
    EXPECT_FALSE(data.hasOverrides());
    EXPECT_EQ(result.ops[0].changedCorners, 0u);
    EXPECT_EQ(result.ops[0].changedVertices, 0u);
    EXPECT_EQ(result.ops[0].inTarget, 3u);
}

TEST(VertexColorOps, VertexWriteReachesEverySeamMember) {
    const MeshView mesh = cube();
    TargetData data;
    Op op;
    op.type = OpType::Set;
    op.value = kBlue;
    op.target = idsTarget(TargetKind::Vertices, {0});
    ASSERT_TRUE(apply(data, mesh, {op}, localOptions(Format::RGBA8)).ok);
    uint32_t blue = 0;
    for (uint32_t v = 0; v < 24; v++) {
        if (data.base[v] == kBlue) {
            blue++;
            EXPECT_EQ(mesh.logical[v], 0u);
        }
    }
    EXPECT_EQ(blue, 3u);
    EXPECT_FALSE(data.hasOverrides());
}

TEST(VertexColorOps, FaceElementShapeTakesWholeFacesOnly) {
    const MeshView mesh = grid();
    TargetData data;
    Op op;
    op.type = OpType::Set;
    op.value = kRed;
    op.target.kind = TargetKind::Box;
    op.target.center = Vector3(0.05f, 0.0f, 0.05f);
    op.target.halfExtents = Vector3(0.06f, 1.0f, 0.06f);
    op.target.element = Element::Face;
    const OpsResult result = apply(data, mesh, {op}, localOptions(Format::RGBA8));
    ASSERT_TRUE(result.ok);
    // the first cell's two triangles hold all three corners inside; their neighbours do not
    EXPECT_EQ(result.ops[0].inTarget, 2u);
    EXPECT_EQ(data.corners.size(), 6u);
    for (const CornerValue& entry : data.corners) EXPECT_LT(entry.corner, 6u);
}

TEST(VertexColorOps, ChannelsLeaveUnlistedChannelsAlone) {
    const MeshView mesh = quad();
    TargetData data;
    Op op = fillOp(Vector4(0.0f, 0.5f, 0.0f, 0.0f));
    op.channels = CHANNEL_G | CHANNEL_A;
    ASSERT_TRUE(apply(data, mesh, {op}, localOptions()).ok);
    for (const Vector4& v : data.base) EXPECT_EQ(v, Vector4(1.0f, 0.5f, 1.0f, 0.0f));
}

TEST(VertexColorOps, BlendModesFollowTheirFormulas) {
    const Vector4 dst(0.2f, 0.4f, 0.8f, 1.0f);
    const Vector4 src(0.5f, 0.5f, 0.5f, 0.5f);
    struct Case {
        Blend blend;
        Vector4 expected;
    };
    const Case cases[] = {
        {Blend::Replace, Vector4(0.5f, 0.5f, 0.5f, 0.5f)},
        {Blend::Add, Vector4(0.7f, 0.9f, 1.3f, 1.5f)},
        {Blend::Subtract, Vector4(-0.3f, -0.1f, 0.3f, 0.5f)},
        {Blend::Multiply, Vector4(0.1f, 0.2f, 0.4f, 0.5f)},
        {Blend::Screen, Vector4(0.6f, 0.7f, 0.9f, 1.0f)},
        {Blend::Overlay, Vector4(0.2f, 0.4f, 0.8f, 1.0f)},
        {Blend::Min, Vector4(0.2f, 0.4f, 0.5f, 0.5f)},
        {Blend::Max, Vector4(0.5f, 0.5f, 0.8f, 1.0f)},
        {Blend::Lerp, Vector4(0.35f, 0.45f, 0.65f, 0.75f)},
    };
    for (const Case& c : cases) {
        const Vector4 full = blendValue(dst, src, c.blend, 1.0f, CHANNELS_RGBA);
        expectNear(full, c.expected, 1e-6f);
        // strength scales the change; masked channels are untouched
        const Vector4 halfway = blendValue(dst, src, c.blend, 0.5f, CHANNELS_RGB);
        EXPECT_NEAR(halfway.x, dst.x + (c.expected.x - dst.x) * 0.5f, 1e-6f) << (int)c.blend;
        EXPECT_EQ(halfway.w, dst.w);
    }
}

TEST(VertexColorOps, StrengthScalesTheChange) {
    const MeshView mesh = quad();
    TargetData data;
    Op op = fillOp(kBlack);
    op.strength = 0.25f;
    ASSERT_TRUE(apply(data, mesh, {op}, localOptions()).ok);
    expectNear(data.base[0], Vector4(0.75f, 0.75f, 0.75f, 1.0f), 1e-6f);
}

// ---- R26: all or nothing

TEST(VertexColorOps, BadOpRollsTheBatchBackAndNamesIt) {
    const MeshView mesh = quad();
    TargetData data;
    ASSERT_TRUE(apply(data, mesh, {fillOp(kBlue)}, localOptions(Format::RGBA8)).ok);
    const TargetData before = data;
    Op bad;
    bad.type = OpType::Set;
    bad.target = idsTarget(TargetKind::Vertices, {2, 9});
    const OpsResult result = apply(data, mesh, {fillOp(kRed), bad}, localOptions(Format::RGBA8));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.failedOp, 1u);
    EXPECT_EQ(result.error, "ops[1].target.ids: 9 is out of range; the target has 4 vertices (0..3); read valid ids with sample_mesh_colors elements");
    EXPECT_EQ(hashTarget(data), hashTarget(before));
    EXPECT_EQ(data.base, before.base);
    // Lua counts from 1
    ApplyOptions lua = localOptions(Format::RGBA8);
    lua.firstIndex = 1;
    EXPECT_EQ(apply(data, mesh, {fillOp(kRed), bad}, lua).error.rfind("ops[2].target.ids: ", 0), 0u);
}

TEST(VertexColorOps, FailureOnAFreshTargetLeavesItEmpty) {
    const MeshView mesh = quad();
    TargetData data;
    Op stroke = strokeOp({Vector3(0, 0, 0)}, 0.0f, kRed);
    const OpsResult result = apply(data, mesh, {fillOp(kRed), stroke}, localOptions());
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "ops[1].radius: must be > 0, got 0; give the brush radius in world units");
    EXPECT_TRUE(data.base.empty());
}

TEST(VertexColorOps, DryRunReportsTheResultAndWritesNothing) {
    const MeshView mesh = sphere();
    TargetData data;
    Op face;
    face.type = OpType::Set;
    face.value = kRed;
    face.target = idsTarget(TargetKind::Faces, {1, 2, 3});
    const OpsResult dry = apply(data, mesh, {face}, localOptions(Format::RGBA8), true);
    ASSERT_TRUE(dry.ok);
    EXPECT_TRUE(data.base.empty());
    TargetData copy;
    const OpsResult real = apply(copy, mesh, {face}, localOptions(Format::RGBA8));
    EXPECT_EQ(dry.hash, real.hash);
    EXPECT_EQ(dry.ops[0].splitAdded, real.ops[0].splitAdded);
    EXPECT_EQ(dry.after.mean, real.after.mean);
}

TEST(VertexColorOps, CancelLeavesTheDataUnchanged) {
    const MeshView mesh = quad();
    TargetData data;
    std::atomic<bool> cancel{true};
    ApplyOptions options = localOptions();
    options.cancel = &cancel;
    const OpsResult result = apply(data, mesh, {fillOp(kRed)}, options);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.cancelled);
    EXPECT_TRUE(data.base.empty());
}

TEST(VertexColorOps, StaleTargetIsRefusedWithTheFix) {
    const MeshView mesh = quad();
    TargetData data;
    data.base.assign(3, kWhite);
    const OpsResult result = apply(data, mesh, {fillOp(kRed)}, localOptions());
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("3 values for a mesh of 4 vertices"), std::string::npos) << result.error;
}

// ---- R5, R6: formats and the start value

TEST(VertexColorOps, ValuesAreQuantizedToTheFormat) {
    const MeshView mesh = quad();
    TargetData bytes, halves;
    ASSERT_TRUE(apply(bytes, mesh, {fillOp(Vector4(0.3f, 0.1f, 0.7f, 1.0f))}, localOptions(Format::RGBA8)).ok);
    EXPECT_EQ(bytes.base[0], Vector4(77.0f / 255.0f, 26.0f / 255.0f, 179.0f / 255.0f, 1.0f));
    ASSERT_TRUE(apply(halves, mesh, {fillOp(Vector4(0.3f, 0.1f, 0.7f, 1.0f))}, localOptions(Format::RGBA16F)).ok);
    EXPECT_EQ(halves.base[0], half(Vector4(0.3f, 0.1f, 0.7f, 1.0f)));
    EXPECT_NE(hashTarget(bytes), hashTarget(halves));
}

TEST(VertexColorOps, FreshTargetStartsFromTheInitialValue) {
    MeshView mesh = quad();
    mesh.assetColors = {kRed, kGreen, kBlue, kWhite};
    ASSERT_TRUE(mesh.build());
    Op noop = fillOp(kBlack);
    noop.strength = 0.0f;
    TargetData asset;
    ApplyOptions options = localOptions(Format::RGBA8);
    options.initial = InitialValue::Asset;
    ASSERT_TRUE(apply(asset, mesh, {noop}, options).ok);
    EXPECT_EQ(asset.base, mesh.assetColors);
    TargetData black;
    options.initial = InitialValue::Black;
    ASSERT_TRUE(apply(black, mesh, {noop}, options).ok);
    for (const Vector4& v : black.base) EXPECT_EQ(v, kBlack);
    TargetData initialized;
    initializeTarget(initialized, mesh, InitialValue::Custom, Vector4(0, 0, 1, 0.5f));
    EXPECT_EQ(initialized.base[3], Vector4(0, 0, 1, 0.5f));
}

// ---- R30-R35: strokes

TEST(VertexColorOps, StrokeCapHoldsAcrossApplyCalls) {
    // AC8: crossing a vertex again and again at 30 % moves it 30 % of the way, across calls
    const MeshView mesh = grid();
    const uint32_t centre = gridLogical(mesh, 5, 5);
    TargetData data;
    StrokeState state;
    ApplyOptions options = localOptions();
    options.stroke = &state;
    for (int call = 0; call < 5; call++) {
        Op op = strokeOp({Vector3(0.2f, 0, 0.5f), Vector3(0.8f, 0, 0.5f), Vector3(0.2f, 0, 0.5f)}, 0.15f, kBlack);
        op.strength = 0.3f;
        op.falloff = FalloffPreset::Constant;
        ASSERT_TRUE(apply(data, mesh, {op}, options).ok);
    }
    EXPECT_EQ(logicalValue(data, mesh, centre), half(Vector4(0.7f, 0.7f, 0.7f, 1.0f)));
    EXPECT_TRUE(state.hasLastPoint);
}

TEST(VertexColorOps, StrokeBuildupAccumulatesPerStamp) {
    const MeshView mesh = grid();
    const uint32_t centre = gridLogical(mesh, 5, 5);
    TargetData data;
    StrokeState state;
    ApplyOptions options = localOptions();
    options.stroke = &state;
    Op op = strokeOp({Vector3(0.2f, 0, 0.5f), Vector3(0.8f, 0, 0.5f), Vector3(0.2f, 0, 0.5f)}, 0.15f, kBlack);
    op.strength = 0.3f;
    op.falloff = FalloffPreset::Constant;
    op.buildup = true;
    ASSERT_TRUE(apply(data, mesh, {op}, options).ok);
    EXPECT_LT(logicalValue(data, mesh, centre).x, 0.3f);
}

TEST(VertexColorOps, StrokeWithoutStateCapsWithinTheOp) {
    const MeshView mesh = grid();
    TargetData data;
    Op op = strokeOp({Vector3(0.2f, 0, 0.5f), Vector3(0.8f, 0, 0.5f), Vector3(0.2f, 0, 0.5f)}, 0.15f, kBlack);
    op.strength = 0.4f;
    op.falloff = FalloffPreset::Constant;
    ASSERT_TRUE(apply(data, mesh, {op, op}, localOptions()).ok);
    // two ops are two strokes: 1 - 0.4, then 0.6 - 0.4 * 0.6
    EXPECT_EQ(logicalValue(data, mesh, gridLogical(mesh, 5, 5)), half(Vector4(0.36f, 0.36f, 0.36f, 1.0f)));
}

TEST(VertexColorOps, StrokeStampsByArcLengthAndCarriesSpacingOver) {
    const MeshView mesh = grid();
    TargetData data;
    StrokeState state;
    ApplyOptions options = localOptions();
    options.stroke = &state;
    // radius 0.5, spacing 0.25: a stamp every 0.125 along the path
    Op op = strokeOp({Vector3(0, 0, 0), Vector3(1, 0, 0)}, 0.5f, kBlack);
    op.strength = 0.01f;
    ASSERT_TRUE(apply(data, mesh, {op}, options).ok);
    EXPECT_EQ(state.lastStamp, Vector3(1, 0, 0));
    EXPECT_FLOAT_EQ(state.carry, 0.0f);
    // a corner in the path: 0.1 along z then 0.2 along x, 0.3 in all, stamps at 0.125 and 0.25
    op.points = {StrokePoint{Vector3(1, 0, 0.1f), 1.0f}, StrokePoint{Vector3(1.2f, 0, 0.1f), 1.0f}};
    ASSERT_TRUE(apply(data, mesh, {op}, options).ok);
    EXPECT_NEAR(state.lastStamp.x, 1.15f, 1e-6f);
    EXPECT_NEAR(state.lastStamp.z, 0.1f, 1e-6f);
    EXPECT_NEAR(state.carry, 0.05f, 1e-6f);
    EXPECT_EQ(state.lastPoint, Vector3(1.2f, 0, 0.1f));
}

TEST(VertexColorOps, StrokeFalloffPresetsShapeTheWeight) {
    const MeshView mesh = grid();
    struct Case {
        FalloffPreset falloff;
        float hardness;
        float expected[5];   // weight at x = 0, 0.2, 0.4, 0.6, 0.8 of the radius
    };
    auto smooth = [](float t) { return 1.0f - t * t * (3.0f - 2.0f * t); };
    const Case cases[] = {
        {FalloffPreset::Constant, 0.0f, {1, 1, 1, 1, 1}},
        {FalloffPreset::Linear, 0.0f, {1, 0.8f, 0.6f, 0.4f, 0.2f}},
        {FalloffPreset::Smooth, 0.0f, {1, smooth(0.2f), smooth(0.4f), smooth(0.6f), smooth(0.8f)}},
        {FalloffPreset::Sphere, 0.0f, {1, std::sqrt(1 - 0.04f), std::sqrt(1 - 0.16f), std::sqrt(1 - 0.36f), std::sqrt(1 - 0.64f)}},
        {FalloffPreset::Linear, 0.5f, {1, 1, 1, 0.8f, 0.4f}},
    };
    for (const Case& c : cases) {
        TargetData data;
        Op op = strokeOp({Vector3(0.5f, 0, 0.5f)}, 0.5f, kBlack);
        op.falloff = c.falloff;
        op.hardness = c.hardness;
        ASSERT_TRUE(apply(data, mesh, {op}, localOptions()).ok);
        for (int k = 0; k < 5; k++) {
            const float value = logicalValue(data, mesh, gridLogical(mesh, 5 + k, 5)).x;
            EXPECT_NEAR(value, 1.0f - c.expected[k], 2e-3f) << "falloff " << (int)c.falloff << " hardness " << c.hardness << " step " << k;
        }
        EXPECT_EQ(logicalValue(data, mesh, gridLogical(mesh, 10, 10)), kWhite) << "outside the radius";
    }
    // a custom curve: half weight everywhere inside
    TargetData data;
    Op op = strokeOp({Vector3(0.5f, 0, 0.5f)}, 0.5f, kBlack);
    op.falloff = FalloffPreset::Custom;
    op.falloffCurve = Curve::polyline({{0.0f, 0.5f}, {1.0f, 0.5f}});
    ASSERT_TRUE(apply(data, mesh, {op}, localOptions()).ok);
    EXPECT_NEAR(logicalValue(data, mesh, gridLogical(mesh, 7, 5)).x, 0.5f, 2e-3f);
}

TEST(VertexColorOps, StrokePressureScalesStrengthAndSize) {
    const MeshView mesh = grid();
    TargetData data;
    Op op = strokeOp({Vector3(0.5f, 0, 0.5f)}, 0.5f, kBlack);
    op.points[0].pressure = 0.5f;
    op.falloff = FalloffPreset::Constant;
    op.pressureSize = true;
    ASSERT_TRUE(apply(data, mesh, {op}, localOptions()).ok);
    // half strength, half radius (0.25): (7, 5) is 0.2 away, (8, 5) 0.3
    EXPECT_NEAR(logicalValue(data, mesh, gridLogical(mesh, 7, 5)).x, 0.5f, 2e-3f);
    EXPECT_EQ(logicalValue(data, mesh, gridLogical(mesh, 8, 5)), kWhite);
}

TEST(VertexColorOps, StrokeModesEraseSmoothAndSharpen) {
    const MeshView mesh = grid();
    TargetData data;
    ApplyOptions options = localOptions();
    ASSERT_TRUE(apply(data, mesh, {fillOp(kRed)}, options).ok);
    Op erase = strokeOp({Vector3(0.5f, 0, 0.5f)}, 0.25f, kBlack);
    erase.mode = StrokeMode::Erase;
    erase.falloff = FalloffPreset::Constant;
    ASSERT_TRUE(apply(data, mesh, {erase}, options).ok);
    EXPECT_EQ(logicalValue(data, mesh, gridLogical(mesh, 5, 5)), kWhite) << "erase goes back to the initial value";
    EXPECT_EQ(logicalValue(data, mesh, gridLogical(mesh, 9, 5)), kRed);

    // smooth: a black dot surrounded by white moves towards its neighbours
    TargetData dot;
    ASSERT_TRUE(apply(dot, mesh, {fillOp(kBlack, idsTarget(TargetKind::Vertices, {gridLogical(mesh, 5, 5)}))}, options).ok);
    Op smooth = strokeOp({Vector3(0.5f, 0, 0.5f)}, 0.05f, kBlack);
    smooth.mode = StrokeMode::Smooth;
    smooth.falloff = FalloffPreset::Constant;
    TargetData smoothed = dot;
    ASSERT_TRUE(apply(smoothed, mesh, {smooth}, options).ok);
    EXPECT_EQ(logicalValue(smoothed, mesh, gridLogical(mesh, 5, 5)), kWhite);
    smooth.mode = StrokeMode::Sharpen;
    smooth.radius = 0.15f;
    TargetData sharpened = dot;
    ASSERT_TRUE(apply(sharpened, mesh, {smooth}, options).ok);
    // a neighbour of the dot was 1 with a neighbour mean below 1: it moves up, away from it
    EXPECT_GT(logicalValue(sharpened, mesh, gridLogical(mesh, 6, 5)).x, 1.0f);
}

TEST(VertexColorOps, StrokeStencilMultipliesTheFalloff) {
    const MeshView mesh = grid();
    TargetData data;
    auto stencil = std::make_shared<Image>();
    stencil->width = 2;
    stencil->height = 1;
    stencil->pixels = {Vector4(0, 0, 0, 1), Vector4(0, 0, 0, 1)};
    Op op = strokeOp({Vector3(0.5f, 0, 0.5f)}, 0.3f, kBlack);
    op.falloff = FalloffPreset::Constant;
    op.stencil = stencil;
    ASSERT_TRUE(apply(data, mesh, {op}, localOptions()).ok);
    EXPECT_EQ(logicalValue(data, mesh, gridLogical(mesh, 5, 5)), kWhite) << "a black stencil masks everything";
    stencil->pixels = {Vector4(0.5f, 0.5f, 0.5f, 1), Vector4(0.5f, 0.5f, 0.5f, 1)};
    ASSERT_TRUE(apply(data, mesh, {op}, localOptions()).ok);
    EXPECT_NEAR(logicalValue(data, mesh, gridLogical(mesh, 5, 5)).x, 0.5f, 2e-3f);
}

TEST(VertexColorOps, StrokeRadiusIsMeasuredInWorldSpace) {
    const MeshView mesh = grid();
    TargetData data;
    ApplyOptions options = localOptions();
    options.world = true;
    options.model = Matrix4::scaleMatrix(Vector3(2, 2, 2));
    // world (1, 0, 1) is the local centre; world radius 0.25 reaches local 0.125
    Op op = strokeOp({Vector3(1, 0, 1)}, 0.25f, kBlack);
    op.falloff = FalloffPreset::Constant;
    ASSERT_TRUE(apply(data, mesh, {op}, options).ok);
    EXPECT_EQ(logicalValue(data, mesh, gridLogical(mesh, 6, 5)), kBlack);
    EXPECT_EQ(logicalValue(data, mesh, gridLogical(mesh, 7, 5)), kWhite);
}

// ---- R25: filters

TEST(VertexColorOps, FiltersNarrowTheTargetAndInvert) {
    const MeshView mesh = cube();
    auto run = [&](Filter filter, Element element) {
        TargetData data;
        Op op = fillOp(kRed);
        op.target.element = element;
        op.filters = {filter};
        const OpsResult result = apply(data, mesh, {op}, localOptions(Format::RGBA8));
        EXPECT_TRUE(result.ok) << result.error;
        return result.ops.empty() ? OpResult() : result.ops[0];
    };
    Filter facing;
    facing.kind = FilterKind::Facing;
    facing.angle = 10.0f;
    OpResult r = run(facing, Element::Face);
    EXPECT_EQ(r.inTarget, 12u);
    EXPECT_EQ(r.filteredOut, 10u);
    facing.invert = true;
    EXPECT_EQ(run(facing, Element::Face).filteredOut, 2u);

    Filter height;
    height.kind = FilterKind::Height;
    height.min = 0.0f;
    height.max = 1.0f;
    EXPECT_EQ(run(height, Element::Vertex).filteredOut, 4u);
    height.invert = true;
    EXPECT_EQ(run(height, Element::Vertex).filteredOut, 4u);

    Filter slope;
    slope.kind = FilterKind::Slope;
    slope.min = 0.0f;
    slope.max = 60.0f;   // the top corners' normals are 54.7 degrees from up
    EXPECT_EQ(run(slope, Element::Vertex).filteredOut, 4u);

    Filter curvature;
    curvature.kind = FilterKind::Curvature;
    curvature.min = 0.1f;
    curvature.max = 1.0f;   // every cube corner is convex
    EXPECT_EQ(run(curvature, Element::Vertex).filteredOut, 0u);
    curvature.invert = true;
    EXPECT_EQ(run(curvature, Element::Vertex).filteredOut, 8u);
}

TEST(VertexColorOps, OccludedFilterAsksTheVisibilityHook) {
    const MeshView mesh = cube();
    TargetData data;
    ApplyOptions options = localOptions(Format::RGBA8);
    options.world = true;
    options.model = Matrix4::translateMatrix(Vector3(10, 0, 0));
    // world points: the hook sees the model applied
    options.visible = [](const Vector3& p) { return p.x > 10.0f; };
    Op op = fillOp(kRed);
    Filter occluded;
    occluded.kind = FilterKind::Occluded;
    op.filters = {occluded};
    const OpsResult result = apply(data, mesh, {op}, options);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.ops[0].filteredOut, 4u);
    for (uint32_t l = 0; l < 8; l++) EXPECT_EQ(logicalValue(data, mesh, l), mesh.logicalPositions[l].x > 0 ? kRed : kWhite);
}

TEST(VertexColorOps, ChannelFilterReadsTheCurrentValues) {
    const MeshView mesh = quad();
    TargetData data;
    ApplyOptions options = localOptions(Format::RGBA8);
    Op op = fillOp(kBlack, idsTarget(TargetKind::Vertices, {0, 1}));
    Op green = fillOp(kGreen);
    Filter channel;
    channel.kind = FilterKind::Channel;
    channel.channel = 0;
    channel.min = 0.5f;
    channel.max = 1.0f;
    green.filters = {channel};
    ASSERT_TRUE(apply(data, mesh, {op, green}, options).ok);
    EXPECT_EQ(logicalValue(data, mesh, 0), kBlack);
    EXPECT_EQ(logicalValue(data, mesh, 2), kGreen);
}

// ---- R23: shapes and the color target

TEST(VertexColorOps, ShapeTargetsSelectInsideThem) {
    const MeshView mesh = grid();
    const TargetData data;
    ApplyOptions options = localOptions();
    ElementKind kind;
    Target sphere;
    sphere.kind = TargetKind::Sphere;
    sphere.center = Vector3(0.5f, 0, 0.5f);
    sphere.radius = 0.105f;
    std::vector<uint32_t> ids = resolveElements(data, mesh, sphere, {}, options, kind);
    EXPECT_EQ(kind, ElementKind::Vertex);
    EXPECT_EQ(ids.size(), 5u);
    Target box;
    box.kind = TargetKind::Box;
    box.center = Vector3(0.5f, 0, 0.5f);
    box.halfExtents = Vector3(0.15f, 0.1f, 0.01f);
    box.rotation = detail::eulerDegrees(Vector3(0, 90, 0));
    // rotated a quarter turn: long along z
    ids = resolveElements(data, mesh, box, {}, options, kind);
    ASSERT_EQ(ids.size(), 3u);
    for (uint32_t l : ids) EXPECT_NEAR(mesh.logicalPositions[l].x, 0.5f, 1e-6f);
    Target capsule;
    capsule.kind = TargetKind::Capsule;
    capsule.from = Vector3(0, 0, 0);
    capsule.to = Vector3(1, 0, 0);
    capsule.radius = 0.01f;
    EXPECT_EQ(resolveElements(data, mesh, capsule, {}, options, kind).size(), 11u);
}

TEST(VertexColorOps, ColorTargetMatchesWithToleranceAndCoverage) {
    const MeshView mesh = grid(3, 1.0f);
    TargetData data;
    ApplyOptions options = localOptions(Format::RGBA8);
    // the first triangle's corners red, then one corner of the second triangle
    Op faces;
    faces.type = OpType::Set;
    faces.value = kRed;
    faces.target = idsTarget(TargetKind::Faces, {0});
    Op corner;
    corner.type = OpType::Set;
    corner.value = Vector4(0.99f, 0.01f, 0, 1);
    corner.target = idsTarget(TargetKind::Corners, {3});
    ASSERT_TRUE(apply(data, mesh, {faces, corner}, options).ok);
    Target target;
    target.kind = TargetKind::Color;
    target.color.value = kRed;
    target.element = Element::Face;
    target.color.tolerance = 0.02f;
    target.color.coverage = 1.0f;
    ElementKind kind;
    EXPECT_EQ(resolveElements(data, mesh, target, {}, options, kind), std::vector<uint32_t>({0}));
    target.color.coverage = 0.3f;
    EXPECT_EQ(resolveElements(data, mesh, target, {}, options, kind), std::vector<uint32_t>({0, 1}));
    target.color.tolerance = 0.0f;
    EXPECT_EQ(resolveElements(data, mesh, target, {}, options, kind), std::vector<uint32_t>({0}));
    // hue only: a darker red matches
    target.color.value = Vector4(0.3f, 0, 0, 1);
    target.color.ignoreS = true;
    target.color.ignoreV = true;
    target.color.tolerance = 0.01f;
    target.color.coverage = 1.0f;
    EXPECT_EQ(resolveElements(data, mesh, target, {}, options, kind), std::vector<uint32_t>({0}));
    // a gray has no hue to match
    target.color.value = Vector4(0.3f, 0.3f, 0.3f, 1);
    EXPECT_EQ(resolveElements(data, mesh, target, {}, options, kind), std::vector<uint32_t>({2, 3, 4, 5, 6, 7}));
}

// ---- R45 gradient

TEST(VertexColorOps, LinearGradientRunsBetweenItsPoints) {
    const MeshView mesh = grid();
    TargetData data;
    Op op;
    op.type = OpType::Gradient;
    op.gradientFrom = Vector3(0, 0, 0);
    op.gradientTo = Vector3(1, 0, 0);
    op.stops = {GradientStop{0.0f, kBlack, 1.0f}, GradientStop{1.0f, kWhite, 1.0f}};
    op.interpolation = Interpolation::Linear;
    ASSERT_TRUE(apply(data, mesh, {op}, localOptions()).ok);
    for (int x = 0; x <= 10; x++) EXPECT_NEAR(logicalValue(data, mesh, gridLogical(mesh, x, 3)).x, x * 0.1f, 1e-3f) << x;
}

TEST(VertexColorOps, GradientExtendOffLeavesOutsideAlone) {
    const MeshView mesh = grid();
    TargetData data;
    Op op;
    op.type = OpType::Gradient;
    op.gradientFrom = Vector3(0.25f, 0, 0);
    op.gradientTo = Vector3(0.75f, 0, 0);
    op.stops = {GradientStop{0.0f, kBlack, 1.0f}, GradientStop{1.0f, kRed, 1.0f}};
    op.extend = Extend::Off;
    ASSERT_TRUE(apply(data, mesh, {op}, localOptions()).ok);
    EXPECT_EQ(logicalValue(data, mesh, gridLogical(mesh, 2, 0)), kWhite);
    EXPECT_EQ(logicalValue(data, mesh, gridLogical(mesh, 8, 0)), kWhite);
    op.extend = Extend::Forward;
    ASSERT_TRUE(apply(data, mesh, {op}, localOptions()).ok);
    EXPECT_EQ(logicalValue(data, mesh, gridLogical(mesh, 2, 0)), kWhite);
    EXPECT_EQ(logicalValue(data, mesh, gridLogical(mesh, 8, 0)), kRed);
}

TEST(VertexColorOps, GradientInterpolatesInOklabAndHsv) {
    const MeshView mesh = grid();
    auto midpoint = [&](Interpolation interpolation, HueDirection hue) {
        TargetData data;
        Op op;
        op.type = OpType::Gradient;
        op.gradientFrom = Vector3(0, 0, 0);
        op.gradientTo = Vector3(1, 0, 0);
        op.stops = {GradientStop{0.0f, kRed, 1.0f}, GradientStop{1.0f, kBlue, 1.0f}};
        op.interpolation = interpolation;
        op.hueDirection = hue;
        EXPECT_TRUE(apply(data, mesh, {op}, localOptions()).ok);
        return logicalValue(data, mesh, gridLogical(mesh, 5, 0));
    };
    const Vector4 linear = midpoint(Interpolation::Linear, HueDirection::Near);
    expectNear(linear, Vector4(0.5f, 0, 0.5f, 1));
    const Vector4 oklab = midpoint(Interpolation::Oklab, HueDirection::Near);
    EXPECT_GT(std::fabs(oklab.x - linear.x) + std::fabs(oklab.z - linear.z), 0.05f) << "OKLab is not a linear mix";
    // red (hue 0) to blue (hue 2/3): the near way passes magenta, the far way green
    const Vector4 nearHue = midpoint(Interpolation::Hsv, HueDirection::Near);
    EXPECT_GT(nearHue.x, 0.9f);
    EXPECT_GT(nearHue.z, 0.9f);
    EXPECT_LT(nearHue.y, 0.01f);
    const Vector4 farHue = midpoint(Interpolation::Hsv, HueDirection::Far);
    EXPECT_GT(farHue.y, 0.9f);
    EXPECT_LT(farHue.x + farHue.z, 0.01f);
}

TEST(VertexColorOps, HeightGradientSpansTheBoundsAndFaceElementWritesWholeFaces) {
    const MeshView mesh = cube();
    TargetData data;
    Op op;
    op.type = OpType::Gradient;
    op.gradientType = GradientType::Height;
    op.stops = {GradientStop{0.0f, kBlack, 1.0f}, GradientStop{1.0f, kWhite, 1.0f}};
    op.interpolation = Interpolation::Linear;
    ASSERT_TRUE(apply(data, mesh, {op}, localOptions(Format::RGBA8)).ok);
    for (uint32_t l = 0; l < 8; l++) EXPECT_EQ(logicalValue(data, mesh, l), mesh.logicalPositions[l].y > 0 ? kWhite : kBlack);
    EXPECT_FALSE(data.hasOverrides());

    TargetData faces;
    op.target.element = Element::Face;
    const OpsResult result = apply(faces, mesh, {op}, localOptions(Format::RGBA8));
    ASSERT_TRUE(result.ok);
    // a side face's centre is halfway up: its three corners hold one value, a hard edge to the top
    for (uint32_t t = 0; t < mesh.triangleCount(); t++) {
        const Vector4 a = faces.cornerValue(t * 3, mesh.indices);
        EXPECT_EQ(faces.cornerValue(t * 3 + 1, mesh.indices), a);
        EXPECT_EQ(faces.cornerValue(t * 3 + 2, mesh.indices), a);
    }
    EXPECT_GT(result.ops[0].splitAdded, 0u);
}

TEST(VertexColorOps, GradientStopStrengthScalesTheWrite) {
    const MeshView mesh = grid();
    TargetData data;
    Op op;
    op.type = OpType::Gradient;
    op.gradientFrom = Vector3(0, 0, 0);
    op.gradientTo = Vector3(1, 0, 0);
    op.stops = {GradientStop{0.0f, kBlack, 0.0f}, GradientStop{1.0f, kBlack, 1.0f}};
    ASSERT_TRUE(apply(data, mesh, {op}, localOptions()).ok);
    EXPECT_EQ(logicalValue(data, mesh, gridLogical(mesh, 0, 0)), kWhite);
    EXPECT_NEAR(logicalValue(data, mesh, gridLogical(mesh, 5, 0)).x, 0.5f, 1e-3f);
    EXPECT_EQ(logicalValue(data, mesh, gridLogical(mesh, 10, 0)), kBlack);
}

// ---- smooth, sharpen

TEST(VertexColorOps, SmoothAveragesNeighboursOrderIndependently) {
    const MeshView mesh = grid();
    const uint32_t centre = gridLogical(mesh, 5, 5);
    TargetData data;
    ASSERT_TRUE(apply(data, mesh, {fillOp(kBlack, idsTarget(TargetKind::Vertices, {centre}))}, localOptions()).ok);
    Op smooth;
    smooth.type = OpType::Smooth;
    ASSERT_TRUE(apply(data, mesh, {smooth}, localOptions()).ok);
    // the centre takes its neighbours' mean (all white); each neighbour the mean of its own
    // neighbours as they were (one of them black), not as the centre became
    EXPECT_EQ(logicalValue(data, mesh, centre), kWhite);
    const uint32_t right = gridLogical(mesh, 6, 5);
    const uint32_t count = mesh.neighborStart[right + 1] - mesh.neighborStart[right];
    EXPECT_EQ(logicalValue(data, mesh, right), half(Vector4((float)(count - 1) / count, (float)(count - 1) / count, (float)(count - 1) / count, 1)));
}

TEST(VertexColorOps, SmoothCrossesLogicalVertexSeams) {
    const MeshView mesh = cube();
    TargetData data;
    ASSERT_TRUE(apply(data, mesh, {fillOp(kBlack, idsTarget(TargetKind::Vertices, {0}))}, localOptions()).ok);
    Op smooth;
    smooth.type = OpType::Smooth;
    smooth.target = idsTarget(TargetKind::Vertices, {1, 2, 3, 4, 5, 6, 7});
    ASSERT_TRUE(apply(data, mesh, {smooth}, localOptions()).ok);
    for (uint32_t k = mesh.neighborStart[0]; k < mesh.neighborStart[1]; k++) EXPECT_LT(logicalValue(data, mesh, mesh.neighbors[k]).x, 1.0f);
    EXPECT_EQ(logicalValue(data, mesh, 0), kBlack) << "outside the target";
}

TEST(VertexColorOps, SharpenPushesAwayFromTheNeighbourMean) {
    const MeshView mesh = grid();
    const uint32_t centre = gridLogical(mesh, 5, 5);
    TargetData data;
    ASSERT_TRUE(apply(data, mesh, {fillOp(Vector4(0.5f, 0.5f, 0.5f, 1), idsTarget(TargetKind::Vertices, {centre}))}, localOptions()).ok);
    Op sharpen;
    sharpen.type = OpType::Sharpen;
    sharpen.strength = 0.5f;
    sharpen.target = idsTarget(TargetKind::Vertices, {centre});
    ASSERT_TRUE(apply(data, mesh, {sharpen}, localOptions()).ok);
    // 0.5 + (2 * 0.5 - 1 - 0.5) * 0.5
    EXPECT_EQ(logicalValue(data, mesh, centre).x, half(Vector4(0.25f, 0, 0, 0)).x);
}

// ---- R46-R49, R44, clear

TEST(VertexColorOps, NoiseIsSeededAndStaysBetweenItsValues) {
    const MeshView mesh = sphere(12, 16);
    auto run = [&](uint32_t seed, NoiseKind kind) {
        TargetData data;
        Op op;
        op.type = OpType::Noise;
        op.noiseKind = kind;
        op.seed = seed;
        op.noiseScale = 0.5f;
        op.value = Vector4(0.2f, 0.2f, 0.2f, 1);
        op.value2 = Vector4(0.8f, 0.8f, 0.8f, 1);
        EXPECT_TRUE(apply(data, mesh, {op}, localOptions(Format::RGBA8)).ok);
        for (const Vector4& v : data.base) {
            EXPECT_GE(v.x, 0.2f - 1.0f / 255.0f);
            EXPECT_LE(v.x, 0.8f + 1.0f / 255.0f);
        }
        return hashTarget(data);
    };
    EXPECT_EQ(run(7, NoiseKind::Simplex), run(7, NoiseKind::Simplex));
    EXPECT_NE(run(7, NoiseKind::Simplex), run(8, NoiseKind::Simplex));
    EXPECT_NE(run(7, NoiseKind::Value), run(7, NoiseKind::Cellular));
}

TEST(VertexColorOps, AdjustAppliesLevelsContrastHsvInvertAndClamp) {
    const MeshView mesh = quad();
    auto adjusted = [&](const Vector4& start, auto setup) {
        TargetData data;
        Op op;
        op.type = OpType::Adjust;
        setup(op);
        EXPECT_TRUE(apply(data, mesh, {fillOp(start), op}, localOptions()).ok);
        return data.base[0];
    };
    EXPECT_EQ(adjusted(Vector4(0.25f, 0.5f, 1.0f, 1), [](Op& op) {
                  op.adjustKind = AdjustKind::Levels;
                  op.inBlack = 0.25f;
                  op.inWhite = 0.75f;
                  op.channels = CHANNELS_RGB;
              }),
              half(Vector4(0.0f, 0.5f, 1.0f, 1)));
    // gamma 2 lifts the midtones: 0.25^(1/2) = 0.5
    EXPECT_EQ(adjusted(Vector4(0.25f, 0.25f, 0.25f, 1), [](Op& op) {
                  op.adjustKind = AdjustKind::Levels;
                  op.gamma = 2.0f;
                  op.channels = CHANNEL_R;
              }).x,
              0.5f);
    EXPECT_EQ(adjusted(Vector4(0.25f, 0.5f, 0.75f, 1), [](Op& op) {
                  op.adjustKind = AdjustKind::BrightnessContrast;
                  op.brightness = 0.1f;
                  op.contrast = 1.0f;
                  op.channels = CHANNELS_RGB;
              }),
              half(Vector4(0.1f, 0.6f, 1.1f, 1)));
    // red shifted a third of the wheel is green
    expectNear(adjusted(kRed, [](Op& op) {
                   op.adjustKind = AdjustKind::Hsv;
                   op.hueShift = 120.0f;
                   op.channels = CHANNELS_RGB;
               }),
               kGreen);
    EXPECT_EQ(adjusted(Vector4(0.25f, 0.5f, 1.0f, 1), [](Op& op) {
                  op.adjustKind = AdjustKind::Invert;
                  op.channels = CHANNELS_RGB;
              }),
              half(Vector4(0.75f, 0.5f, 0.0f, 1)));
    EXPECT_EQ(adjusted(Vector4(-0.5f, 0.5f, 3.0f, 1), [](Op& op) {
                  op.adjustKind = AdjustKind::Clamp;
                  op.outBlack = 0.0f;
                  op.outWhite = 1.0f;
              }),
              half(Vector4(0.0f, 0.5f, 1.0f, 1)));
}

TEST(VertexColorOps, ChannelOpsCopySwapInvertAndPack) {
    const MeshView mesh = quad();
    const Vector4 start(0.25f, 0.5f, 0.75f, 1.0f);
    auto run = [&](auto setup) {
        TargetData data;
        Op op;
        op.type = OpType::Channel;
        setup(op);
        EXPECT_TRUE(apply(data, mesh, {fillOp(start), op}, localOptions()).ok);
        return data.base[0];
    };
    EXPECT_EQ(run([](Op& op) {
                  op.channelKind = ChannelOpKind::Copy;
                  op.sourceChannel = 0;
                  op.destChannel = 3;
              }),
              half(Vector4(0.25f, 0.5f, 0.75f, 0.25f)));
    EXPECT_EQ(run([](Op& op) {
                  op.channelKind = ChannelOpKind::Swap;
                  op.sourceChannel = 0;
                  op.destChannel = 2;
              }),
              half(Vector4(0.75f, 0.5f, 0.25f, 1.0f)));
    EXPECT_EQ(run([](Op& op) {
                  op.channelKind = ChannelOpKind::Invert;
                  op.channels = CHANNEL_G | CHANNEL_A;
              }),
              half(Vector4(0.25f, 0.5f, 0.75f, 0.0f)));
    EXPECT_EQ(run([](Op& op) {
                  op.channelKind = ChannelOpKind::Pack;
                  op.packSource = {1, 1, 2, -1};
                  op.packConstant = {0, 0, 0, 0.5f};
              }),
              half(Vector4(0.5f, 0.5f, 0.75f, 0.5f)));
}

TEST(VertexColorOps, ReplaceColorRecolorsEveryMatchOnly) {
    const MeshView mesh = cube();
    TargetData data;
    Op red;
    red.type = OpType::Set;
    red.value = kRed;
    red.target = idsTarget(TargetKind::Faces, {0, 1});
    Op blue = red;
    blue.value = kBlue;
    blue.target = idsTarget(TargetKind::Faces, {4, 5});
    Op replace;
    replace.type = OpType::ReplaceColor;
    replace.target.color.value = kRed;
    replace.value = kGreen;
    ASSERT_TRUE(apply(data, mesh, {red, blue, replace}, localOptions(Format::RGBA8)).ok);
    for (uint32_t c = 0; c < 36; c++) {
        const Vector4 v = data.cornerValue(c, mesh.indices);
        EXPECT_NE(v, kRed);
        if (c < 6) EXPECT_EQ(v, kGreen);
        else if (c >= 12 && c < 18) EXPECT_EQ(v, kBlue);
        else EXPECT_EQ(v, kWhite);
    }
}

TEST(VertexColorOps, ByDirectionWritesBySmoothstepOfTheNormal) {
    const MeshView mesh = cube();
    TargetData data;
    Op op;
    op.type = OpType::ByDirection;
    op.value = kGreen;
    op.direction = Vector3(0, 1, 0);
    op.rangeMin = 0.5f;
    op.rangeMax = 1.0f;
    op.target.element = Element::Face;
    ASSERT_TRUE(apply(data, mesh, {op}, localOptions(Format::RGBA8)).ok);
    for (uint32_t t = 0; t < 12; t++) {
        const bool top = mesh.faceNormals[t].y > 0.5f;
        EXPECT_EQ(data.cornerValue(t * 3, mesh.indices), top ? kGreen : kWhite) << t;
    }
}

TEST(VertexColorOps, ClearGoesBackToTheChosenValue) {
    MeshView mesh = quad();
    mesh.assetColors = {kRed, kRed, kBlue, kBlue};
    ASSERT_TRUE(mesh.build());
    ApplyOptions options = localOptions(Format::RGBA8);
    options.initial = InitialValue::Custom;
    options.customInitial = kGreen;
    TargetData data;
    Op clear;
    clear.type = OpType::Clear;
    ASSERT_TRUE(apply(data, mesh, {fillOp(kBlack), clear}, options).ok);
    for (const Vector4& v : data.base) EXPECT_EQ(v, kGreen);
    clear.clearTo = ClearTo::Asset;
    ASSERT_TRUE(apply(data, mesh, {clear}, options).ok);
    EXPECT_EQ(data.base, mesh.assetColors);
    clear.clearTo = ClearTo::Black;
    ASSERT_TRUE(apply(data, mesh, {clear}, options).ok);
    EXPECT_EQ(data.base[0], kBlack);
}

// ---- R41, R60: a generated field, written like any op

TEST(VertexColorOps, FieldWithSeamsKeepsHardEdgesOnlyWhereCornersDisagree) {
    const MeshView mesh = quad();
    bake::Field field;
    field.perCorner = true;
    field.vertexValues.assign(4, kWhite);
    // triangle 0 red, triangle 1 blue: the diagonal vertices (1 and 3) see both
    field.cornerValues = {kRed, kRed, kRed, kBlue, kBlue, kBlue};
    Op op;
    op.type = OpType::FromTexture;
    op.image = std::make_shared<Image>();
    TargetData data;
    const OpsResult result = detail::applyOpsWithField(data, mesh, std::span<const Op>(&op, 1), localOptions(Format::RGBA8), field);
    ASSERT_TRUE(result.ok) << result.error;
    for (uint32_t c = 0; c < 6; c++) EXPECT_EQ(data.cornerValue(c, mesh.indices), c < 3 ? kRed : kBlue) << c;
    EXPECT_EQ(data.base[0], kRed);
    EXPECT_EQ(data.base[2], kBlue);
    // vertices 1 and 3: base from their first corner, an override on the other
    EXPECT_EQ(data.corners.size(), 2u);

    // without keep_seams every logical vertex takes its corners' mean
    op.keepSeams = false;
    TargetData smooth;
    ASSERT_TRUE(detail::applyOpsWithField(smooth, mesh, std::span<const Op>(&op, 1), localOptions(), field).ok);
    EXPECT_FALSE(smooth.hasOverrides());
    expectNear(smooth.base[1], Vector4(0.5f, 0, 0.5f, 1));

    // corners within seam_delta_e of each other share their mean
    op.keepSeams = true;
    op.seamDeltaE = 50.0f;
    TargetData close;
    field.cornerValues = {kRed, kRed, kRed, Vector4(0.98f, 0, 0, 1), Vector4(0.98f, 0, 0, 1), Vector4(0.98f, 0, 0, 1)};
    ASSERT_TRUE(detail::applyOpsWithField(close, mesh, std::span<const Op>(&op, 1), localOptions(), field).ok);
    EXPECT_FALSE(close.hasOverrides());
}

TEST(VertexColorOps, FieldLeavesUnmatchedVerticesAlone) {
    const MeshView mesh = cube();
    bake::Field field;
    field.vertexValues.assign(24, kBlue);
    field.valid.assign(24, 1);
    // logical vertex 0: no member matched; logical 1: one of three did
    for (uint32_t v = 0; v < 24; v++) {
        if (mesh.logical[v] <= 1) field.valid[v] = 0;
    }
    field.valid[mesh.members[mesh.memberStart[1]]] = 1;
    Op op;
    op.type = OpType::Transfer;
    op.source = std::make_shared<TransferSource>();
    TargetData data;
    ASSERT_TRUE(detail::applyOpsWithField(data, mesh, std::span<const Op>(&op, 1), localOptions(Format::RGBA8), field).ok);
    EXPECT_EQ(logicalValue(data, mesh, 0), kWhite);
    EXPECT_EQ(logicalValue(data, mesh, 1), kBlue);
    EXPECT_EQ(logicalValue(data, mesh, 7), kBlue);
}

// ---- R28, R54, R121: stats, palette, hash

TEST(VertexColorOps, StatsCountEveryCorner) {
    const MeshView mesh = quad();
    TargetData data;
    data.base = {kBlack, kWhite, kWhite, Vector4(0.5f, 0.5f, 0.5f, 0.5f)};
    const ChannelStats stats = computeStats(data, mesh);
    // corners: 0 3 1 1 3 2 -> black, gray, white, white, gray, white
    EXPECT_EQ(stats.count, 6u);
    EXPECT_EQ(stats.min[0], 0.0f);
    EXPECT_EQ(stats.max[0], 1.0f);
    EXPECT_FLOAT_EQ(stats.mean[0], 4.0f / 6.0f);
    EXPECT_EQ(stats.p50[0], 1.0f);
    EXPECT_EQ(stats.p05[0], 0.0f);
    EXPECT_EQ(stats.histogram[0][0], 1u);
    EXPECT_EQ(stats.histogram[0][8], 2u);
    EXPECT_EQ(stats.histogram[0][15], 3u);
    EXPECT_EQ(stats.histogram[3][8], 2u);
}

TEST(VertexColorOps, ResultCarriesStatsBeforeAndAfter) {
    const MeshView mesh = quad();
    TargetData data;
    OpsResult result = apply(data, mesh, {fillOp(kBlack)}, localOptions());
    EXPECT_EQ(result.before.mean[0], 1.0f);
    EXPECT_EQ(result.after.mean[0], 0.0f);
    ApplyOptions quiet = localOptions();
    quiet.stats = false;
    result = apply(data, mesh, {fillOp(kWhite)}, quiet);
    EXPECT_EQ(result.after.count, 0u);
}

TEST(VertexColorOps, PaletteSortsByHueAndKeepsTheMostUsed) {
    const MeshView mesh = grid(4, 1.0f);
    TargetData data;
    ApplyOptions options = localOptions(Format::RGBA8);
    Op blue;
    blue.type = OpType::Set;
    blue.value = kBlue;
    blue.target = idsTarget(TargetKind::Faces, {0, 1, 2});
    Op red = blue;
    red.value = kRed;
    red.target = idsTarget(TargetKind::Faces, {3});
    Op gray = blue;
    gray.value = Vector4(0.5f, 0.5f, 0.5f, 1);
    gray.target = idsTarget(TargetKind::Faces, {4, 5});
    ASSERT_TRUE(apply(data, mesh, {blue, red, gray}, options).ok);
    std::vector<PaletteEntry> entries = palette(data, mesh);
    ASSERT_EQ(entries.size(), 4u);
    // grays first (gray, then white), then by hue: red, blue
    EXPECT_EQ(entries[0].value, quantize(Vector4(0.5f, 0.5f, 0.5f, 1), Format::RGBA8));
    EXPECT_EQ(entries[0].corners, 6u);
    EXPECT_EQ(entries[1].value, kWhite);
    EXPECT_EQ(entries[1].corners, 36u);
    EXPECT_EQ(entries[2].value, kRed);
    EXPECT_EQ(entries[2].corners, 3u);
    EXPECT_EQ(entries[3].value, kBlue);
    EXPECT_EQ(entries[3].corners, 9u);
    entries = palette(data, mesh, 2);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].value, kWhite);
    EXPECT_EQ(entries[1].value, kBlue);
}

TEST(VertexColorOps, ColorMathsConvertAtTheBoundary) {
    Vector4 v;
    ASSERT_TRUE(parseHex("#FF000080", v));
    EXPECT_EQ(v, Vector4(1, 0, 0, 128.0f / 255.0f));
    ASSERT_TRUE(parseHex("808080", v));
    EXPECT_NEAR(v.x, 0.2195197f, 1e-6f);   // (128 / 255) ^ 2.2, the engine's display encoding
    EXPECT_EQ(toHex(v, false), "#808080");
    EXPECT_EQ(toHex(Vector4(1, 0.5f, 0, 0.5f), true), "#FFBA0080");
    EXPECT_FALSE(parseHex("#12345", v));
    EXPECT_FALSE(parseHex("#GG0000", v));
    EXPECT_EQ(deltaE(kRed, kRed), 0.0f);
    EXPECT_NEAR(deltaE(kBlack, kWhite), 100.0f, 0.01f);
}

// AC1: every op on fixed meshes gives the same bytes everywhere. A change here is a change of the
// stored values: look at why before rewriting a hash.
TEST(VertexColorOps, EveryOpMatchesItsGoldenHash) {
    const MeshView meshes[] = {sphere(), cube()};
    std::vector<std::pair<std::string, Op>> cases;
    {
        Op op;
        op.type = OpType::Set;
        op.value = Vector4(0.2f, 0.4f, 0.6f, 1);
        op.target = idsTarget(TargetKind::Faces, {1, 3, 5});
        cases.emplace_back("set", op);
    }
    {
        Op op = fillOp(Vector4(0.9f, 0.1f, 0.3f, 1));
        op.blend = Blend::Multiply;
        op.strength = 0.7f;
        op.target.kind = TargetKind::Sphere;
        op.target.center = Vector3(0.3f, 0.5f, 0.2f);
        op.target.radius = 0.8f;
        cases.emplace_back("fill", op);
    }
    {
        Op op = strokeOp({Vector3(-0.6f, 0.4f, 0.5f), Vector3(0.1f, 0.7f, 0.4f), Vector3(0.6f, 0.2f, -0.3f)}, 0.45f, Vector4(0.1f, 0.8f, 0.2f, 1));
        op.strength = 0.6f;
        op.hardness = 0.2f;
        cases.emplace_back("stroke", op);
    }
    {
        Op op;
        op.type = OpType::Gradient;
        op.gradientFrom = Vector3(-1, -1, 0);
        op.gradientTo = Vector3(1, 1, 0);
        op.stops = {GradientStop{0.0f, Vector4(0.9f, 0.2f, 0.1f, 1), 1.0f}, GradientStop{0.6f, Vector4(0.1f, 0.3f, 0.9f, 1), 0.8f},
                    GradientStop{1.0f, Vector4(0.9f, 0.9f, 0.2f, 1), 1.0f}};
        cases.emplace_back("gradient", op);
    }
    {
        Op op;
        op.type = OpType::Noise;
        op.seed = 3;
        op.noiseScale = 0.7f;
        op.value = Vector4(0, 0, 0, 1);
        cases.emplace_back("noise", op);
    }
    {
        Op op;
        op.type = OpType::Adjust;
        op.adjustKind = AdjustKind::Hsv;
        op.hueShift = 45.0f;
        op.saturationShift = 0.2f;
        cases.emplace_back("adjust", op);
    }
    std::vector<std::string> hashes;
    for (const MeshView& mesh : meshes) {
        for (const auto& [name, op] : cases) {
            TargetData data;
            // a colored start so value-reading ops have something to read
            Op base;
            base.type = OpType::Gradient;
            base.gradientType = GradientType::Axis;
            base.axis = Vector3(1, 0.5f, 0.25f);
            base.stops = {GradientStop{0.0f, Vector4(0.8f, 0.3f, 0.2f, 1), 1.0f}, GradientStop{1.0f, Vector4(0.2f, 0.6f, 0.9f, 1), 1.0f}};
            Op smooth;
            smooth.type = OpType::Smooth;
            smooth.iterations = 2;
            smooth.strength = 0.5f;
            const OpsResult result = apply(data, mesh, {base, op, smooth}, localOptions(Format::RGBA8));
            EXPECT_TRUE(result.ok) << name << ": " << result.error;
            hashes.push_back(name + " " + hex(result.hash));
        }
    }
    // sphere, then cube
    const std::vector<std::string> expected = {
        "set c992d9377c7d96df", "fill 4f5c552aa90dff15", "stroke 640f47d67818a652", "gradient 64a014bd7ed36427",
        "noise dd8597a5d73f35d6", "adjust d316f4e49d0d48d2", "set d048aa22d3fde150", "fill 8a67a5c9d7b33308",
        "stroke b0f2725fb5743a0a", "gradient 1dc788cca25778b3", "noise 68ad385d3b914bd6", "adjust 15d83501990dea54",
    };
    EXPECT_EQ(hashes, expected);
}

} // namespace vertex_color_ops_test
