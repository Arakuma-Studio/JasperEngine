// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The payload addressing and the playback math (spec 2.2, 3.4) that vat.glsl mirrors: texel
// indices at row wraps in both modes, the frame pair of every wrap (loop seam, once/clamp with
// finished, ping-pong legs and turning points, negative time, one-frame clips, no
// interpolation, fractional fps), the normalized (phase-synced) form, B's weight, the
// per-instance packing, clip lengths and the layout hash.

#include <gtest/gtest.h>

#include "VatTestEnvironment.h"

#include "animation/VertexAnimation.h"

#include <cmath>
#include <functional>
#include <limits>
#include <set>

using namespace jasperengine;
using namespace jasperengine::vat;

namespace {

    VatClip makeClip(int start, int frames, float fps, ClipWrapMode wrap, bool interpolate = true){
        VatClip clip;
        clip.name = "c";
        clip.start = start;
        clip.frames = frames;
        clip.fps = fps;
        clip.wrap = wrap;
        clip.interpolate = interpolate;
        return clip;
    }

    // frame0 / frame1 / alpha / normalized / finished of one sample
    void expectSample(const VatFrameSample& s, int f0, int f1, float alpha, float normalized, bool finished){
        EXPECT_EQ(s.frame0, f0);
        EXPECT_EQ(s.frame1, f1);
        EXPECT_NEAR(s.alpha, alpha, 1e-5f);
        EXPECT_NEAR(s.normalized, normalized, 1e-5f);
        EXPECT_EQ(s.finished, finished);
    }

}

TEST(VatPayloadLayout, TexelsPerSample){
    VatLayout layout;
    layout.normals = false;
    layout.colors = false;
    EXPECT_EQ(layout.texelsPerSample(VatMode::Vertices), 1);
    layout.normals = true;
    EXPECT_EQ(layout.texelsPerSample(VatMode::Vertices), 2);
    layout.colors = true;
    EXPECT_EQ(layout.texelsPerSample(VatMode::Vertices), 3);
    EXPECT_EQ(layout.texelsPerSample(VatMode::Bones), 2);
    layout.normals = false;
    EXPECT_EQ(layout.texelsPerSample(VatMode::Bones), 2);
}

TEST(VatPayloadLayout, TexelIndexInBothModesAndRowWraps){
    VatLayout layout;
    layout.width = 8;
    layout.vertices = 3;
    layout.bones = 5;
    layout.normals = true;
    layout.colors = false;
    // vertices: t = (frame * V + vertex) * S + k
    EXPECT_EQ(vatTexelIndex(VatMode::Vertices, layout, 0, 0, 0), 0u);
    EXPECT_EQ(vatTexelIndex(VatMode::Vertices, layout, 0, 2, 1), 5u);
    EXPECT_EQ(vatTexelIndex(VatMode::Vertices, layout, 1, 0, 0), 6u);
    EXPECT_EQ(vatTexelIndex(VatMode::Vertices, layout, 1, 1, 0), 8u);
    // texel 8 is the first of row 1 (width 8): (t % W, t / W) = (0, 1); frame 1 vertex 0's
    // tangent frame (7) is the last texel of row 0
    uint64_t t = vatTexelIndex(VatMode::Vertices, layout, 1, 1, 0);
    EXPECT_EQ(t % (uint64_t)layout.width, 0u);
    EXPECT_EQ(t / (uint64_t)layout.width, 1u);
    t = vatTexelIndex(VatMode::Vertices, layout, 1, 0, 1);
    EXPECT_EQ(t % 8, 7u);
    EXPECT_EQ(t / 8, 0u);
    layout.colors = true;
    EXPECT_EQ(vatTexelIndex(VatMode::Vertices, layout, 2, 1, 2), (uint64_t)((2 * 3 + 1) * 3 + 2));
    layout.normals = false;
    layout.colors = false;
    EXPECT_EQ(vatTexelIndex(VatMode::Vertices, layout, 4, 2, 0), 14u);
    // bones: t = (frame * B + bone) * 2 + k
    EXPECT_EQ(vatTexelIndex(VatMode::Bones, layout, 0, 0, 1), 1u);
    EXPECT_EQ(vatTexelIndex(VatMode::Bones, layout, 0, 4, 0), 8u);
    EXPECT_EQ(vatTexelIndex(VatMode::Bones, layout, 3, 2, 1), (uint64_t)((3 * 5 + 2) * 2 + 1));
    // no 32-bit overflow on big payloads
    layout.vertices = VAT_MAX_VERTICES;
    layout.normals = true;
    EXPECT_EQ(vatTexelIndex(VatMode::Vertices, layout, 300, 7, 1), ((uint64_t)300 * VAT_MAX_VERTICES + 7) * 2 + 1);
}

TEST(VatClipLength, ByWrap){
    EXPECT_FLOAT_EQ(makeClip(0, 30, 30, ClipWrapMode::Loop).length(), 1.0f);
    EXPECT_FLOAT_EQ(makeClip(0, 31, 30, ClipWrapMode::Once).length(), 1.0f);
    EXPECT_FLOAT_EQ(makeClip(0, 31, 30, ClipWrapMode::PingPong).length(), 1.0f);
    EXPECT_FLOAT_EQ(makeClip(0, 31, 30, ClipWrapMode::ClampForever).length(), 1.0f);
    EXPECT_FLOAT_EQ(makeClip(0, 1, 30, ClipWrapMode::Once).length(), 0.0f);
    EXPECT_FLOAT_EQ(makeClip(0, 1, 30, ClipWrapMode::Loop).length(), 1.0f / 30.0f);
    EXPECT_FLOAT_EQ(makeClip(0, 10, 0, ClipWrapMode::Loop).length(), 0.0f);
}

TEST(VatFrameMath, LoopIncludingTheSeam){
    VatClip clip = makeClip(10, 4, 4, ClipWrapMode::Loop);   // 1 s, samples at 0, .25, .5, .75
    expectSample(vatSample(clip, 0.0f), 10, 11, 0.0f, 0.0f, false);
    expectSample(vatSample(clip, 0.375f), 11, 12, 0.5f, 0.375f, false);
    // last -> first: the shader interpolates frame 3 into frame 0 (no duplicate end frame)
    expectSample(vatSample(clip, 0.875f), 13, 10, 0.5f, 0.875f, false);
    expectSample(vatSample(clip, 1.0f), 10, 11, 0.0f, 0.0f, false);
    expectSample(vatSample(clip, 2.375f), 11, 12, 0.5f, 0.375f, false);
    // backwards (negative speed gives a negative local time)
    expectSample(vatSample(clip, -0.125f), 13, 10, 0.5f, 0.875f, false);
    expectSample(vatSample(clip, -1.0f), 10, 11, 0.0f, 0.0f, false);
    // a tiny negative time rounds to x = frames in float: kept in range, the first frame's value
    VatFrameSample tiny = vatSample(clip, -1e-9f);
    EXPECT_GE(tiny.frame0, 10);
    EXPECT_LE(tiny.frame0, 13);
    EXPECT_GE(tiny.frame1, 10);
    EXPECT_LE(tiny.frame1, 13);
    float value = (tiny.frame0 - 10) * (1 - tiny.alpha) + (tiny.frame1 - 10) * tiny.alpha;
    EXPECT_NEAR(value, 0.0f, 1e-4f);
    // never finished
    EXPECT_FALSE(vatSample(clip, 1000.0f).finished);
    // interpolate off: the lower sample
    clip.interpolate = false;
    expectSample(vatSample(clip, 0.875f), 13, 10, 0.0f, 0.875f, false);
}

TEST(VatFrameMath, OnceClampsAndFinishes){
    VatClip clip = makeClip(5, 3, 2, ClipWrapMode::Once);    // 1 s, samples at 0, .5, 1
    expectSample(vatSample(clip, 0.0f), 5, 6, 0.0f, 0.0f, false);
    expectSample(vatSample(clip, 0.25f), 5, 6, 0.5f, 0.25f, false);
    expectSample(vatSample(clip, 0.75f), 6, 7, 0.5f, 0.75f, false);
    // exactly at the end: the last frame, not past it yet
    expectSample(vatSample(clip, 1.0f), 7, 7, 0.0f, 1.0f, false);
    expectSample(vatSample(clip, 1.01f), 7, 7, 0.0f, 1.0f, true);
    expectSample(vatSample(clip, 50.0f), 7, 7, 0.0f, 1.0f, true);
    // played backwards: finished before 0
    expectSample(vatSample(clip, -0.01f), 5, 6, 0.0f, 0.0f, true);
    clip.interpolate = false;
    expectSample(vatSample(clip, 0.75f), 6, 7, 0.0f, 0.75f, false);
}

TEST(VatFrameMath, ClampForeverIsOnceThatKeepsPlaying){
    VatClip clip = makeClip(0, 3, 2, ClipWrapMode::ClampForever);
    expectSample(vatSample(clip, 0.25f), 0, 1, 0.5f, 0.25f, false);
    expectSample(vatSample(clip, 3.0f), 2, 2, 0.0f, 1.0f, true);
    expectSample(vatSample(clip, -2.0f), 0, 1, 0.0f, 0.0f, true);
}

TEST(VatFrameMath, PingPongBothLegsAndTurningPoints){
    VatClip clip = makeClip(20, 4, 1, ClipWrapMode::PingPong);   // last 3, period 6 samples
    // forward leg
    expectSample(vatSample(clip, 0.0f), 20, 21, 0.0f, 0.0f, false);
    expectSample(vatSample(clip, 1.5f), 21, 22, 0.5f, 0.5f, false);
    // turning point at the end
    expectSample(vatSample(clip, 3.0f), 23, 23, 0.0f, 1.0f, false);
    // backward leg: x = 6 - 4.5 = 1.5, mixed on the same pair as the forward leg
    expectSample(vatSample(clip, 4.5f), 21, 22, 0.5f, 0.5f, false);
    expectSample(vatSample(clip, 5.0f), 21, 22, 0.0f, 1.0f / 3.0f, false);
    // turning point at the start, next period
    expectSample(vatSample(clip, 6.0f), 20, 21, 0.0f, 0.0f, false);
    expectSample(vatSample(clip, 7.5f), 21, 22, 0.5f, 0.5f, false);
    // negative time mirrors
    expectSample(vatSample(clip, -1.5f), 21, 22, 0.5f, 0.5f, false);
    // never finished
    EXPECT_FALSE(vatSample(clip, 1000.25f).finished);
    // a two-frame ping-pong
    VatClip two = makeClip(0, 2, 1, ClipWrapMode::PingPong);
    expectSample(vatSample(two, 1.25f), 0, 1, 0.75f, 0.75f, false);
}

TEST(VatFrameMath, OneFrameClips){
    for (ClipWrapMode wrap : {ClipWrapMode::Loop, ClipWrapMode::Once, ClipWrapMode::PingPong, ClipWrapMode::ClampForever}){
        VatClip clip = makeClip(7, 1, 30, wrap);
        for (float tau : {0.0f, 0.01f, 0.5f, 3.0f}){
            VatFrameSample s = vatSample(clip, tau);
            EXPECT_EQ(s.frame0, 7);
            EXPECT_EQ(s.frame1, 7);
            EXPECT_EQ(s.alpha, 0.0f);
        }
        if (wrap == ClipWrapMode::Once || wrap == ClipWrapMode::ClampForever){
            EXPECT_FALSE(vatSample(clip, 0.0f).finished);
            EXPECT_TRUE(vatSample(clip, 0.1f).finished);
            EXPECT_EQ(vatSample(clip, 0.1f).normalized, 0.0f);
        }
    }
    // a one-frame loop: phase x / frames within its one sample period
    EXPECT_NEAR(vatSample(makeClip(0, 1, 2, ClipWrapMode::Loop), 0.25f).normalized, 0.5f, 1e-6f);
}

TEST(VatFrameMath, FractionalFpsAndGuards){
    VatClip clip = makeClip(0, 30, 29.97f, ClipWrapMode::Loop);
    VatFrameSample s = vatSample(clip, 0.5f);
    EXPECT_EQ(s.frame0, 14);
    EXPECT_EQ(s.frame1, 15);
    EXPECT_NEAR(s.alpha, 0.985f, 1e-4f);
    // a loop baked from 1.0333 s at 30 fps: frames 31, fps 30.0010
    VatClip baked = makeClip(0, 31, 31.0f / 1.0333f, ClipWrapMode::Loop);
    VatFrameSample end = vatSample(baked, baked.length() - 1e-4f);
    EXPECT_EQ(end.frame0, 30);
    EXPECT_EQ(end.frame1, 0);
    // non-finite times sample the first frame
    expectSample(vatSample(clip, std::numeric_limits<float>::quiet_NaN()), 0, 1, 0.0f, 0.0f, false);
    expectSample(vatSample(clip, std::numeric_limits<float>::infinity()), 0, 1, 0.0f, 0.0f, false);
    // huge times stay in range
    for (ClipWrapMode wrap : {ClipWrapMode::Loop, ClipWrapMode::Once, ClipWrapMode::PingPong}){
        clip.wrap = wrap;
        for (float tau : {1e6f, -1e6f, 2047.999f, 1e30f}){
            VatFrameSample h = vatSample(clip, tau);
            EXPECT_GE(h.frame0, 0);
            EXPECT_LT(h.frame0, 30);
            EXPECT_GE(h.frame1, 0);
            EXPECT_LT(h.frame1, 30);
            EXPECT_GE(h.alpha, 0.0f);
            EXPECT_LE(h.alpha, 1.0f);
        }
    }
}

TEST(VatFrameMath, NormalizedFormFollowsAPhase){
    VatClip loop = makeClip(0, 4, 4, ClipWrapMode::Loop);
    expectSample(vatSampleNormalized(loop, 0.5f), 2, 3, 0.0f, 0.5f, false);
    expectSample(vatSampleNormalized(loop, 0.875f), 3, 0, 0.5f, 0.875f, false);
    expectSample(vatSampleNormalized(loop, 1.0f), 0, 1, 0.0f, 0.0f, false);
    VatClip once = makeClip(3, 3, 2, ClipWrapMode::Once);
    expectSample(vatSampleNormalized(once, 0.5f), 4, 5, 0.0f, 0.5f, false);
    expectSample(vatSampleNormalized(once, 0.75f), 4, 5, 0.5f, 0.75f, false);
    expectSample(vatSampleNormalized(once, 1.0f), 5, 5, 0.0f, 1.0f, false);
    expectSample(vatSampleNormalized(once, 1.5f), 5, 5, 0.0f, 1.0f, true);
    VatClip pingPong = makeClip(0, 4, 1, ClipWrapMode::PingPong);
    expectSample(vatSampleNormalized(pingPong, 0.5f), 1, 2, 0.5f, 0.5f, false);
    // the phase out of vatSample reproduces the same frames
    for (VatClip clip : {loop, once, pingPong}){
        for (float tau : {0.1f, 0.3f, 0.6f, 0.9f, 1.7f}){
            VatFrameSample a = vatSample(clip, tau);
            VatFrameSample b = vatSampleNormalized(clip, a.normalized);
            EXPECT_EQ(a.frame0, b.frame0);
            EXPECT_EQ(a.frame1, b.frame1);
            EXPECT_NEAR(a.alpha, b.alpha, 1e-4f);
        }
    }
    EXPECT_EQ(vatSampleNormalized(loop, std::numeric_limits<float>::quiet_NaN()).frame0, 0);
}

TEST(VatWeightB, FadeBlendAndNoB){
    VatTrackPair pair;
    pair.a.clip = 0;
    EXPECT_EQ(vatWeightB(pair, 5.0f), 0.0f);
    pair.blend = 0.7f;
    EXPECT_EQ(vatWeightB(pair, 5.0f), 0.0f);     // no B
    pair.b.clip = 1;
    EXPECT_FLOAT_EQ(vatWeightB(pair, 5.0f), 0.7f);
    pair.blend = 1.5f;
    EXPECT_EQ(vatWeightB(pair, 5.0f), 1.0f);
    pair.blend = 0.0f;
    EXPECT_EQ(vatWeightB(pair, 5.0f), 0.0f);
    pair.blend = -1.0f;
    pair.fadeStart = 2.0f;
    pair.fadeEnd = 4.0f;
    EXPECT_EQ(vatWeightB(pair, 1.0f), 0.0f);
    EXPECT_EQ(vatWeightB(pair, 2.0f), 0.0f);
    EXPECT_FLOAT_EQ(vatWeightB(pair, 3.0f), 0.5f);
    EXPECT_EQ(vatWeightB(pair, 4.0f), 1.0f);
    EXPECT_EQ(vatWeightB(pair, 9.0f), 1.0f);
    pair.fadeEnd = 2.0f;     // no fade length: B at once
    EXPECT_EQ(vatWeightB(pair, 0.0f), 1.0f);
}

TEST(VatInstancePacking, LayoutAndRoundTrip){
    VatTrackPair pair;
    pair.a = {3, 12.5f, -1.25f};
    pair.b = {7, 14.0f, 2.0f};
    pair.fadeStart = 14.0f;
    pair.fadeEnd = 14.25f;
    Vector4 v0;
    Vector4 v1;
    vatPackInstance(pair, v0, v1);
    EXPECT_EQ(v0, Vector4(3, 12.5f, -1.25f, 7));
    EXPECT_EQ(v1, Vector4(14.0f, 2.0f, 14.0f, 14.25f));
    VatTrackPair back = vatUnpackInstance(v0, v1);
    EXPECT_EQ(back.a.clip, 3);
    EXPECT_EQ(back.a.start, 12.5f);
    EXPECT_EQ(back.a.speed, -1.25f);
    EXPECT_EQ(back.b.clip, 7);
    EXPECT_EQ(back.b.start, 14.0f);
    EXPECT_EQ(back.b.speed, 2.0f);
    EXPECT_EQ(back.fadeStart, 14.0f);
    EXPECT_EQ(back.fadeEnd, 14.25f);
    EXPECT_LT(back.blend, 0.0f);

    // blend mode: w = -(1 + blend) < 0, blend 0 included
    for (float blend : {0.0f, 0.25f, 1.0f}){
        pair.blend = blend;
        vatPackInstance(pair, v0, v1);
        EXPECT_EQ(v1.w, -(1.0f + blend));
        back = vatUnpackInstance(v0, v1);
        EXPECT_EQ(back.blend, blend);
        EXPECT_EQ(back.fadeEnd, 0.0f);
        EXPECT_EQ(vatWeightB(back, 100.0f), vatWeightB(pair, 100.0f));
    }

    // rest (no clips)
    VatTrackPair rest;
    vatPackInstance(rest, v0, v1);
    EXPECT_EQ(v0.x, -1.0f);
    EXPECT_EQ(v0.w, -1.0f);
    back = vatUnpackInstance(v0, v1);
    EXPECT_EQ(back.a.clip, -1);
    EXPECT_EQ(back.b.clip, -1);
    EXPECT_LT(back.blend, 0.0f);

    // a fade that ended before the epoch can't read as blend mode, and weighs the same at now >= 0
    VatTrackPair old;
    old.a.clip = 0;
    old.b.clip = 1;
    old.fadeStart = -3.0f;
    old.fadeEnd = -2.0f;
    vatPackInstance(old, v0, v1);
    EXPECT_GE(v1.w, 0.0f);
    back = vatUnpackInstance(v0, v1);
    EXPECT_LT(back.blend, 0.0f);
    for (float now : {0.0f, 1.0f, 100.0f}) EXPECT_EQ(vatWeightB(back, now), vatWeightB(old, now));
}

TEST(VatLayoutHash, StableAndSensitiveToGeneratedFieldsOnly){
    VatData data = vattest::sampleData();
    std::string hash = computeLayoutHash(data);
    EXPECT_EQ(hash.size(), 16u);
    EXPECT_EQ(hash.find_first_not_of("0123456789abcdef"), std::string::npos);
    EXPECT_EQ(hash, computeLayoutHash(data));
    // a value stored in files: never changes for the same fields
    EXPECT_EQ(hash, "31b8cb67a64457f3");

    std::set<std::string> seen = {hash};
    auto changed = [&](const std::function<void(VatData&)>& edit, const char* what){
        VatData copy = vattest::sampleData();
        edit(copy);
        std::string other = computeLayoutHash(copy);
        EXPECT_NE(other, hash) << what;
        EXPECT_TRUE(seen.insert(other).second) << what << " collides";
    };
    changed([](VatData& d){ d.mode = VatMode::Bones; }, "mode");
    changed([](VatData& d){ d.layout.precision = VatPrecision::Half; }, "precision");
    changed([](VatData& d){ d.layout.width = 16; }, "width");
    changed([](VatData& d){ d.layout.height = 7; }, "height");
    changed([](VatData& d){ d.layout.samples = 6; }, "samples");
    changed([](VatData& d){ d.layout.vertices = 4; }, "vertices");
    changed([](VatData& d){ d.layout.bones = 1; }, "bones");
    changed([](VatData& d){ d.layout.normals = false; }, "normals");
    changed([](VatData& d){ d.layout.colors = true; }, "colors");
    changed([](VatData& d){ d.clips[1].start = 3; }, "clip start");
    changed([](VatData& d){ d.clips[1].frames = 2; }, "clip frames");
    changed([](VatData& d){ d.clips[1].fps = 2.5f; }, "clip fps");
    changed([](VatData& d){ d.clips[1].restNone = true; }, "clip rest");
    changed([](VatData& d){ d.clips.pop_back(); }, "clip removed");

    auto same = [&](const std::function<void(VatData&)>& edit, const char* what){
        VatData copy = vattest::sampleData();
        edit(copy);
        EXPECT_EQ(computeLayoutHash(copy), hash) << what;
    };
    same([](VatData& d){ d.clips[0].name = "Run"; }, "name");
    same([](VatData& d){ d.clips[0].wrap = ClipWrapMode::Once; }, "wrap");
    same([](VatData& d){ d.clips[0].speed = 2; }, "speed");
    same([](VatData& d){ d.clips[0].interpolate = false; }, "interpolate");
    same([](VatData& d){ d.clips[0].events.clear(); }, "events");
    same([](VatData& d){ d.clips[0].bounds = VatBounds(); }, "clip bounds");
    same([](VatData& d){ d.sockets.clear(); }, "sockets");
    same([](VatData& d){ d.recipe.fps = 60; }, "recipe");
    same([](VatData& d){ d.stats.bytes = 1; }, "stats");
    same([](VatData& d){ d.guid.clear(); }, "guid");
    same([](VatData& d){ d.mesh = "other.vat.glb"; }, "mesh");
    same([](VatData& d){ d.layout.layoutHash = "ff"; }, "the hash itself");
}
