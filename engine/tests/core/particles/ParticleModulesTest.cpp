// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// AC2: one analytic test per module, a particle (or a few) with a fixed seed checked against
// closed-form values.

#include "ParticleTestUtil.h"

#include "memory/Arena.h"
#include "particles/ParticleRender.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace particletest;

TEST(ParticleModules, GravityParabola){
    ParticleEffectDocument d = singleParticle(Vector3(3.0f, 5.0f, 0.0f));
    d.main.gravityModifier = MinMaxCurve(1.0f);
    Sim sim(d);
    sim.ticks(1);
    ASSERT_EQ(sim.count(), 1u);
    sim.ticks(60);
    const float t = 1.0f;
    EXPECT_NEAR(sim.ch(ChPosX), 3.0f * t, 1e-4f);
    EXPECT_NEAR(sim.ch(ChPosY), 5.0f * t - 0.5f * 10.0f * t * t, 1e-4f);
    EXPECT_NEAR(sim.ch(ChVelY), 5.0f - 10.0f * t, 1e-4f);
    EXPECT_NEAR(sim.ch(ChAge), t, 1e-5f);
}

TEST(ParticleModules, GravityModifierScales){
    // half gravity: the parabola halves
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.main.gravityModifier = MinMaxCurve(0.5f);
    Sim sim(d);
    sim.ticks(61);
    EXPECT_NEAR(sim.ch(ChPosY), -0.5f * 5.0f, 1e-4f);
}

TEST(ParticleModules, LinearDragDecaysExponentially){
    ParticleEffectDocument d = singleParticle(Vector3(10.0f, 0.0f, 0.0f));
    d.limitVelocity.present = true;
    d.limitVelocity.limit = MinMaxCurve(1000.0f);
    d.limitVelocity.drag = MinMaxCurve(0.5f);
    d.limitVelocity.multiplyBySize = false;
    Sim sim(d);
    sim.ticks(1);
    sim.ticks(60);
    EXPECT_NEAR(sim.ch(ChVelX), 10.0f * std::exp(-0.5f), 1e-4f);
    // x = v0 / k (1 - e^-kt)
    EXPECT_NEAR(sim.ch(ChPosX), 10.0f / 0.5f * (1.0f - std::exp(-0.5f)), 1e-3f);
}

TEST(ParticleModules, QuadraticDragFollowsItsClosedForm){
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 8.0f, 0.0f));
    d.limitVelocity.present = true;
    d.limitVelocity.limit = MinMaxCurve(1000.0f);
    d.limitVelocity.quadraticDrag = 0.25f;
    d.limitVelocity.multiplyBySize = false;
    Sim sim(d);
    sim.ticks(61);
    // s' = -k s^2: s = s0 / (1 + k s0 t)
    EXPECT_NEAR(sim.ch(ChVelY), 8.0f / (1.0f + 0.25f * 8.0f * 1.0f), 1e-4f);
}

TEST(ParticleModules, LimitVelocityDampens){
    for (float dampen : {1.0f, 0.5f}){
        ParticleEffectDocument d = singleParticle(Vector3(10.0f, 0.0f, 0.0f), 30);
        d.limitVelocity.present = true;
        d.limitVelocity.limit = MinMaxCurve(4.0f);
        d.limitVelocity.dampen = dampen;
        Sim sim(d);
        sim.ticks(1);
        EXPECT_NEAR(sim.ch(ChVelX), 10.0f, 1e-5f);
        sim.ticks(1);
        // a 1/30 s tick removes `dampen` of the excess
        EXPECT_NEAR(sim.ch(ChVelX), 4.0f + 6.0f * (1.0f - dampen), 1e-4f) << dampen;
    }
}

TEST(ParticleModules, OrbitKeepsTheRadius){
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.shape.enabled = true;
    d.shape.shape = ShapeType::Sphere;
    d.shape.radius = 0.0f;
    d.shape.position = Vector3(2.0f, 0.0f, 0.0f);
    d.orbit.present = true;
    d.orbit.axis = Vector3(0.0f, 1.0f, 0.0f);
    d.orbit.angularSpeed = MinMaxCurve(90.0f);
    Sim sim(d);
    sim.ticks(1);
    for (int i = 0; i < 1000; i++){
        sim.ticks(1);
        const float r = std::sqrt(sim.ch(ChPosX) * sim.ch(ChPosX) + sim.ch(ChPosZ) * sim.ch(ChPosZ));
        ASSERT_NEAR(r, 2.0f, 1e-3f) << "tick " << i;
    }
    // a quarter turn per second: after 1000 / 60 s the angle is 1500 degrees (60 past 4 turns)
    const float angle = std::atan2(-sim.ch(ChPosZ), sim.ch(ChPosX)) * 57.2957795f;
    EXPECT_NEAR(std::fmod(angle + 360.0f, 360.0f), std::fmod(90.0f * 1000.0f / 60.0f, 360.0f), 0.05f);
}

TEST(ParticleModules, OrbitRadialPullStopsAtTheAxis){
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.shape.enabled = true;
    d.shape.shape = ShapeType::Sphere;
    d.shape.radius = 0.0f;
    d.shape.position = Vector3(1.0f, 0.0f, 0.0f);
    d.orbit.present = true;
    d.orbit.angularSpeed = MinMaxCurve(0.0f);
    d.orbit.radialPull = MinMaxCurve(1.0f);
    Sim sim(d);
    sim.ticks(31);
    EXPECT_NEAR(sim.ch(ChPosX), 0.5f, 1e-4f);
    sim.ticks(60);
    EXPECT_NEAR(sim.ch(ChPosX), 0.0f, 1e-4f);
}

TEST(ParticleModules, ForceOverLifetimeIsAnAcceleration){
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.forceOverLifetime.present = true;
    d.forceOverLifetime.x = MinMaxCurve(2.0f);
    Sim sim(d);
    sim.ticks(61);
    EXPECT_NEAR(sim.ch(ChPosX), 0.5f * 2.0f, 1e-4f);
    EXPECT_NEAR(sim.ch(ChVelX), 2.0f, 1e-4f);
}

TEST(ParticleModules, VelocityOverLifetimeAddsDisplacementOnly){
    ParticleEffectDocument d = singleParticle(Vector3(1.0f, 0.0f, 0.0f));
    d.velocityOverLifetime.present = true;
    d.velocityOverLifetime.y = MinMaxCurve(3.0f);
    Sim sim(d);
    sim.ticks(61);
    EXPECT_NEAR(sim.ch(ChPosX), 1.0f, 1e-4f);
    EXPECT_NEAR(sim.ch(ChPosY), 3.0f, 1e-4f);
    // the stored velocity is the particle's own
    EXPECT_NEAR(sim.ch(ChVelY), 0.0f, 1e-6f);
}

TEST(ParticleModules, SpeedModifierScalesTheMotion){
    ParticleEffectDocument d = singleParticle(Vector3(2.0f, 0.0f, 0.0f));
    d.velocityOverLifetime.present = true;
    d.velocityOverLifetime.speedModifier = MinMaxCurve(0.5f);
    Sim sim(d);
    sim.ticks(61);
    EXPECT_NEAR(sim.ch(ChPosX), 1.0f, 1e-4f);
}

TEST(ParticleModules, NoiseMovesAndStaysDeterministic){
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.noise.present = true;
    d.noise.strength = MinMaxCurve(2.0f);
    d.noise.damping = false;
    Sim a(d), b(d);
    a.ticks(31);
    b.ticks(31);
    const float moved = std::sqrt(a.ch(ChPosX) * a.ch(ChPosX) + a.ch(ChPosY) * a.ch(ChPosY) + a.ch(ChPosZ) * a.ch(ChPosZ));
    EXPECT_GT(moved, 1e-3f);
    // at most strength * time (the curl's magnitude is bounded by about 2.5 per axis)
    EXPECT_LT(moved, 2.0f * 0.5f * 6.0f);
    EXPECT_EQ(a.runtime.stateHash(), b.runtime.stateHash());
}

TEST(ParticleModules, SizeAndColorCurvesOverLifetime){
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.main.startLifetime = MinMaxCurve(1.0001f);
    d.main.startSize = MinMaxCurve(2.0f);
    d.sizeOverLifetime.present = true;
    d.sizeOverLifetime.size = MinMaxCurve::fromCurve(Curve::polyline({{0.0f, 0.5f}, {0.5f, 1.0f}, {1.0f, 0.25f}}));
    d.colorOverLifetime.present = true;
    d.colorOverLifetime.color.mode = GradientValueMode::Gradient;
    d.colorOverLifetime.color.gradient = Gradient::twoColors(Vector3(1.0f, 1.0f, 1.0f), Vector3(0.0f, 0.0f, 0.0f), 1.0f, 0.0f);
    d.colorOverLifetime.color.gradient.linear = true;
    Sim sim(d);
    float color[4], size, rotation;
    sim.ticks(1);
    sim.runtime.appearance(0, color, size, rotation);
    EXPECT_NEAR(size, 2.0f * 0.5f, 1e-4f);
    EXPECT_NEAR(color[0], 1.0f, 1e-4f);
    EXPECT_NEAR(color[3], 1.0f, 1e-4f);
    sim.ticks(30);
    sim.runtime.appearance(0, color, size, rotation);
    EXPECT_NEAR(size, 2.0f * 1.0f, 1e-3f);
    EXPECT_NEAR(color[0], 0.5f, 2e-3f);
    EXPECT_NEAR(color[3], 0.5f, 2e-3f);
    sim.ticks(30);
    ASSERT_EQ(sim.count(), 1u);
    sim.runtime.appearance(0, color, size, rotation);
    EXPECT_NEAR(size, 2.0f * 0.25f, 2e-3f);
    EXPECT_NEAR(color[0], 0.0f, 2e-3f);
    EXPECT_NEAR(color[3], 0.0f, 2e-3f);
}

TEST(ParticleModules, RotationOverLifetimeIsAClosedForm){
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.main.startRotation = MinMaxCurve(30.0f);
    d.rotationOverLifetime.present = true;
    d.rotationOverLifetime.angularVelocity = MinMaxCurve(90.0f);
    Sim sim(d);
    sim.ticks(61);
    float color[4], size, rotation;
    sim.runtime.appearance(0, color, size, rotation);
    EXPECT_NEAR(rotation * 57.2957795f, 30.0f + 90.0f, 1e-3f);
}

TEST(ParticleModules, TextureSheetFrameAndBlend){
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.main.startLifetime = MinMaxCurve(1.0f);
    d.textureSheetAnimation.present = true;
    d.textureSheetAnimation.tilesX = 8;
    d.textureSheetAnimation.tilesY = 8;
    d.textureSheetAnimation.frameBlending = true;
    d.interpolate = false;
    Sim sim(d);
    sim.ticks(1 + 30);
    memory::Arena arena("ParticleModulesTest");
    RenderView view;
    PackResult pack = ParticleRenderPrep::pack(sim.runtime, view, arena);
    ASSERT_EQ(pack.count, 1u);
    float frame = (float)(pack.instances[0].rotationFrame >> 16) / 65535.0f * 64.0f;
    EXPECT_NEAR(frame, 32.0f, 0.01f);
    // 31/60 of the life: frame 33, 7% into the next
    sim.ticks(1);
    pack = ParticleRenderPrep::pack(sim.runtime, view, arena);
    frame = (float)(pack.instances[0].rotationFrame >> 16) / 65535.0f * 64.0f;
    const float expected = (31.0f / 60.0f) * 64.0f;
    EXPECT_NEAR(std::floor(frame), std::floor(expected), 0.0f);
    EXPECT_NEAR(frame - std::floor(frame), expected - std::floor(expected), 0.01f);
}

TEST(ParticleModules, BurstsWithCyclesAndProbability){
    // 5 cycles of 10, each kept with probability 0.4: 20 a seed on average, variance 5 * 0.4 * 0.6 * 100
    ParticleEffectDocument d;
    d.tickRate = 30;
    d.main.startLifetime = MinMaxCurve(10.0f);
    d.emission.rateOverTime = MinMaxCurve(0.0f);
    Burst b;
    b.count = MinMaxCurve(10.0f);
    b.cycles = 5;
    b.interval = 0.1f;
    b.probability = 0.4f;
    d.emission.bursts = {b};
    const auto effect = compileOrFail(d);
    double sum = 0.0;
    const int seeds = 1000;
    for (int s = 1; s <= seeds; s++){
        EmitterRuntime runtime;
        runtime.bind(effect);
        runtime.reset((uint32_t)s * 7919u);
        runtime.play();
        EmitterInput input;
        input.seed = (uint32_t)s * 7919u;
        for (int t = 0; t < 30; t++) runtime.step(input);
        const uint32_t n = runtime.count();
        EXPECT_EQ(n % 10u, 0u);
        sum += n;
    }
    const double mean = 20.0 * seeds;
    const double sigma = std::sqrt(seeds * 5 * 0.4 * 0.6 * 100.0);
    EXPECT_NEAR(sum, mean, 3.0 * sigma);
}

TEST(ParticleModules, BurstRandomCountsAreUniform){
    ParticleEffectDocument d;
    d.tickRate = 30;
    d.main.startLifetime = MinMaxCurve(10.0f);
    d.emission.rateOverTime = MinMaxCurve(0.0f);
    Burst b;
    b.count = MinMaxCurve::twoConstants(5.0f, 15.0f);
    d.emission.bursts = {b};
    const auto effect = compileOrFail(d);
    double sum = 0.0;
    uint32_t lo = 1000, hi = 0;
    const int seeds = 1000;
    for (int s = 1; s <= seeds; s++){
        EmitterRuntime runtime;
        runtime.bind(effect);
        runtime.reset((uint32_t)s);
        runtime.play();
        EmitterInput input;
        input.seed = (uint32_t)s;
        runtime.step(input);
        const uint32_t n = runtime.count();
        lo = std::min(lo, n);
        hi = std::max(hi, n);
        sum += n;
    }
    EXPECT_EQ(lo, 5u);
    EXPECT_EQ(hi, 15u);
    // uniform on 5..15: mean 10, variance 10
    EXPECT_NEAR(sum / seeds, 10.0, 3.0 * std::sqrt(10.0 / seeds));
}

TEST(ParticleModules, RateOverTimeSpreadsOverTheTick){
    ParticleEffectDocument d;
    d.tickRate = 60;
    d.main.startLifetime = MinMaxCurve(100.0f);
    d.main.startSpeed = MinMaxCurve(0.0f);
    d.shape.enabled = false;
    d.startVelocity.present = true;
    d.startVelocity.min = d.startVelocity.max = Vector3(6.0f, 0.0f, 0.0f);
    d.emission.rateOverTime = MinMaxCurve(120.0f);
    Sim sim(d);
    sim.ticks(1);
    // two a tick, born at 1/3 and 2/3 of it and moved to its end
    ASSERT_EQ(sim.count(), 2u);
    EXPECT_NEAR(sim.ch(ChPosX, 0), 6.0f * (2.0f / 3.0f) / 60.0f, 1e-5f);
    EXPECT_NEAR(sim.ch(ChPosX, 1), 6.0f * (1.0f / 3.0f) / 60.0f, 1e-5f);
    sim.ticks(59);
    EXPECT_EQ(sim.count(), 120u);
}

TEST(ParticleModules, RateOverDistanceFollowsTheEmitter){
    ParticleEffectDocument d;
    d.tickRate = 30;
    d.simulationSpace = SimulationSpace::World;
    d.main.startLifetime = MinMaxCurve(100.0f);
    d.emission.rateOverTime = MinMaxCurve(0.0f);
    d.emission.rateOverDistance = MinMaxCurve(4.0f);
    Sim sim(d);
    sim.ticks(1);
    EXPECT_EQ(sim.count(), 0u);
    for (int i = 1; i <= 30; i++){
        sim.moveTo(0.1f * i, 0.0f, 0.0f);
        sim.ticks(1);
    }
    // 3 units at 4 per unit
    EXPECT_NEAR((float)sim.count(), 12.0f, 1.0f);
}

TEST(ParticleModules, MaxParticlesCaps){
    ParticleEffectDocument d;
    d.maxParticles = 50;
    d.main.startLifetime = MinMaxCurve(100.0f);
    d.emission.rateOverTime = MinMaxCurve(1000.0f);
    Sim sim(d);
    sim.ticks(30);
    EXPECT_EQ(sim.count(), 50u);
}

TEST(ParticleModules, StartDelayAndDurationOfANonLoopingEffect){
    ParticleEffectDocument d;
    d.tickRate = 10;
    d.looping = false;
    d.duration = 1.0f;
    d.startDelay = 0.5f;
    d.main.startLifetime = MinMaxCurve(0.25f);
    d.emission.rateOverTime = MinMaxCurve(10.0f);
    Sim sim(d);
    sim.ticks(5);
    EXPECT_EQ(sim.count(), 0u);
    EXPECT_TRUE(sim.runtime.isEmitting());
    sim.ticks(10);
    EXPECT_FALSE(sim.runtime.isEmitting());
    sim.ticks(5);
    EXPECT_EQ(sim.count(), 0u);
    EXPECT_EQ(sim.runtime.state(), PlayState::Stopped);
}

TEST(ParticleModules, StartLifetimeRangeAndDeaths){
    ParticleEffectDocument d;
    d.tickRate = 30;
    d.main.startLifetime = MinMaxCurve::twoConstants(0.5f, 1.0f);
    d.emission.rateOverTime = MinMaxCurve(0.0f);
    Burst b;
    b.count = MinMaxCurve(1000.0f);
    d.emission.bursts = {b};
    d.maxParticles = 1000;
    Sim sim(d);
    sim.ticks(1);
    ASSERT_EQ(sim.count(), 1000u);
    for (uint32_t i = 0; i < sim.count(); i++){
        const float life = 1.0f / sim.ch(ChInvLife, i);
        ASSERT_GE(life, 0.5f - 1e-5f);
        ASSERT_LE(life, 1.0f + 1e-5f);
    }
    sim.ticks(15);   // 0.5 s
    EXPECT_GT(sim.count(), 900u);
    sim.ticks(16);
    EXPECT_EQ(sim.count(), 0u);
}

TEST(ParticleModules, ShapesStayInsideTheirVolumes){
    struct Case { ShapeType shape; ShapeEmitFrom from; };
    for (const Case c : {Case{ShapeType::Sphere, ShapeEmitFrom::Volume}, Case{ShapeType::Hemisphere, ShapeEmitFrom::Volume},
                         Case{ShapeType::Cone, ShapeEmitFrom::Base}, Case{ShapeType::Cone, ShapeEmitFrom::Volume},
                         Case{ShapeType::Box, ShapeEmitFrom::Volume}, Case{ShapeType::Box, ShapeEmitFrom::Shell},
                         Case{ShapeType::Box, ShapeEmitFrom::Edge}, Case{ShapeType::Circle, ShapeEmitFrom::Volume},
                         Case{ShapeType::Donut, ShapeEmitFrom::Volume}, Case{ShapeType::Edge, ShapeEmitFrom::Volume},
                         Case{ShapeType::Rectangle, ShapeEmitFrom::Volume}}){
        ParticleEffectDocument d;
        d.tickRate = 30;
        d.main.startLifetime = MinMaxCurve(10.0f);
        d.main.startSpeed = MinMaxCurve(1.0f);
        d.emission.rateOverTime = MinMaxCurve(0.0f);
        Burst b;
        b.count = MinMaxCurve(2000.0f);
        d.emission.bursts = {b};
        d.maxParticles = 2000;
        d.shape.shape = c.shape;
        d.shape.emitFrom = c.from;
        d.shape.radius = 2.0f;
        d.shape.radiusThickness = 0.5f;
        d.shape.angle = 30.0f;
        d.shape.length = 3.0f;
        if (c.shape == ShapeType::Box || c.shape == ShapeType::Rectangle)
            d.shape.scale = Vector3(2.0f, 4.0f, 6.0f);
        d.shape.donutRadius = 0.5f;
        Sim sim(d);
        sim.ticks(1);
        ASSERT_EQ(sim.count(), 2000u);
        for (uint32_t i = 0; i < sim.count(); i++){
            const float x = sim.ch(ChPosX, i), y = sim.ch(ChPosY, i), z = sim.ch(ChPosZ, i);
            const float vx = sim.ch(ChVelX, i), vy = sim.ch(ChVelY, i), vz = sim.ch(ChVelZ, i);
            const float r = std::sqrt(x * x + y * y + z * z);
            const float speed = std::sqrt(vx * vx + vy * vy + vz * vz);
            ASSERT_NEAR(speed, 1.0f, 1e-4f) << (int)c.shape;
            switch (c.shape){
                case ShapeType::Sphere:
                    ASSERT_GE(r, 1.0f - 1e-4f); ASSERT_LE(r, 2.0f + 1e-4f); break;
                case ShapeType::Hemisphere:
                    ASSERT_GE(r, 1.0f - 1e-4f); ASSERT_LE(r, 2.0f + 1e-4f); ASSERT_GE(y, -1e-5f); break;
                case ShapeType::Cone: {
                    // the direction within the angle, the position within the frustum
                    ASSERT_GE(vy, std::cos(30.0f * 0.0174533f) - 1e-4f);
                    ASSERT_GE(y, -1e-5f);
                    ASSERT_LE(y, (c.from == ShapeEmitFrom::Volume ? 3.0f : 0.0f) + 1e-4f);
                    const float radial = std::sqrt(x * x + z * z);
                    ASSERT_LE(radial, 2.0f + y * std::tan(30.0f * 0.0174533f) + 1e-4f);
                    break;
                }
                case ShapeType::Box:
                    ASSERT_LE(std::fabs(x), 1.0f + 1e-5f); ASSERT_LE(std::fabs(y), 2.0f + 1e-5f); ASSERT_LE(std::fabs(z), 3.0f + 1e-5f);
                    if (c.from == ShapeEmitFrom::Edge){
                        // two of the three coordinates at the box's faces
                        const int onFace = (std::fabs(std::fabs(x) - 1.0f) < 1e-4f) + (std::fabs(std::fabs(y) - 2.0f) < 1e-4f) + (std::fabs(std::fabs(z) - 3.0f) < 1e-4f);
                        ASSERT_GE(onFace, 2);
                    }
                    if (c.from == ShapeEmitFrom::Shell){
                        const int onFace = (std::fabs(std::fabs(x) - 1.0f) < 1e-4f) + (std::fabs(std::fabs(y) - 2.0f) < 1e-4f) + (std::fabs(std::fabs(z) - 3.0f) < 1e-4f);
                        ASSERT_GE(onFace, 1);
                    }
                    break;
                case ShapeType::Circle:
                    ASSERT_NEAR(y, 0.0f, 1e-6f); ASSERT_GE(r, 1.0f - 1e-4f); ASSERT_LE(r, 2.0f + 1e-4f); break;
                case ShapeType::Donut: {
                    const float ring = std::sqrt(x * x + z * z);
                    const float tube = std::sqrt((ring - 2.0f) * (ring - 2.0f) + y * y);
                    ASSERT_LE(tube, 0.5f + 1e-4f);
                    break;
                }
                case ShapeType::Edge:
                    ASSERT_LE(std::fabs(x), 2.0f + 1e-5f); ASSERT_NEAR(y, 0.0f, 1e-6f); ASSERT_NEAR(z, 0.0f, 1e-6f); break;
                case ShapeType::Rectangle:
                    ASSERT_LE(std::fabs(x), 1.0f + 1e-5f); ASSERT_NEAR(y, 0.0f, 1e-6f); ASSERT_LE(std::fabs(z), 3.0f + 1e-5f); break;
                default: break;
            }
        }
    }
}

TEST(ParticleModules, InheritVelocityTakesTheEmittersMotion){
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f), 30);
    d.emission.bursts[0].time = 0.1f;
    d.main.inheritVelocity = MinMaxCurve(0.5f);
    Sim sim(d);
    // the emitter moves 0.1 a tick: 3 units per second
    for (int i = 0; i < 5; i++){
        sim.moveTo(0.1f * i, 0.0f, 0.0f);
        sim.ticks(1);
    }
    ASSERT_EQ(sim.count(), 1u);
    EXPECT_NEAR(sim.ch(ChVelX), 1.5f, 1e-4f);
}

TEST(ParticleModules, CustomDataFollowsItsCurves){
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.main.startLifetime = MinMaxCurve(1.0f);
    d.customData.present = true;
    d.customData.data[0] = MinMaxCurve(3.0f);
    d.customData.data[1] = MinMaxCurve::fromCurve(Curve::linear(0.0f, 2.0f));
    d.interpolate = false;
    Sim sim(d);
    sim.ticks(31);
    memory::Arena arena("ParticleModulesTest");
    const PackResult pack = ParticleRenderPrep::pack(sim.runtime, RenderView(), arena);
    ASSERT_EQ(pack.count, 1u);
    ASSERT_NE(pack.custom, nullptr);
    EXPECT_NEAR(ParticleRenderPrep::fromHalf(pack.custom[0].data[0]), 3.0f, 1e-3f);
    EXPECT_NEAR(ParticleRenderPrep::fromHalf(pack.custom[0].data[1]), 1.0f, 2e-3f);
}

TEST(ParticleModules, SetOverAgeHoldsTheLastValueOutsideItsWindow){
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.setOverAge.present = true;
    SetOverAgeEntry e;
    e.property = SetProperty::Velocity;
    e.fromTime = 0.0f;
    e.toTime = 0.51f;
    e.from = Vector4(0, 0, 0, 0);
    e.to = Vector4(4, 0, 0, 0);
    d.setOverAge.entries = {e};
    Sim sim(d);
    sim.ticks(1 + 15);
    // set at the age the tick starts: 14/60 s
    EXPECT_NEAR(sim.ch(ChVelX), 4.0f * (14.0f / 60.0f) / 0.51f, 1e-3f);
    sim.ticks(60);
    // past the window it keeps the last value set (at age 30/60 s, the last tick inside)
    EXPECT_NEAR(sim.ch(ChVelX), 4.0f * 0.5f / 0.51f, 1e-3f);
}

TEST(ParticleModules, SetOverAgeFramesStayInsideTheList){
    // the old sprite modifier's ends (upstream's fix of it): the window's end (1) is the last
    // frame, not one past it; an ease outside [0, 1] sets nothing (as the old modifier), and its
    // lanes never index the list with a negative position cast to an unsigned one
    for (float ease : {-1.0f, 1.0f}){
        ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
        d.setOverAge.present = true;
        SetOverAgeEntry e;
        e.property = SetProperty::Frame;
        e.fromTime = 0.0f;
        e.toTime = 0.5f;
        e.curve = Curve::polyline({{0.0f, ease}, {1.0f, ease}});
        e.frames = {3, 5};
        d.setOverAge.entries = {e};
        Sim sim(d);
        sim.ticks(1 + 10);
        EXPECT_EQ(sim.ch(ChFrame), ease < 0.0f ? 0.0f : 5.0f) << "ease " << ease;
    }
}
