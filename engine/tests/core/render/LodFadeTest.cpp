// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Detail level selection and cross-fade (spec S54 R5, R6, AC1; lod-crossfade.md §5 tests 1-6, 8):
// the pure functions of render/LodFade.h that RenderSystem plans every fading view with.

#include "render/LodFade.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

using namespace jasperengine;

namespace {
    namespace lodFadeOracle {
        // the rank 0..63 of a pixel in the 8x8 Bayer matrix (includes/lod_fade.glsl lodFadeRank)
        inline int lodFadeRank(int x, int y){
            const int px = x & 7, py = y & 7;
            const int v = px ^ py;
            return ((v & 1) << 5) | ((py & 1) << 4) | ((v & 2) << 2) | ((py & 2) << 1) | ((v & 4) >> 1) | ((py & 4) >> 2);
        }
        // whether a draw of that fade keeps a pixel of that rank (includes/lod_fade.glsl lodFadeKeeps)
        inline bool lodFadeKeeps(float fade, int rank){
            const int k = (int)((fade < 0.0f ? -fade : fade) * LOD_FADE_STEPS + 0.5f);
            return (fade < 0.0f) ? (rank < k) : (rank >= LOD_FADE_STEPS - k);
        }
    }


    // levels of errors 1, 2 and 4 world units: level l fits a budget of at least its error
    const float ERRORS[4] = {0.0f, 1.0f, 2.0f, 4.0f};

    LodFadeInput input(float allowed, uint64_t frame){
        LodFadeInput in;
        in.errors = ERRORS;
        in.numLods = 4;
        in.allowed = allowed;
        in.screenHeight = 200.0f;
        in.viewHeight = 1000.0f;
        in.fadeAllowed = true;
        in.duration = 0.5f;
        in.deltaTime = 1.0f / 60.0f;
        in.frame = frame;
        return in;
    }

    // the share of an 8x8 tile a draw of that fade keeps
    int kept(float fade){
        int count = 0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                count += lodFadeOracle::lodFadeKeeps(fade, lodFadeOracle::lodFadeRank(x, y)) ? 1 : 0;
        return count;
    }
}

// test 8: the rank is a permutation of 0..63, the two sides of every k partition each tile
// exactly, and the kept set only grows with k (a pixel changes level once per fade)
TEST(LodFade, TheTwoSidesOfAFadePartitionEveryPixel){
    bool seen[64] = {};
    for (int y = 0; y < 8; y++){
        for (int x = 0; x < 8; x++){
            const int rank = lodFadeOracle::lodFadeRank(x, y);
            ASSERT_GE(rank, 0);
            ASSERT_LT(rank, 64);
            EXPECT_FALSE(seen[rank]) << "rank " << rank << " twice";
            seen[rank] = true;
            EXPECT_EQ(rank, lodFadeOracle::lodFadeRank(x + 8, y + 16)) << "the pattern repeats every 8 pixels";
        }
    }
    for (int k = 1; k < LOD_FADE_STEPS; k++){
        LodDraw draw;
        draw.from = 0;
        draw.to = 1;
        draw.k = (int8_t)k;
        for (int y = 0; y < 8; y++){
            for (int x = 0; x < 8; x++){
                const int rank = lodFadeOracle::lodFadeRank(x, y);
                const bool from = lodFadeOracle::lodFadeKeeps(draw.fromFade(), rank);
                const bool to = lodFadeOracle::lodFadeKeeps(draw.toFade(), rank);
                EXPECT_NE(from, to) << "k " << k << " pixel " << x << "," << y << " drawn by " << (from ? "both" : "neither");
                if (k > 1){
                    LodDraw before = draw;
                    before.k = (int8_t)(k - 1);
                    if (lodFadeOracle::lodFadeKeeps(before.fromFade(), rank)){
                        EXPECT_TRUE(from) << "k " << k << ": a pixel went back to the other level";
                    }
                }
            }
        }
        EXPECT_EQ(kept(draw.fromFade()), k);
        EXPECT_EQ(kept(draw.toFade()), LOD_FADE_STEPS - k);
    }
}

TEST(LodFade, SelectsTheDeepestLevelThatFits){
    EXPECT_EQ(lodSelect(ERRORS, 4, 0.5f), 0u);
    EXPECT_EQ(lodSelect(ERRORS, 4, 1.0f), 1u);
    EXPECT_EQ(lodSelect(ERRORS, 4, 3.0f), 2u);
    EXPECT_EQ(lodSelect(ERRORS, 4, 100.0f), 3u);
    EXPECT_EQ(lodSelect(ERRORS, 2, 100.0f), 1u);
}

// test 1: a switch fades for the duration (30 frames of 1/60 s), k going down from 64 to 0, and
// stays at the new level
TEST(LodFade, AFadeEndsOnTime){
    MeshLodFade state;
    uint64_t frame = 1;
    EXPECT_EQ(lodFadeUpdate(state, input(0.9f, frame++)).to, 0u);
    int fadingFrames = 0;
    int lastK = LOD_FADE_STEPS;
    LodDraw draw;
    for (int i = 0; i < 40; i++){
        draw = lodFadeUpdate(state, input(1.5f, frame++));
        if (draw.fading()){
            fadingFrames++;
            EXPECT_EQ(draw.from, 0u);
            EXPECT_EQ(draw.to, 1u);
            EXPECT_LT(draw.k, lastK) << "frame " << i;
            lastK = draw.k;
        }
    }
    EXPECT_NEAR(fadingFrames, 30, 1);
    EXPECT_FALSE(draw.fading());
    EXPECT_EQ(draw.to, 1u);
    EXPECT_EQ(draw.dominant(), 1u);
}

// test 2: back to where it came from mid-fade: it reverses from where it is, never a full frame
TEST(LodFade, AFadeReversesWithoutAPop){
    MeshLodFade state;
    uint64_t frame = 1;
    lodFadeUpdate(state, input(0.9f, frame++));
    LodDraw draw;
    for (int i = 0; i < 9; i++)
        draw = lodFadeUpdate(state, input(1.5f, frame++));
    ASSERT_TRUE(draw.fading());
    const int before = draw.k;  // the share level 0 still keeps
    draw = lodFadeUpdate(state, input(0.9f, frame++));
    ASSERT_TRUE(draw.fading()) << "reverses, does not snap";
    EXPECT_EQ(draw.from, 1u);
    EXPECT_EQ(draw.to, 0u);
    // level 0 now keeps 64 - k: about what it kept, a step more
    EXPECT_NEAR(LOD_FADE_STEPS - draw.k, before + 2, 2);
}

// test 3: a third level during a fade waits for its end
TEST(LodFade, AThirdLevelWaitsForTheFadesEnd){
    MeshLodFade state;
    uint64_t frame = 1;
    lodFadeUpdate(state, input(0.9f, frame++));
    lodFadeUpdate(state, input(1.5f, frame++));
    // (under 2x a frame: not a camera cut)
    LodDraw draw = lodFadeUpdate(state, input(2.9f, frame++));
    EXPECT_EQ(draw.to, 1u) << "still 0 to 1";
    for (int i = 0; i < 40 && draw.to != 2u; i++)
        draw = lodFadeUpdate(state, input(2.9f, frame++));
    EXPECT_EQ(draw.from, 1u);
    EXPECT_EQ(draw.to, 2u);
    EXPECT_TRUE(draw.fading());
}

// test 4: the snaps
TEST(LodFade, ChangesSnapWhenAFadeWouldNotShowOrCannotRun){
    auto changeOnce = [](const std::function<void(LodFadeInput&, MeshLodFade&)>& tweak){
        MeshLodFade state;
        lodFadeUpdate(state, input(0.9f, 1));
        LodFadeInput in = input(1.5f, 2);
        tweak(in, state);
        return lodFadeUpdate(state, in);
    };
    EXPECT_TRUE(changeOnce([](LodFadeInput&, MeshLodFade&){}).fading()) << "the control fades";
    // not drawn last frame
    EXPECT_FALSE(changeOnce([](LodFadeInput& in, MeshLodFade&){ in.frame = 3; }).fading());
    // the budget jumped 2.5x in a frame (a camera cut)
    {
        MeshLodFade state;
        lodFadeUpdate(state, input(0.8f, 1));
        const LodDraw draw = lodFadeUpdate(state, input(2.0f, 2));
        EXPECT_FALSE(draw.fading());
        EXPECT_EQ(draw.to, 2u);
    }
    // under 8 pixels tall
    EXPECT_FALSE(changeOnce([](LodFadeInput& in, MeshLodFade&){ in.screenHeight = 7.0f; }).fading());
    // the fade variant not ready, Fade Mode None, Cross Fade off (fadeAllowed false)
    EXPECT_FALSE(changeOnce([](LodFadeInput& in, MeshLodFade&){ in.fadeAllowed = false; }).fading());
    // a forced level
    const LodDraw forced = changeOnce([](LodFadeInput& in, MeshLodFade&){ in.forced = 3; });
    EXPECT_FALSE(forced.fading());
    EXPECT_EQ(forced.to, 3u);
    // the first frame
    MeshLodFade fresh;
    EXPECT_FALSE(lodFadeUpdate(fresh, input(1.5f, 7)).fading());
}

// test 5, AC1: a budget oscillating 5 % around a threshold for 120 frames fades once
// (red control: LOD_HYSTERESIS 0 gives a fade at every crossing)
TEST(LodFade, HysteresisHoldsALevelAtItsThreshold){
    MeshLodFade state;
    uint64_t frame = 1;
    int fades = 0;
    bool wasFading = false;
    lodFadeUpdate(state, input(0.9f, frame++));
    for (int i = 0; i < 120; i++){
        const float allowed = (i % 2 == 0) ? 1.0f * 1.05f : 1.0f * 0.95f;
        const LodDraw draw = lodFadeUpdate(state, input(allowed, frame++));
        if (draw.fading() && !wasFading)
            fades++;
        wasFading = draw.fading();
    }
    EXPECT_EQ(fades, 1);
}

// test 6: the distance fade is continuous at both ends of the band and one draw outside it
TEST(LodFade, TheDistanceFadeIsContinuousOverItsBand){
    LodFadeInput in = input(1.5f, 1);
    in.transitionWidth = 0.1f;
    // level 1's band of the budget is [1, 2): it fades over [1.9, 2)
    in.allowed = 1.85f;
    EXPECT_FALSE(lodDistanceFade(in).fading());
    EXPECT_EQ(lodDistanceFade(in).to, 1u);
    in.allowed = 1.9001f;
    LodDraw start = lodDistanceFade(in);
    EXPECT_TRUE(start.k == 0 || start.k >= 63) << "starts with all of level 1";
    in.allowed = 1.95f;
    const LodDraw middle = lodDistanceFade(in);
    ASSERT_TRUE(middle.fading());
    EXPECT_EQ(middle.from, 1u);
    EXPECT_EQ(middle.to, 2u);
    EXPECT_NEAR(middle.k, 32, 1);
    in.allowed = 1.9999f;
    const LodDraw end = lodDistanceFade(in);
    EXPECT_TRUE(end.k <= 1) << "ends with all of level 2";
    in.allowed = 2.0f;
    EXPECT_FALSE(lodDistanceFade(in).fading());
    EXPECT_EQ(lodDistanceFade(in).to, 2u);
    // no band without a width, or for the coarsest level
    in.allowed = 1.95f;
    in.transitionWidth = 0.0f;
    EXPECT_FALSE(lodDistanceFade(in).fading());
    in.transitionWidth = 0.1f;
    in.allowed = 50.0f;
    EXPECT_EQ(lodDistanceFade(in).to, 3u);
}

// R5: Cull Below Screen Height culls a mesh under that share of the view, fading it out
TEST(LodFade, CullBelowCullsSmallMeshes){
    LodFadeInput in = input(0.5f, 1);
    in.cullBelow = 0.1f;
    in.screenHeight = 50.0f;          // 5 % of the view
    EXPECT_TRUE(lodDistanceFade(in).culled());
    MeshLodFade state;
    in.screenHeight = 200.0f;
    EXPECT_FALSE(lodFadeUpdate(state, in).culled());
    in.frame = 2;
    in.screenHeight = 90.0f;
    const LodDraw out = lodFadeUpdate(state, in);
    ASSERT_TRUE(out.fading()) << "fades out";
    EXPECT_EQ(out.to, LOD_CULLED);
    for (uint64_t f = 3; f < 60; f++){
        in.frame = f;
        lodFadeUpdate(state, in);
    }
    in.frame = 60;
    EXPECT_TRUE(lodFadeUpdate(state, in).culled());
    // it stays culled until it grows past the height by the hysteresis
    in.frame = 61;
    in.screenHeight = 105.0f;
    EXPECT_TRUE(lodFadeUpdate(state, in).culled());
    in.frame = 62;
    in.screenHeight = 120.0f;
    EXPECT_FALSE(lodFadeUpdate(state, in).culled());
}

TEST(LodFade, TheSwitchHeightFollowsTheThreshold){
    // 1 px threshold, an error of 0.01 m on a 2 m mesh: about 200 px tall
    EXPECT_NEAR(lodSwitchHeight(0.01f, 1.0f, 2.0f, 1.0f, 1.0f), 200.0f, 1e-3f);
    EXPECT_NEAR(lodSwitchHeight(0.01f, 2.0f, 4.0f, 1.0f, 1.0f), 200.0f, 1e-3f);
    EXPECT_NEAR(lodSwitchHeight(0.01f, 1.0f, 2.0f, 2.0f, 2.0f), 200.0f, 1e-3f);
    EXPECT_EQ(lodSwitchHeight(0.0f, 1.0f, 2.0f, 1.0f, 1.0f), 0.0f);
}

// R18: selection and fade state of 1,000 meshes a frame stay under 0.05 ms and allocate nothing
// (a camera dollying through their thresholds, so many of them fade)
TEST(LodFadeBench, ThousandMeshesAFrame){
    constexpr int meshes = 1000;
    std::vector<MeshLodFade> states(meshes);
    std::vector<double> samples;
    uint64_t allocations = 0;
    for (uint64_t frame = 1; frame <= 600; frame++){
        testsupport::ThreadAllocationCount counter;
        const auto start = std::chrono::steady_clock::now();
        int fading = 0;
        for (int m = 0; m < meshes; m++){
            LodFadeInput in = input(0.5f + 3.0f * (float)((frame + (uint64_t)m * 7) % 300) / 300.0f, frame);
            in.animated = (m % 4) != 0;
            in.cullBelow = (m % 8 == 0) ? 0.05f : 0.0f;
            const LodDraw draw = in.animated ? lodFadeUpdate(states[(size_t)m], in) : lodDistanceFade(in);
            fading += draw.fading() ? 1 : 0;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        allocations += counter.count();
        samples.push_back(ms);
        EXPECT_GE(fading, 0);
    }
    std::sort(samples.begin(), samples.end());
    const double median = samples[samples.size() / 2];
    std::printf("[bench] LOD selection and fade of 1000 meshes: %.4f ms median (budget 0.05)\n", median);
    if (testsupport::perfSlack() > 0.0){
        EXPECT_LE(median, 0.05 * testsupport::perfSlack());
    }
    if (testsupport::countsAllocations()){
        EXPECT_EQ(allocations, 0u);
    }
}
