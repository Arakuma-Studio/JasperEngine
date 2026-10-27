// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The pure parts of the agent front end (vertex animation spec 6): the contact sheet's sample
// times, the CPU decode of a payload with the shader's frame math (vatMeshAt, both modes and
// both precisions, rest "none" clips), the cell rasterizer and the sheet composition, and the hand
// edits a rebake keeps (keepVatEdits). The source row (VatPoser) and the routing need a project:
// tools/mcp/tests/test_bake_vertex_animation.py covers them.

#include <gtest/gtest.h>

#include "vat/VatBakeCompute.h"
#include "vat/VatContactSheet.h"
#include "vat/VatFrontEnd.h"

#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::vat;
using jasperengine::vat::VatClip;
using jasperengine::vat::VatData;
using jasperengine::vat::VatMode;
using jasperengine::vat::VatPayload;
using jasperengine::vat::VatPrecision;

namespace {

    // a 3-vertex triangle mesh, rest at the given positions
    VatMeshData triangle(VatMode mode) {
        VatMeshData mesh;
        mesh.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        mesh.vatIndex = {0, 1, 2};
        VatSubmesh sub;
        sub.indices = {0, 1, 2};
        mesh.submeshes.push_back(sub);
        if (mode == VatMode::Bones) {
            mesh.joints = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
            mesh.weights = {1, 0, 0, 0, 1, 0, 0, 0, 0.5f, 0.5f, 0, 0};
        }
        return mesh;
    }

    void putTexel(VatPayload& payload, uint64_t texel, const float v[4]) {
        if (payload.precision == VatPrecision::Float) {
            std::memcpy(payload.bytes.data() + texel * 16, v, 16);
        } else {
            for (int k = 0; k < 4; k++) {
                const uint16_t h = floatToHalf(v[k]);
                std::memcpy(payload.bytes.data() + texel * 8 + k * 2, &h, 2);
            }
        }
    }

    // two frames of a 3-vertex Vertices asset: frame 0 at rest, frame 1 moved by (0, 2, 0)
    VatData verticesAsset(VatPrecision precision, VatPayload& payload, ClipWrapMode wrap) {
        VatData data;
        data.mode = VatMode::Vertices;
        data.layout.precision = precision;
        data.layout.width = 8;
        data.layout.vertices = 3;
        data.layout.normals = false;
        data.layout.samples = 2;
        data.layout.height = 1;
        VatClip clip;
        clip.name = "Lift";
        clip.start = 0;
        clip.frames = 2;
        clip.fps = 2.0f;
        clip.wrap = wrap;
        data.clips.push_back(clip);
        payload.precision = precision;
        payload.width = 8;
        payload.height = 1;
        payload.bytes.assign(8 * (precision == VatPrecision::Float ? 16 : 8), 0);
        const float up[4] = {0, 2, 0, 0};
        for (int v = 0; v < 3; v++) putTexel(payload, jasperengine::vat::vatTexelIndex(data.mode, data.layout, 1, v, 0), up);
        return data;
    }

}

TEST(VatContactSheet, TimesCoverALoopAndIncludeTheEndOtherwise) {
    const std::vector<float> loop = vatSheetTimes(1.0f, ClipWrapMode::Loop);
    ASSERT_EQ(loop.size(), 4u);
    EXPECT_FLOAT_EQ(loop[1], 0.25f);
    EXPECT_FLOAT_EQ(loop[3], 0.75f);
    const std::vector<float> once = vatSheetTimes(1.5f, ClipWrapMode::Once);
    EXPECT_FLOAT_EQ(once[0], 0.0f);
    EXPECT_FLOAT_EQ(once[3], 1.5f);
}

TEST(VatContactSheet, VerticesDecodeMixesFramesLikeTheShader) {
    for (VatPrecision precision : {VatPrecision::Half, VatPrecision::Float}) {
        VatPayload payload;
        // clamp: frame 1 is the end (0.5 s), halfway at 0.25 s
        VatData data = verticesAsset(precision, payload, ClipWrapMode::ClampForever);
        VatMeshData mesh = triangle(VatMode::Vertices);
        VatSheetMesh out;
        std::string error;
        ASSERT_TRUE(vatMeshAt(data, payload, mesh, 0, 0.25f, out, &error)) << error;
        ASSERT_EQ(out.positions.size(), 3u);
        EXPECT_NEAR(out.positions[1].x, 1.0f, 1e-6f);
        EXPECT_NEAR(out.positions[1].y, 1.0f, 1e-3f);
        ASSERT_TRUE(vatMeshAt(data, payload, mesh, 0, 10.0f, out, &error)) << error;
        EXPECT_NEAR(out.positions[2].y, 3.0f, 1e-3f);
        EXPECT_EQ(out.indices.size(), 3u);
        EXPECT_EQ(out.colors.size(), 1u);
        // not interpolated: the lower frame
        data.clips[0].interpolate = false;
        ASSERT_TRUE(vatMeshAt(data, payload, mesh, 0, 0.25f, out, &error)) << error;
        EXPECT_NEAR(out.positions[0].y, 0.0f, 1e-6f);
        // rest "none": the offsets are the positions
        data.clips[0].restNone = true;
        ASSERT_TRUE(vatMeshAt(data, payload, mesh, 0, 10.0f, out, &error)) << error;
        EXPECT_NEAR(out.positions[1].x, 0.0f, 1e-6f);
        EXPECT_NEAR(out.positions[1].y, 2.0f, 1e-3f);
    }
}

TEST(VatContactSheet, LoopWrapsTheLastFrameToTheFirst) {
    VatPayload payload;
    VatData data = verticesAsset(VatPrecision::Float, payload, ClipWrapMode::Loop);
    data.clips[0].fps = 2.0f;   // 2 frames at 2 fps: a 1 s loop, frame 1 at 0.5 s
    VatSheetMesh out;
    std::string error;
    // 0.75 s: halfway from frame 1 back to frame 0
    ASSERT_TRUE(vatMeshAt(data, payload, triangle(VatMode::Vertices), 0, 0.75f, out, &error)) << error;
    EXPECT_NEAR(out.positions[0].y, 1.0f, 1e-5f);
}

TEST(VatContactSheet, BonesDecodeBlendsWeightedBones) {
    VatData data;
    data.mode = VatMode::Bones;
    data.layout.precision = VatPrecision::Float;
    data.layout.width = 8;
    data.layout.bones = 2;
    data.layout.samples = 1;
    data.layout.height = 1;
    VatClip clip;
    clip.name = "Pose";
    clip.frames = 1;
    clip.fps = 30.0f;
    data.clips.push_back(clip);
    VatPayload payload;
    payload.precision = VatPrecision::Float;
    payload.width = 8;
    payload.height = 1;
    payload.bytes.assign(8 * 16, 0);
    // bone 0: identity; bone 1: translate (0, 0, 4) and turn 90 degrees about Z, scale 1
    const float identity[4] = {0, 0, 0, 1};
    const float still[4] = {0, 0, 0, 1};
    const float s = std::sqrt(0.5f);
    const float turn[4] = {0, 0, s, s};
    const float move[4] = {0, 0, 4, 1};
    putTexel(payload, jasperengine::vat::vatTexelIndex(VatMode::Bones, data.layout, 0, 0, 0), identity);
    putTexel(payload, jasperengine::vat::vatTexelIndex(VatMode::Bones, data.layout, 0, 0, 1), still);
    putTexel(payload, jasperengine::vat::vatTexelIndex(VatMode::Bones, data.layout, 0, 1, 0), turn);
    putTexel(payload, jasperengine::vat::vatTexelIndex(VatMode::Bones, data.layout, 0, 1, 1), move);
    VatSheetMesh out;
    std::string error;
    ASSERT_TRUE(vatMeshAt(data, payload, triangle(VatMode::Bones), 0, 0.0f, out, &error)) << error;
    EXPECT_NEAR(out.positions[1].x, 1.0f, 1e-5f);              // bone 0 only: at rest
    // half bone 0 (rest (0,1,0)), half bone 1 ((0,1,0) turned to (-1,0,0), moved to (-1,0,4))
    EXPECT_NEAR(out.positions[2].x, -0.5f, 1e-5f);
    EXPECT_NEAR(out.positions[2].y, 0.5f, 1e-5f);
    EXPECT_NEAR(out.positions[2].z, 2.0f, 1e-5f);
    // a joint past the payload's bones is an error, not a guess
    VatMeshData bad = triangle(VatMode::Bones);
    bad.joints[0] = 7;
    EXPECT_FALSE(vatMeshAt(data, payload, bad, 0, 0.0f, out, &error));
    EXPECT_FALSE(vatMeshAt(data, payload, triangle(VatMode::Bones), 3, 0.0f, out, &error));
}

TEST(VatContactSheet, CellsDrawTheMeshAndTheSheetComposes) {
    VatSheetMesh mesh;
    mesh.positions = {Vector3(-1, 0, 0), Vector3(1, 0, 0), Vector3(0, 2, 0)};
    mesh.indices = {0, 1, 2};
    mesh.colors = {PoseColor(255, 0, 0)};
    const PoseImage cell = renderVatCell(mesh, Vector3(-1, 0, -0.1f), Vector3(1, 2, 0.1f), 64, 64);
    ASSERT_EQ(cell.getWidth(), 64);
    int red = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            if (cell.getPixel(x, y).r > 100 && cell.getPixel(x, y).g < 60) red++;
    EXPECT_GT(red, 400);
    // an empty mesh is the background only
    const PoseImage empty = renderVatCell(VatSheetMesh(), Vector3(-1, -1, -1), Vector3(1, 1, 1), 16, 16);
    EXPECT_EQ(empty.getPixel(8, 8), empty.getPixel(0, 0));

    VatSheetRow row;
    row.label = "Walk VAT";
    for (int i = 0; i < VAT_SHEET_TIMES; i++) row.meshes.push_back(mesh);
    const PoseImage sheet = composeVatSheet({row, row}, {vatSheetTimes(1.0f, ClipWrapMode::Loop)}, "title", 32);
    EXPECT_EQ(sheet.getWidth(), 4 * 32 + 3);
    EXPECT_EQ(sheet.getHeight(), (14 + 1) + 2 * (32 + 14) + 1);
}

TEST(VatFrontEnd, RebakesKeepHandEditsByClip) {
    VatData old;
    VatClip walk;
    walk.name = "Stroll";
    walk.frames = 30;
    walk.fps = 30.0f;
    walk.speed = 1.5f;
    walk.wrap = ClipWrapMode::PingPong;
    walk.interpolate = false;
    ClipEventData step;
    step.time = 0.25f;
    step.function = "onStep";
    walk.events.push_back(step);
    old.clips.push_back(walk);
    old.sockets.push_back({"Sword", "Armature/Hand"});
    old.unknownKeys.push_back({"notes", "\"keep\""});

    VatData fresh;
    VatClip again = walk;
    again.name = "Walk";
    again.speed = 1.0f;
    again.wrap = ClipWrapMode::Loop;
    again.interpolate = true;
    again.events.clear();
    fresh.clips.push_back(again);
    VatClip idle;
    idle.name = "Idle";
    idle.frames = 60;
    idle.fps = 30.0f;
    fresh.clips.push_back(idle);
    fresh.sockets.push_back({"RightHand", "Armature/Hand"});

    keepVatEdits(fresh, old);
    // matched in order by length: the rename, speed, events, interpolate and the ping-pong wrap
    // (a non-loop wrap is kept only when the new clip loops the same way: here loop vs ping-pong
    // differ in looping, so the bake's loop stays)
    EXPECT_EQ(fresh.clips[0].name, "Stroll");
    EXPECT_FLOAT_EQ(fresh.clips[0].speed, 1.5f);
    EXPECT_FALSE(fresh.clips[0].interpolate);
    ASSERT_EQ(fresh.clips[0].events.size(), 1u);
    EXPECT_EQ(fresh.clips[0].events[0].function, "onStep");
    EXPECT_EQ(fresh.clips[0].wrap, ClipWrapMode::Loop);
    EXPECT_EQ(fresh.clips[1].name, "Idle");
    EXPECT_EQ(fresh.sockets[0].name, "Sword");
    ASSERT_EQ(fresh.unknownKeys.size(), 1u);
}
