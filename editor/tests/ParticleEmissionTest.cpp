// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The emission of version 2 systems (particles spec 6.3 step 2, 9): rate over time counts
// exactly and accumulates fractions across frames, rate over distance spawns per unit
// travelled (spread along the path, never on a teleport), bursts with cycles, intervals and
// probability, the max particles cap, and a long frame split into substeps.

#include <gtest/gtest.h>

#include "particles/ParticleSimulation.h"

#include <cmath>

using namespace jasperengine;

namespace {

    // a system that keeps every particle for 100 s, emitting from a point
    ParticleSystemComponent counter() {
        ParticleSystemComponent ps;
        ps.version = 2;
        ps.main.startLifetime = MinMaxCurve(100.0f);
        ps.main.startSpeed = MinMaxCurve(0.0f);
        ps.main.duration = 100.0f;
        ps.main.autoRandomSeed = false;
        ps.main.randomSeed = 7;
        ps.main.maxParticles = 100000;
        ps.shape.enabled = false;
        ps.emission.rateOverTime = MinMaxCurve(0.0f);
        return ps;
    }

    void run(ParticleSystemComponent& ps, float seconds, float dt, const ParticleEmitterFrame& emitter = ParticleEmitterFrame()) {
        const int steps = (int)std::lround(seconds / dt);
        for (int i = 0; i < steps; i++) particles::step(ps, dt, emitter, ParticleEnvironment());
    }

}

TEST(ParticleEmission, RateOverTimeCountsExactly) {
    ParticleSystemComponent ps = counter();
    ps.emission.rateOverTime = MinMaxCurve(10.0f);
    particles::play(ps, ParticleEmitterFrame(), ParticleEnvironment());
    run(ps, 1.0f, 1.0f / 60.0f);
    EXPECT_EQ(particles::particleCount(ps), 10u);
    run(ps, 1.0f, 1.0f / 60.0f);
    EXPECT_EQ(particles::particleCount(ps), 20u);
}

TEST(ParticleEmission, FractionsAccumulateAcrossFrames) {
    ParticleSystemComponent ps = counter();
    // 3 a second at 60 fps: 0.05 a frame
    ps.emission.rateOverTime = MinMaxCurve(3.0f);
    particles::play(ps, ParticleEmitterFrame(), ParticleEnvironment());
    run(ps, 0.3f, 1.0f / 60.0f);
    EXPECT_EQ(particles::particleCount(ps), 0u) << "0.9 of one so far";
    run(ps, 0.05f, 1.0f / 60.0f);
    EXPECT_EQ(particles::particleCount(ps), 1u);
    run(ps, 2.65f, 1.0f / 60.0f);
    EXPECT_EQ(particles::particleCount(ps), 9u);
}

TEST(ParticleEmission, SpawnsSpreadOverTheFrameAreAgedByWhatIsLeft) {
    ParticleSystemComponent ps = counter();
    ps.emission.rateOverTime = MinMaxCurve(4.0f);
    particles::play(ps, ParticleEmitterFrame(), ParticleEnvironment());
    // one 1 s step (cut in 0.1 s substeps): spawns at 0.25, 0.5, 0.75 and 1.0
    particles::step(ps, 1.0f, ParticleEmitterFrame(), ParticleEnvironment());
    ASSERT_EQ(particles::particleCount(ps), 4u);
    std::vector<float> ages(ps.state.age.begin(), ps.state.age.end());
    std::sort(ages.begin(), ages.end());
    EXPECT_NEAR(ages[0], 0.0f, 1e-4f);
    EXPECT_NEAR(ages[1], 0.25f, 1e-4f);
    EXPECT_NEAR(ages[2], 0.5f, 1e-4f);
    EXPECT_NEAR(ages[3], 0.75f, 1e-4f);
}

TEST(ParticleEmission, RateOverDistance) {
    ParticleSystemComponent ps = counter();
    ps.emission.rateOverDistance = MinMaxCurve(5.0f);
    ps.main.simulationSpace = ParticleSystemSimulationSpace::World;
    ParticleEmitterFrame emitter;
    particles::play(ps, emitter, ParticleEnvironment());
    // 2 units in 20 frames: 10 particles, along the path
    for (int i = 1; i <= 20; i++) {
        emitter.position = Vector3(0.1f * i, 0, 0);
        particles::step(ps, 1.0f / 60.0f, emitter, ParticleEnvironment());
    }
    ASSERT_EQ(particles::particleCount(ps), 10u);
    std::vector<float> xs;
    for (const Vector3& p : ps.state.position) xs.push_back(p.x);
    std::sort(xs.begin(), xs.end());
    for (size_t i = 0; i < xs.size(); i++) {
        EXPECT_NEAR(xs[i], 0.2f * (float)(i + 1), 1e-3f) << "one every 0.2 units";
    }
    // a teleport spawns nothing
    emitter.position = Vector3(500.0f, 0, 0);
    particles::step(ps, 1.0f / 60.0f, emitter, ParticleEnvironment());
    EXPECT_EQ(particles::particleCount(ps), 10u);
    EXPECT_TRUE(ps.state.teleportWarned);
}

TEST(ParticleEmission, BurstCyclesAndIntervals) {
    ParticleSystemComponent ps = counter();
    ParticleEmissionBurst burst;
    burst.time = 0.2f;
    burst.count = MinMaxCurve(5.0f);
    burst.cycles = 3;
    burst.interval = 0.5f;
    ps.emission.bursts = {burst};
    particles::play(ps, ParticleEmitterFrame(), ParticleEnvironment());
    run(ps, 0.19f, 0.01f);
    EXPECT_EQ(particles::particleCount(ps), 0u);
    run(ps, 0.02f, 0.01f);
    EXPECT_EQ(particles::particleCount(ps), 5u) << "at 0.2";
    run(ps, 0.5f, 0.01f);
    EXPECT_EQ(particles::particleCount(ps), 10u) << "at 0.7";
    run(ps, 0.5f, 0.01f);
    EXPECT_EQ(particles::particleCount(ps), 15u) << "at 1.2";
    run(ps, 2.0f, 0.01f);
    EXPECT_EQ(particles::particleCount(ps), 15u) << "3 cycles only";
}

TEST(ParticleEmission, BurstProbability) {
    ParticleSystemComponent ps = counter();
    ParticleEmissionBurst burst;
    burst.time = 0.0f;
    burst.count = MinMaxCurve(1.0f);
    burst.cycles = 1000;
    burst.interval = 0.01f;
    burst.probability = 0.5f;
    ps.emission.bursts = {burst};
    particles::play(ps, ParticleEmitterFrame(), ParticleEnvironment());
    run(ps, 10.0f, 0.05f);
    const float fired = (float)particles::particleCount(ps);
    EXPECT_NEAR(fired / 1000.0f, 0.5f, 0.05f);
}

TEST(ParticleEmission, LoopingReArmsBursts) {
    ParticleSystemComponent ps = counter();
    ps.main.duration = 1.0f;
    ParticleEmissionBurst burst;
    burst.time = 0.0f;
    burst.count = MinMaxCurve(4.0f);
    ps.emission.bursts = {burst};
    particles::play(ps, ParticleEmitterFrame(), ParticleEnvironment());
    run(ps, 2.5f, 1.0f / 60.0f);
    EXPECT_EQ(particles::particleCount(ps), 12u) << "at 0, 1 and 2 s";
    EXPECT_EQ(ps.state.loops, 2);
}

TEST(ParticleEmission, MaxParticlesCap) {
    ParticleSystemComponent ps = counter();
    ps.main.maxParticles = 25;
    ps.emission.rateOverTime = MinMaxCurve(100.0f);
    particles::play(ps, ParticleEmitterFrame(), ParticleEnvironment());
    run(ps, 1.0f, 1.0f / 60.0f);
    EXPECT_EQ(particles::particleCount(ps), 25u);
}

TEST(ParticleEmission, LongFrameIsSubstepped) {
    // half a second in one frame emits and integrates as five 0.1 s steps
    ParticleSystemComponent a = counter();
    a.emission.rateOverTime = MinMaxCurve(20.0f);
    a.main.startLifetime = MinMaxCurve(0.33f);
    a.main.startSpeed = MinMaxCurve(1.0f);
    a.shape.enabled = true;
    a.shape.shapeType = ParticleSystemShapeType::Cone;
    a.shape.angle = 0.0f;
    a.shape.radius = 0.0f;
    ParticleSystemComponent b = a;
    particles::play(a, ParticleEmitterFrame(), ParticleEnvironment());
    particles::play(b, ParticleEmitterFrame(), ParticleEnvironment());
    particles::step(a, 0.5f, ParticleEmitterFrame(), ParticleEnvironment());
    for (int i = 0; i < 5; i++) particles::step(b, 0.1f, ParticleEmitterFrame(), ParticleEnvironment());
    ASSERT_EQ(particles::particleCount(a), particles::particleCount(b));
    // particles older than their 0.33 s life are gone in both: those of 0.2 .. 0.5 s live
    EXPECT_EQ(particles::particleCount(a), 7u);
    for (size_t i = 0; i < particles::particleCount(a); i++) {
        EXPECT_NEAR(a.state.position[i].y, b.state.position[i].y, 1e-4f);
        EXPECT_NEAR(a.state.position[i].y, a.state.age[i], 1e-4f) << "speed 1 straight up";
    }
}

TEST(ParticleEmission, StartDelayHoldsTheClock) {
    ParticleSystemComponent ps = counter();
    ps.main.startDelay = MinMaxCurve(0.5f);
    ps.emission.rateOverTime = MinMaxCurve(10.0f);
    particles::play(ps, ParticleEmitterFrame(), ParticleEnvironment());
    run(ps, 0.5f, 0.05f);
    EXPECT_EQ(particles::particleCount(ps), 0u);
    EXPECT_NEAR(ps.state.time, 0.0f, 1e-5f);
    run(ps, 1.0f, 0.05f);
    EXPECT_EQ(particles::particleCount(ps), 10u);
}
