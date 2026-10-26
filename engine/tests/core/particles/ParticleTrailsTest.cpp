// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// AC6: the trail store's spacing rule, ring wrap, die-with-particle against fading, ribbon order.

#include "ParticleTestUtil.h"

#include "memory/Arena.h"
#include "particles/ParticleRender.h"
#include "particles/ParticleTrails.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace particletest;

TEST(ParticleTrails, SpacingRuleMovesTheHead){
    ParticleTrails trails;
    trails.configure(1, 8);
    const uint32_t ring = trails.acquire();
    ASSERT_NE(ring, kNoTrail);
    trails.setLifetime(ring, 100.0f);
    trails.record(ring, 0, 0, 0, 0.0f, 1.0f);
    trails.record(ring, 0.5f, 0, 0, 0.1f, 1.0f);
    EXPECT_EQ(trails.live(ring), 2u);
    // within 1 of the point before the head: the head follows
    trails.record(ring, 0.8f, 0, 0, 0.2f, 1.0f);
    EXPECT_EQ(trails.live(ring), 2u);
    float x, y, z, t;
    trails.point(ring, 0, x, y, z, t);
    EXPECT_FLOAT_EQ(x, 0.8f);
    EXPECT_FLOAT_EQ(t, 0.2f);
    // past it: a new head
    trails.record(ring, 1.5f, 0, 0, 0.3f, 1.0f);
    EXPECT_EQ(trails.live(ring), 3u);
    trails.point(ring, 1, x, y, z, t);
    EXPECT_FLOAT_EQ(x, 0.8f);
}

TEST(ParticleTrails, RingWrapsAtItsCapacity){
    ParticleTrails trails;
    trails.configure(1, 4);
    const uint32_t ring = trails.acquire();
    trails.setLifetime(ring, 100.0f);
    for (int i = 0; i < 10; i++)
        trails.record(ring, (float)i * 2.0f, 0, 0, (float)i, 1.0f);
    EXPECT_EQ(trails.live(ring), 4u);
    // newest first: 18, 16, 14, 12
    for (uint32_t k = 0; k < 4; k++){
        float x, y, z, t;
        trails.point(ring, k, x, y, z, t);
        EXPECT_FLOAT_EQ(x, 18.0f - 2.0f * k);
    }
}

TEST(ParticleTrails, ExpireDropsTheOldPoints){
    ParticleTrails trails;
    trails.configure(1, 8);
    const uint32_t ring = trails.acquire();
    trails.setLifetime(ring, 0.5f);
    for (int i = 0; i < 5; i++)
        trails.record(ring, (float)i * 2.0f, 0, 0, 0.2f * i, 1.0f);
    trails.expire(0.9f);
    // points younger than 0.5 s at 0.9: born at 0.4, 0.6, 0.8
    EXPECT_EQ(trails.live(ring), 3u);
}

TEST(ParticleTrails, DieWithParticleOrFade){
    for (bool die : {true, false}){
        ParticleEffectDocument d = singleParticle(Vector3(3.0f, 0.0f, 0.0f), 30);
        d.main.startLifetime = MinMaxCurve(0.5f);
        d.trails.present = true;
        d.trails.lifetime = MinMaxCurve(0.5f);
        d.trails.minVertexDistance = 0.05f;
        d.trails.dieWithParticles = die;
        Sim sim(d);
        sim.ticks(1 + 10);
        EXPECT_EQ(sim.runtime.trails().usedRings(), 1u);
        // the particle dies at 0.5 s
        sim.ticks(6);
        EXPECT_EQ(sim.count(), 0u);
        if (die){
            EXPECT_EQ(sim.runtime.trails().usedRings(), 0u);
            EXPECT_FALSE(sim.runtime.isAlive());
        }else{
            // the orphaned trail fades out over its lifetime, then the system ends
            EXPECT_EQ(sim.runtime.trails().usedRings(), 1u);
            EXPECT_TRUE(sim.runtime.isAlive());
            sim.ticks(20);
            EXPECT_EQ(sim.runtime.trails().usedRings(), 0u);
            EXPECT_FALSE(sim.runtime.isAlive());
        }
    }
}

TEST(ParticleTrails, RatioPicksTheTrailedParticles){
    ParticleEffectDocument d;
    d.tickRate = 30;
    d.maxParticles = 1000;
    d.main.startLifetime = MinMaxCurve(10.0f);
    d.emission.rateOverTime = MinMaxCurve(0.0f);
    Burst b;
    b.count = MinMaxCurve(1000.0f);
    d.emission.bursts = {b};
    d.trails.present = true;
    d.trails.ratio = 0.25f;
    Sim sim(d);
    sim.ticks(1);
    const uint32_t trailed = sim.runtime.trails().usedRings();
    EXPECT_NEAR((float)trailed, 250.0f, 3.0f * std::sqrt(1000.0f * 0.25f * 0.75f));
}

TEST(ParticleTrails, PackedStripsFollowTheParticle){
    ParticleEffectDocument d = singleParticle(Vector3(3.0f, 0.0f, 0.0f), 30);
    d.trails.present = true;
    d.trails.lifetime = MinMaxCurve(10.0f);
    d.trails.minVertexDistance = 0.0f;
    d.trails.maxPoints = 8;
    d.interpolate = false;
    Sim sim(d);
    sim.ticks(1 + 5);
    memory::Arena arena("ParticleTrailsTest");
    const TrailPack pack = ParticleRenderPrep::packTrails(sim.runtime, arena);
    ASSERT_EQ(pack.strips, 1u);
    EXPECT_EQ(pack.pointsPerStrip, 9u);
    // the head is the particle; the points go back along its path, two vertices each
    EXPECT_NEAR(pack.vertices[0].center[0], sim.ch(ChPosX), 1e-5f);
    EXPECT_NEAR(pack.vertices[2].center[0], sim.ch(ChPosX), 1e-5f);
    EXPECT_NEAR(pack.vertices[4].center[0], sim.ch(ChPosX) - 3.0f / 30.0f, 1e-5f);
    // the unused points repeat the last one (no area)
    const TrailVertex& last = pack.vertices[(pack.pointsPerStrip - 1) * 2];
    const TrailVertex& lastUsed = pack.vertices[(sim.runtime.trails().live(0)) * 2];
    EXPECT_FLOAT_EQ(last.center[0], lastUsed.center[0]);
    EXPECT_EQ(pack.vertices[0].uvSide[1], 0u);
    EXPECT_EQ(pack.vertices[1].uvSide[1], 65535u);
}

TEST(ParticleTrails, RibbonsConnectParticlesInSpawnOrder){
    ParticleEffectDocument d;
    d.tickRate = 30;
    d.simulationSpace = SimulationSpace::World;
    d.main.startLifetime = MinMaxCurve(100.0f);
    d.main.startSpeed = MinMaxCurve(0.0f);
    d.shape.enabled = false;
    d.emission.rateOverTime = MinMaxCurve(30.0f);
    d.trails.present = true;
    d.trails.mode = TrailMode::Ribbon;
    d.trails.ribbonCount = 2;
    d.interpolate = false;
    Sim sim(d);
    // the emitter walks along X: particle k sits at x = k / 10
    for (int t = 0; t < 20; t++){
        sim.moveTo(0.1f * t, 0.0f, 0.0f);
        sim.ticks(1);
    }
    ASSERT_EQ(sim.count(), 20u);
    memory::Arena arena("ParticleTrailsTest");
    const TrailPack pack = ParticleRenderPrep::packTrails(sim.runtime, arena);
    ASSERT_EQ(pack.strips, 2u);
    EXPECT_EQ(pack.pointsPerStrip, 10u);
    for (uint32_t r = 0; r < 2; r++){
        // newest first, every other particle (spawn index modulo 2)
        float previous = 1e9f;
        for (uint32_t k = 0; k < pack.pointsPerStrip; k++){
            const float x = pack.vertices[(r * pack.pointsPerStrip + k) * 2].center[0];
            EXPECT_LT(x, previous) << "ribbon " << r << " point " << k;
            previous = x;
        }
    }
}
