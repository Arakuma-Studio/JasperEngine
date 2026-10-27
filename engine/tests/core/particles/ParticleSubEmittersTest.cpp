// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// AC7: Birth, Death, Collision and Trigger spawn the expected counts with what they inherit;
// the children are pooled (nothing allocated once warm).

#include "ParticleTestUtil.h"

#include "AllocationCounter.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace particletest;

namespace {

    std::shared_ptr<const CompiledEffect> child(float lifetime = 10.0f){
        ParticleEffectDocument d;
        d.tickRate = 30;
        d.simulationSpace = SimulationSpace::World;
        d.maxParticles = 4096;
        d.main.startLifetime = MinMaxCurve(lifetime);
        d.main.startSpeed = MinMaxCurve(0.0f);
        d.main.startSize = MinMaxCurve(1.0f);
        d.main.startColor = MinMaxGradient(Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        d.shape.enabled = false;
        d.emission.rateOverTime = MinMaxCurve(0.0f);
        return compileOrFail(d);
    }

    ParticleEffectDocument parent(SubEmitterTrigger trigger, int count){
        ParticleEffectDocument d;
        d.tickRate = 30;
        d.simulationSpace = SimulationSpace::World;
        d.maxParticles = 1024;
        d.main.startLifetime = MinMaxCurve(0.5f);
        d.main.startSpeed = MinMaxCurve(0.0f);
        d.main.startSize = MinMaxCurve(2.0f);
        d.main.startColor = MinMaxGradient(Vector4(1.0f, 0.5f, 0.25f, 1.0f));
        d.shape.enabled = false;
        d.startVelocity.present = true;
        d.startVelocity.min = d.startVelocity.max = Vector3(0.0f, -4.0f, 0.0f);
        d.emission.rateOverTime = MinMaxCurve(0.0f);
        Burst b;
        b.count = MinMaxCurve(10.0f);
        d.emission.bursts = {b};
        d.subEmittersModule.present = true;
        SubEmitterEntry e;
        e.trigger = trigger;
        e.effect = "child.particles";
        e.emitCount = count;
        e.inheritColor = true;
        e.inheritSize = true;
        e.inheritVelocity = true;
        e.inheritFactor = 0.5f;
        e.triggerName = "Boom";
        d.subEmitters = {e};
        return d;
    }

    struct Family {
        EmitterRuntime runtime;
        EmitterInput input;
        explicit Family(const ParticleEffectDocument& d, float childLifetime = 10.0f){
            runtime.bind(compileOrFail(d));
            const auto c = child(childLifetime);
            runtime.bindSubEmitters([&](const SubEmitterEntry&){ return c; });
            runtime.reset(77);
            input.seed = 77;
            runtime.play();
        }
        void ticks(int n){ for (int i = 0; i < n; i++) runtime.step(input); }
        EmitterRuntime& kid(){ return *runtime.child(0); }
    };

    void expectInherited(EmitterRuntime& kid, float vy){
        const ParticleChannels& ch = kid.particleChannels();
        ASSERT_GT(ch.count, 0u);
        // size 1 x parent 2 x factor 0.5; colour (1, 0.5, 0.25) x white; velocity x 0.5
        EXPECT_NEAR(ch.get(ChStartSize)[0], 1.0f, 1e-5f);
        const uint32_t c = ch.getU(ChStartColor)[0];
        EXPECT_EQ(c & 255u, 255u);
        EXPECT_NEAR((float)((c >> 8) & 255u), std::pow(std::pow(0.5f, 2.2f), 1.0f / 2.2f) * 255.0f, 1.0f);
        EXPECT_NEAR(ch.get(ChVelY)[0], vy, 1e-4f);
    }

}

TEST(ParticleSubEmitters, BirthSpawnsWithTheParent){
    Family f(parent(SubEmitterTrigger::Birth, 3));
    f.ticks(1);
    EXPECT_EQ(f.runtime.count(), 10u);
    // the children's requests are consumed in the same tick, after the parent
    EXPECT_EQ(f.kid().count(), 30u);
    expectInherited(f.kid(), -4.0f * 0.5f);
}

TEST(ParticleSubEmitters, DeathSpawnsWhereTheParentDied){
    Family f(parent(SubEmitterTrigger::Death, 2));
    f.ticks(1);
    EXPECT_EQ(f.kid().count(), 0u);
    f.ticks(15);   // the parents live 0.5 s
    EXPECT_EQ(f.runtime.count(), 0u);
    EXPECT_EQ(f.kid().count(), 20u);
    // they died 0.5 s down at -4 m/s
    EXPECT_NEAR(f.kid().particleChannels().get(ChPosY)[0], -2.0f, 0.15f);
    expectInherited(f.kid(), -4.0f * 0.5f);
}

TEST(ParticleSubEmitters, CollisionSpawnsAtTheContact){
    ParticleEffectDocument d = parent(SubEmitterTrigger::Collision, 1);
    d.main.startLifetime = MinMaxCurve(5.0f);
    d.collision.present = true;
    CollisionPlane floor;
    floor.point = Vector3(0.0f, -1.0f, 0.0f);
    d.collision.planes = {floor};
    d.collision.killOnCollide = true;
    Family f(d);
    int t = 0;
    while (f.kid().count() == 0 && t++ < 30) f.ticks(1);
    // the tick they hit: the parents die, their children start where they stopped (the
    // contact on the plane, a radius above it)
    EXPECT_EQ(f.runtime.count(), 0u);
    EXPECT_EQ(f.kid().count(), 10u);
    EXPECT_NEAR(f.kid().particleChannels().get(ChPosY)[0], -1.0f + 1.0f, 1e-3f);
}

TEST(ParticleSubEmitters, TriggerSpawnsFromEveryParticle){
    ParticleEffectDocument d = parent(SubEmitterTrigger::Trigger, 4);
    d.main.startLifetime = MinMaxCurve(5.0f);
    Family f(d);
    f.ticks(2);
    EXPECT_EQ(f.kid().count(), 0u);
    f.runtime.sendTrigger(nameHash("Other"));
    f.ticks(1);
    EXPECT_EQ(f.kid().count(), 0u);
    f.runtime.sendTrigger(nameHash("Boom"));
    f.ticks(1);
    EXPECT_EQ(f.kid().count(), 40u);
}

TEST(ParticleSubEmitters, AFrameKeepsTheEventsOfAllItsTicks){
    // a frame of two ticks (30 Hz, 1/15 s): the trigger's sub-emits of its first tick are still
    // there when the frame is over (Lua reads them then); they once lasted one tick
    ParticleEffectDocument d = parent(SubEmitterTrigger::Trigger, 4);
    d.main.startLifetime = MinMaxCurve(5.0f);
    Family f(d);
    f.ticks(2);
    f.runtime.sendTrigger(nameHash("Boom"));
    f.runtime.advance(f.input, 2.0 / 30.0);
    EXPECT_EQ(f.kid().count(), 40u);
    EXPECT_EQ(f.runtime.subEmitEvents().size(), 10u);
    // the next frame starts them over
    f.runtime.advance(f.input, 1.0 / 30.0);
    EXPECT_EQ(f.runtime.subEmitEvents().size(), 0u);
}

TEST(ParticleSubEmitters, ProbabilityThinsTheSpawns){
    ParticleEffectDocument d = parent(SubEmitterTrigger::Birth, 1);
    d.emission.bursts[0].count = MinMaxCurve(1000.0f);
    d.subEmitters[0].probability = 0.3f;
    Family f(d);
    f.ticks(1);
    EXPECT_NEAR((float)f.kid().count(), 300.0f, 3.0f * std::sqrt(1000.0f * 0.3f * 0.7f));
}

TEST(ParticleSubEmitters, PooledChildrenAllocateNothingWhenWarm){
    if (!testsupport::countsAllocations())
        GTEST_SKIP() << "no allocation counting on this platform";
    ParticleEffectDocument d = parent(SubEmitterTrigger::Death, 2);
    d.looping = true;
    d.emission.bursts.clear();
    d.emission.rateOverTime = MinMaxCurve(200.0f);
    Family f(d, 0.5f);
    // warm up: storage, the children's and the rings grow to their peak
    f.ticks(120);
    testsupport::ThreadAllocationCount allocations;
    f.ticks(120);
    EXPECT_EQ(allocations.count(), 0u);
    EXPECT_GT(f.kid().count(), 0u);
}
