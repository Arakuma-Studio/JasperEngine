// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A burst at time 0 fires again after a restart (docs/specs/S94-rendering-anim-gaps.md F4, AC4;
// the port's R27, which moved its bursts to 0.001 s): the reset re-arms the bursts of the cycle,
// for the runtime itself and for an old script's stop, reset and start on a scene's system.

#include "ParticleTestUtil.h"

#include "Scene.h"
#include "object/ParticleSystem.h"
#include "subsystem/ParticleSubsystem.h"

using namespace particletest;

namespace {

    // one burst of 7 at 0 that outlives the test, no rate
    ParticleEffectDocument burstAtZero(){
        ParticleEffectDocument d;
        d.tickRate = 60;
        d.duration = 1.0f;
        d.looping = false;
        d.simulationSpace = SimulationSpace::World;
        d.main.startLifetime = MinMaxCurve(100.0f);
        d.main.startSpeed = MinMaxCurve(0.0f);
        d.shape.enabled = false;
        d.emission.rateOverTime = MinMaxCurve(0.0f);
        Burst b;
        b.time = 0.0f;
        b.count = MinMaxCurve(7.0f);
        d.emission.bursts = {b};
        return d;
    }

}

TEST(EmitterRuntimeTest, BurstAtZeroAfterRestart){
    Sim sim(burstAtZero());
    sim.ticks(1);
    ASSERT_EQ(sim.count(), 7u) << "the first play fires the burst at its first tick";
    sim.ticks(30);
    ASSERT_EQ(sim.count(), 7u);

    sim.runtime.reset(1);
    EXPECT_EQ(sim.count(), 0u) << "the reset clears the particles";
    sim.runtime.play();
    sim.ticks(1);
    EXPECT_EQ(sim.count(), 7u) << "the burst at 0 fires again at the first tick after the restart";
}

TEST(EmitterRuntimeTest, LegacyStopResetStartFiresTheBurstAtZero){
    // KFx's order (ARCADE scripts/k/KFx.lua): stop, reset, then start on the next use
    Scene scene;
    ParticleSystem ps(&scene);
    ps.setPlayOnAwake(false);
    ASSERT_TRUE(ps.setInlineEffect(R"({"version": 1, "duration": 1, "looping": false, "maxParticles": 50,
        "modules": [{"type": "main", "startLifetime": 100, "startSpeed": 0},
                    {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 7}]},
                    {"type": "shape", "enabled": false}]})"));
    std::shared_ptr<ParticleSubsystem> particles = scene.getSystem<ParticleSubsystem>();
    auto frames = [&](int n){ for (int i = 0; i < n; i++) particles->update(1.0 / 60.0); };
    // the frames until the system's first tick has run (a frame may carry its tick to the next)
    auto firstTick = [&](){ for (int i = 0; i < 10 && ps.getTime() <= 0.0f; i++) frames(1); };

    ps.play();
    firstTick();
    ASSERT_GT(ps.getTime(), 0.0f);
    ASSERT_EQ(ps.getParticleCount(), 7u) << "the first play fires the burst";
    frames(30);
    ASSERT_EQ(ps.getParticleCount(), 7u);

    for (int round = 0; round < 2; round++){
        ps.stop();
        ps.reset();
        ps.start();
        EXPECT_EQ(ps.getParticleCount(), 0u) << "round " << round << ": the reset clears the particles";
        firstTick();
        ASSERT_GT(ps.getTime(), 0.0f);
        EXPECT_LE(ps.getTime(), 2.5f / 60.0f) << "round " << round << ": within its first ticks";
        EXPECT_EQ(ps.getParticleCount(), 7u) << "round " << round << ": the burst at 0 fires at the first tick";
        frames(30);
    }
}
