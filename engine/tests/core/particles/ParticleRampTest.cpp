// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S49 AC6 (CPU; R12, R13): the Palette Ramp colour mode steps along a ramp without blending (bias
// and reverse), Start Color picks a random step with an even distribution (chi-square over 10^5
// draws), and the packed colour of a ramp-coloured particle is the step's sRGB8 bytes, unchanged.

#include "ParticleTestUtil.h"

#include "memory/Arena.h"
#include "particles/ParticleEval.h"
#include "particles/ParticleRender.h"
#include "pixelart/PixelColor.h"

#include <array>
#include <cmath>
#include <vector>

using namespace particletest;
using jasperengine::pixelart::Rgba8;

namespace {

    const std::vector<Rgba8> kRamp = {{20, 10, 30, 255}, {70, 30, 40, 255}, {140, 40, 30, 255}, {200, 80, 30, 255},
                                      {240, 140, 40, 255}, {255, 200, 90, 255}, {255, 240, 170, 255}, {255, 255, 255, 255}};

    MinMaxGradient ramp(){
        MinMaxGradient g;
        g.mode = GradientValueMode::Ramp;
        g.rampColors = kRamp;
        return g;
    }

    uint32_t bytes(const Rgba8& c){ return c.r | (c.g << 8) | (c.b << 16) | ((uint32_t)c.a << 24); }

}

TEST(ParticleRamp, StepsBiasAndReverse){
    MinMaxGradient g = ramp();
    // pixelfx's at(): floor(t n), never blended
    EXPECT_EQ(g.rampStep(0.0f, 8), 0);
    EXPECT_EQ(g.rampStep(0.124f, 8), 0);
    EXPECT_EQ(g.rampStep(0.125f, 8), 1);
    EXPECT_EQ(g.rampStep(0.999f, 8), 7);
    EXPECT_EQ(g.rampStep(1.0f, 8), 7);
    EXPECT_EQ(jasperengine::pixelart::rampAt(kRamp, 0.4f), kRamp[(size_t)g.rampStep(0.4f, 8)]);
    // bias: t^bias (pixelfx's fire 0.62 runs hotter early)
    g.rampBias = 0.62f;
    EXPECT_EQ(g.rampStep(0.1f, 8), (int)std::floor(std::pow(0.1f, 0.62f) * 8.0f));
    g.rampBias = 1.0f;
    g.rampReverse = true;
    EXPECT_EQ(g.rampStep(0.0f, 8), 7);
    EXPECT_EQ(g.rampStep(1.0f, 8), 0);
    // Steps: fitted (S43 fitRamp), both ends kept
    g.rampReverse = false;
    g.rampSteps = 3;
    const std::vector<Rgba8> fitted = g.resolveRamp();
    ASSERT_EQ(fitted.size(), 3u);
    EXPECT_EQ(fitted.front(), kRamp.front());
    EXPECT_EQ(fitted.back(), kRamp.back());
    // a step's colour is exact (linear in the engine's gamma 2.2, the byte back)
    g.rampSteps = 0;
    for (size_t i = 0; i < kRamp.size(); i++)
        EXPECT_EQ(jasperengine::pixelart::fromEngineLinear(g.evaluate(((float)i + 0.5f) / 8.0f, 0.0f)), kRamp[i]);
}

TEST(ParticleRamp, StartColorRandomStepIsUniform){
    ParticleEffectDocument d;
    d.maxParticles = 100000;
    d.main.startLifetime = MinMaxCurve(10.0f);
    d.main.startColor = ramp();
    d.emission.rateOverTime = MinMaxCurve(0.0f);
    Burst b;
    b.count = MinMaxCurve(100000.0f);
    d.emission.bursts = {b};
    Sim sim(d, 7);
    sim.ticks(1);
    ASSERT_EQ(sim.count(), 100000u);
    std::array<double, 8> counts{};
    const uint32_t* colors = sim.runtime.particleChannels().getU(ChStartColor);
    for (uint32_t i = 0; i < sim.count(); i++){
        int step = -1;
        for (size_t k = 0; k < kRamp.size(); k++){
            if (colors[i] == bytes(kRamp[k])) step = (int)k;
        }
        ASSERT_GE(step, 0) << "a start colour off the ramp's exact bytes: " << std::hex << colors[i];
        counts[(size_t)step] += 1.0;
    }
    double chi2 = 0.0;
    const double expected = 100000.0 / 8.0;
    for (double c : counts) chi2 += (c - expected) * (c - expected) / expected;
    // 7 degrees of freedom: p = 0.001 at 24.32
    EXPECT_LT(chi2, 24.32) << "the random steps are not uniform";
}

TEST(ParticleRamp, PackedColourIsTheStepsBytes){
    // Color over Lifetime a ramp, Start Color white: the drawn bytes are the step's (R13)
    ParticleEffectDocument d;
    d.main.startLifetime = MinMaxCurve(1.0f);
    d.main.startSpeed = MinMaxCurve(0.0f);
    d.emission.rateOverTime = MinMaxCurve(0.0f);
    Burst b;
    b.count = MinMaxCurve(1.0f);
    d.emission.bursts = {b};
    d.colorOverLifetime.present = true;
    d.colorOverLifetime.color = ramp();
    d.pixelArt.present = true;
    d.renderer.renderMode = RenderMode::Pixel;
    d.interpolate = false;
    Sim sim(d, 3);
    for (int tick = 1; tick < 29; tick++){
        sim.ticks(1);
        memory::Arena arena("ParticleRampTest");
        const PackResult pack = ParticleRenderPrep::pack(sim.runtime, RenderView(), arena);
        ASSERT_EQ(pack.count, 1u);
        const float age = sim.ch(ChAge) * sim.ch(ChInvLife);
        const Rgba8 expected = kRamp[(size_t)std::min(7, (int)std::floor(age * 8.0f))];
        EXPECT_EQ(pack.instances[0].color & 0x00ffffffu, bytes(expected) & 0x00ffffffu) << "at normalized age " << age;
        ASSERT_NE(pack.pixel, nullptr);
        EXPECT_EQ((int)pack.pixel[0].rampStep, std::min(7, (int)std::floor(age * 8.0f)));
    }
}
