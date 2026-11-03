// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// AC5: plane bounces, kill speeds, terrain without tunnelling, world collision's query cap and
// shared contact planes, collision events.

#include "ParticleTestUtil.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

using namespace particletest;

namespace {

    ParticleEffectDocument falling(float height, float vy, CollisionType type){
        ParticleEffectDocument d = singleParticle(Vector3(0.0f, vy, 0.0f), 60);
        d.shape.enabled = true;
        d.shape.shape = ShapeType::Sphere;
        d.shape.radius = 0.0f;
        d.shape.position = Vector3(0.0f, height, 0.0f);
        d.main.startSize = MinMaxCurve(0.0f);
        d.collision.present = true;
        d.collision.type = type;
        CollisionPlane floor;
        d.collision.planes = {floor};
        d.collision.bounce = MinMaxCurve(0.6f);
        d.collision.sendEvents = true;
        return d;
    }

    // a heightmap of 33 x 33 samples over [-16, 16]^2 (bilinear), the terrain provider
    struct Heightmap {
        float h[33][33];
        Heightmap(){
            for (int z = 0; z < 33; z++)
                for (int x = 0; x < 33; x++)
                    h[z][x] = 2.0f * std::sin(x * 0.4f) * std::cos(z * 0.3f) + 0.05f * ((x * 7 + z * 13) % 5);
        }
        float at(float x, float z) const{
            const float fx = std::clamp(x + 16.0f, 0.0f, 31.999f), fz = std::clamp(z + 16.0f, 0.0f, 31.999f);
            const int ix = (int)fx, iz = (int)fz;
            const float tx = fx - ix, tz = fz - iz;
            const float a = h[iz][ix] + (h[iz][ix + 1] - h[iz][ix]) * tx;
            const float b = h[iz + 1][ix] + (h[iz + 1][ix + 1] - h[iz + 1][ix]) * tx;
            return a + (b - a) * tz;
        }
        static bool query(const void* self, const float p[3], float surface[3], float normal[3]){
            const Heightmap& m = *static_cast<const Heightmap*>(self);
            if (std::fabs(p[0]) > 16.0f || std::fabs(p[2]) > 16.0f)
                return false;
            const float e = 0.01f;
            const float hx = (m.at(p[0] + e, p[2]) - m.at(p[0] - e, p[2])) / (2 * e);
            const float hz = (m.at(p[0], p[2] + e) - m.at(p[0], p[2] - e)) / (2 * e);
            const float len = std::sqrt(hx * hx + 1.0f + hz * hz);
            normal[0] = -hx / len; normal[1] = 1.0f / len; normal[2] = -hz / len;
            surface[0] = p[0]; surface[1] = m.at(p[0], p[2]); surface[2] = p[2];
            return true;
        }
    };

    // a box floor: the top face of [-50, 50] x [-1, 0] x [-50, 50], answered as rays
    struct BoxFloor {
        // (the world collision stage casts its rays in parallel: a provider is called from jobs)
        mutable std::atomic<uint32_t> rays{0};
        static bool ray(const void* self, const float from[3], const float to[3], float radius, uint32_t, CollisionHit& hit){
            const BoxFloor& b = *static_cast<const BoxFloor*>(self);
            b.rays++;
            const float top = radius;
            if (from[1] < top || to[1] >= top)
                return false;
            const float t = (from[1] - top) / (from[1] - to[1]);
            const float x = from[0] + (to[0] - from[0]) * t, z = from[2] + (to[2] - from[2]) * t;
            if (std::fabs(x) > 50.0f || std::fabs(z) > 50.0f)
                return false;
            hit.position[0] = x; hit.position[1] = 0.0f; hit.position[2] = z;
            hit.normal[0] = 0.0f; hit.normal[1] = 1.0f; hit.normal[2] = 0.0f;
            return true;
        }
    };

}

TEST(ParticleCollision, PlaneBounceRestitution){
    Sim sim(falling(1.0f, -2.0f, CollisionType::Planes));
    sim.ticks(1);
    float before = 0.0f;
    bool bounced = false;
    for (int t = 0; t < 120 && !bounced; t++){
        const float vy = sim.ch(ChVelY);
        sim.ticks(1);
        if (sim.ch(ChVelY) > 0.0f){
            // no gravity: the velocity that reached the plane is the one before the tick
            before = vy;
            bounced = true;
            EXPECT_NEAR(sim.ch(ChVelY), -0.6f * before, 1e-4f);
            EXPECT_NEAR(sim.ch(ChPosY), 0.0f, 1e-5f);
        }
    }
    EXPECT_TRUE(bounced);
    ASSERT_FALSE(sim.runtime.collisionEvents().empty());
    const CollisionEvent& e = sim.runtime.collisionEvents()[0];
    EXPECT_NEAR(e.position[1], 0.0f, 1e-3f);
    EXPECT_NEAR(e.normal[1], 1.0f, 1e-3f);
    EXPECT_NEAR(e.normal[0], 0.0f, 1e-3f);
}

TEST(ParticleCollision, DampenSlowsTheTangent){
    ParticleEffectDocument d = falling(0.1f, -2.0f, CollisionType::Planes);
    d.startVelocity.min = d.startVelocity.max = Vector3(3.0f, -2.0f, 0.0f);
    d.collision.dampen = MinMaxCurve(0.5f);
    d.collision.bounce = MinMaxCurve(1.0f);
    Sim sim(d);
    sim.ticks(1);
    for (int t = 0; t < 30 && sim.ch(ChVelY) < 0.0f; t++) sim.ticks(1);
    EXPECT_NEAR(sim.ch(ChVelX), 1.5f, 1e-4f);
    EXPECT_NEAR(sim.ch(ChVelY), 1.0f, 1e-4f);
}

TEST(ParticleCollision, KillSpeedsAndKillOnCollide){
    {
        // too slow after the bounce: dies
        ParticleEffectDocument d = falling(0.05f, -1.0f, CollisionType::Planes);
        d.collision.minKillSpeed = 0.7f;
        Sim sim(d);
        sim.ticks(1 + 10);
        EXPECT_EQ(sim.count(), 0u);
    }
    {
        // too fast: dies
        ParticleEffectDocument d = falling(0.05f, -10.0f, CollisionType::Planes);
        d.collision.maxKillSpeed = 5.0f;
        Sim sim(d);
        sim.ticks(1 + 10);
        EXPECT_EQ(sim.count(), 0u);
    }
    {
        // between the two: lives
        ParticleEffectDocument d = falling(0.05f, -3.0f, CollisionType::Planes);
        d.collision.minKillSpeed = 0.5f;
        d.collision.maxKillSpeed = 5.0f;
        Sim sim(d);
        sim.ticks(1 + 10);
        EXPECT_EQ(sim.count(), 1u);
    }
    {
        ParticleEffectDocument d = falling(0.05f, -3.0f, CollisionType::Planes);
        d.collision.killOnCollide = true;
        Sim sim(d);
        sim.ticks(1);
        bool killed = false;
        for (int t = 0; t < 10 && !killed; t++){
            sim.ticks(1);
            for (const CollisionEvent& e : sim.runtime.collisionEvents()) killed = killed || e.killed;
        }
        EXPECT_TRUE(killed);
        EXPECT_EQ(sim.count(), 0u);
    }
}

TEST(ParticleCollision, TerrainHasNoTunnelling){
    Heightmap map;
    CollisionProvider provider;
    provider.terrain = &Heightmap::query;
    provider.context = &map;
    ParticleEffectDocument d;
    d.tickRate = 30;
    d.maxParticles = 4000;
    d.simulationSpace = SimulationSpace::World;
    d.main.startLifetime = MinMaxCurve(4.0f);
    d.main.startSpeed = MinMaxCurve::twoConstants(5.0f, 50.0f);
    d.main.startSize = MinMaxCurve(0.0f);
    d.main.gravityModifier = MinMaxCurve(1.0f);
    d.shape.shape = ShapeType::Box;
    d.shape.scale = Vector3(28.0f, 0.0f, 28.0f);
    d.shape.position = Vector3(0.0f, 6.0f, 0.0f);
    d.shape.randomizeDirection = 1.0f;
    d.emission.rateOverTime = MinMaxCurve(0.0f);
    Burst b;
    b.count = MinMaxCurve(4000.0f);
    d.emission.bursts = {b};
    d.collision.present = true;
    d.collision.type = CollisionType::Terrain;
    d.collision.bounce = MinMaxCurve(0.3f);
    Sim sim(d);
    sim.input.collision = &provider;
    sim.ticks(1);
    uint32_t under = 0, checked = 0;
    for (int t = 0; t < 90; t++){
        sim.ticks(1);
        for (uint32_t i = 0; i < sim.count(); i++){
            const float x = sim.ch(ChPosX, i), y = sim.ch(ChPosY, i), z = sim.ch(ChPosZ, i);
            if (std::fabs(x) > 15.9f || std::fabs(z) > 15.9f)
                continue;
            checked++;
            // the speed stays at most 50 m/s (gravity adds about 4 over the test)
            if (y < map.at(x, z) - 1e-3f) under++;
        }
    }
    EXPECT_GT(checked, 10000u);
    EXPECT_EQ(under, 0u);
}

// 10 000 live particles raining on a box floor: at most 256 rays a tick (the newborns first, so
// each finds the floor at once), and no more than 0.5 % of them below it
TEST(ParticleCollision, WorldMediumCapsQueriesWithoutTunnelling){
    BoxFloor floor;
    CollisionProvider provider;
    provider.ray = &BoxFloor::ray;
    provider.context = &floor;
    ParticleEffectDocument d;
    d.tickRate = 30;
    d.maxParticles = 10000;
    d.simulationSpace = SimulationSpace::World;
    d.main.startLifetime = MinMaxCurve(6.0f);
    d.main.startSpeed = MinMaxCurve::twoConstants(0.0f, 3.0f);
    d.main.startSize = MinMaxCurve(0.1f);
    d.main.gravityModifier = MinMaxCurve(1.0f);
    d.shape.shape = ShapeType::Box;
    d.shape.scale = Vector3(40.0f, 8.0f, 40.0f);
    d.shape.position = Vector3(0.0f, 5.0f, 0.0f);
    d.shape.randomizeDirection = 1.0f;
    d.emission.rateOverTime = MinMaxCurve(10000.0f / 6.0f);
    d.collision.present = true;
    d.collision.type = CollisionType::World;
    d.collision.quality = CollisionQuality::Medium;
    d.collision.bounce = MinMaxCurve(0.2f);
    Sim sim(d);
    sim.input.collision = &provider;
    uint32_t worstQueries = 0;
    double worstShare = 0.0;
    for (int t = 0; t < 360; t++){
        floor.rays = 0;
        sim.ticks(1);
        worstQueries = std::max(worstQueries, floor.rays.load());
        if (t < 200)
            continue;
        uint32_t below = 0;
        for (uint32_t i = 0; i < sim.count(); i++){
            if (sim.ch(ChPosY, i) < -0.05f) below++;
        }
        worstShare = std::max(worstShare, (double)below / std::max(sim.count(), 1u));
    }
    EXPECT_GE(sim.count(), 9500u);
    EXPECT_LE(worstQueries, 256u);
    EXPECT_LE(worstShare, 0.005);
}
