// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Glitch schedule (S48 R18, R19): a pure function of the corruption frame and the seed,
// bursts at the rate and length asked, tears and splits in whole source texels within their
// ranges, and the scripted burst on unscaled time.

#include "SceneSettings.h"
#include "pixelart/Glitch.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace {

    bool sameFrame(const GlitchFrame& a, const GlitchFrame& b){
        if (a.frame != b.frame || a.strength != b.strength || a.split != b.split || a.interlace != b.interlace || a.tearCount != b.tearCount)
            return false;
        for (int i = 0; i < a.tearCount; i++){
            if (a.tears[i].row != b.tears[i].row || a.tears[i].height != b.tears[i].height || a.tears[i].shift != b.tears[i].shift)
                return false;
        }
        return true;
    }

}

TEST(GlitchSchedule, PureFunctionOfTheCorruptionFrame){
    PostProcessingSettings s;
    s.glitchEnabled = true;
    s.glitchSeed = 7;
    // every time inside one corruption frame (30 fps) gives the same frame; the next one differs somewhere
    const GlitchFrame a = glitchFrame(s, 10.0, 180);
    EXPECT_EQ(a.frame, 300u);
    EXPECT_TRUE(sameFrame(a, glitchFrame(s, 10.0 + 0.5 / 30.0, 180)));
    EXPECT_TRUE(sameFrame(a, glitchFrame(s, 10.0, 180)));
    s.glitchSeed = 8;
    int differ = 0;
    for (int f = 0; f < 600; f++)
        differ += sameFrame(glitchFrame(s, f / 30.0, 180), [&]{ PostProcessingSettings o = s; o.glitchSeed = 7; return glitchFrame(o, f / 30.0, 180); }()) ? 0 : 1;
    EXPECT_GT(differ, 0) << "the seed moves the schedule";
}

TEST(GlitchSchedule, BurstsFollowRateAndLength){
    PostProcessingSettings s;
    s.glitchEnabled = true;
    s.glitchBurstRate = 0.5f;     // one burst per 2 s
    s.glitchBurstLength = 0.25f;
    s.glitchFps = 30.0f;
    int burstFrames = 0;
    const int frames = 30 * 60;   // a minute
    for (int f = 0; f < frames; f++){
        const GlitchFrame g = glitchFrame(s, f / 30.0, 180);
        if (g.strength > 0.0f){
            burstFrames++;
            EXPECT_GE(g.strength, 0.5f);
            EXPECT_LE(g.strength, 1.0f);
        }
    }
    // 30 bursts of 0.25 s at 30 fps: 7 or 8 frames each
    EXPECT_GE(burstFrames, 30 * 7);
    EXPECT_LE(burstFrames, 30 * 8);

    // rate 0: every frame corrupted at the intensity
    s.glitchBurstRate = 0.0f;
    s.glitchIntensity = 0.6f;
    for (int f = 0; f < 60; f++)
        EXPECT_FLOAT_EQ(glitchFrame(s, f / 30.0, 180).strength, 0.6f);

    // intensity 0: calm forever (no bursts, no slips)
    s.glitchIntensity = 0.0f;
    for (int f = 0; f < 600; f++){
        const GlitchFrame g = glitchFrame(s, f / 30.0, 180);
        EXPECT_EQ(g.strength, 0.0f);
        EXPECT_EQ(g.tearCount, 0);
        EXPECT_EQ(g.split, 0);
    }
}

TEST(GlitchSchedule, CorruptionStaysInItsRanges){
    PostProcessingSettings s;
    s.glitchEnabled = true;
    s.glitchBurstRate = 0.0f;
    s.glitchRgbSplit = 3.0f;
    s.glitchTears = 5;
    s.glitchTearShift = 12.0f;
    s.glitchInterlace = 0.3f;
    int interlaced = 0;
    for (int f = 0; f < 3000; f++){
        const GlitchFrame g = glitchFrame(s, f / 30.0, 90);
        EXPECT_GE(std::abs(g.split), 1);
        EXPECT_LE(std::abs(g.split), 3);
        EXPECT_LE(g.tearCount, 5);
        for (int i = 0; i < g.tearCount; i++){
            EXPECT_GE(g.tears[i].row, 0);
            EXPECT_LT(g.tears[i].row, 90);
            EXPECT_GE(g.tears[i].height, 1);
            EXPECT_LE(g.tears[i].height, 5);
            EXPECT_NE(g.tears[i].shift, 0);
            EXPECT_LE(std::abs(g.tears[i].shift), 12);
        }
        interlaced += g.interlace ? 1 : 0;
    }
    EXPECT_GT(interlaced, 3000 * 25 / 100);
    EXPECT_LT(interlaced, 3000 * 35 / 100);

    // the packing the shader decodes round-trips
    for (const GlitchTear tear : {GlitchTear{0, 1, -64}, GlitchTear{8191, 8, 63}, GlitchTear{179, 3, -7}}){
        const int packed = (int)packGlitchTear(tear);
        EXPECT_EQ((float)packed, packGlitchTear(tear));
        EXPECT_EQ(packed % 8192, tear.row);
        EXPECT_EQ((packed / 8192) % 8 + 1, tear.height);
        EXPECT_EQ(packed / 65536 - 64, tear.shift);
    }
}

TEST(GlitchSchedule, ScriptedBurstOnUnscaledTime){
    Glitch::stop();
    EXPECT_EQ(Glitch::getBurstIntensity(), 0.0f);
    Glitch::burst(0.3f, 0.8f);
    Glitch::burst(0.1f, 0.4f);      // shorter and weaker: keeps 0.3 s at 0.8
    EXPECT_FLOAT_EQ(Glitch::getBurstIntensity(), 0.8f);
    EXPECT_FLOAT_EQ(Glitch::getBurstRemaining(), 0.3f);

    PostProcessingSettings s;
    s.glitchEnabled = true;
    s.glitchIntensity = 0.0f;       // only scripted bursts
    EXPECT_FLOAT_EQ(glitchFrame(s, 1.0, 120, Glitch::getBurstIntensity()).strength, 0.8f);

    Glitch::update(0.2);
    EXPECT_FLOAT_EQ(Glitch::getBurstIntensity(), 0.8f);
    Glitch::update(0.2);
    EXPECT_EQ(Glitch::getBurstIntensity(), 0.0f);
    EXPECT_EQ(glitchFrame(s, 1.0, 120, Glitch::getBurstIntensity()).strength, 0.0f);
}
