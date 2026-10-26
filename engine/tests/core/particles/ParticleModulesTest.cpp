// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "particles/EmitterRuntime.h"
#include "particles/ParticleCompiler.h"
#include "particles/ParticleEffectCodec.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    // one particle at the origin, born at the first tick with velocity v0, living long
    ParticleEffectDocument single(const Vector3& v0, int tickRate = 60){
        ParticleEffectDocument d;
        d.tickRate = tickRate;
        d.simulationSpace = SimulationSpace::World;
        d.main.startLifetime = MinMaxCurve(100.0f);
        d.main.startSpeed = MinMaxCurve(0.0f);
        d.emission.rateOverTime = MinMaxCurve(0.0f);
        Burst b;
        b.count = MinMaxCurve(1.0f);
        d.emission.bursts = {b};
        d.shape.enabled = false;
        d.startVelocity.present = true;
        d.startVelocity.min = d.startVelocity.max = v0;
        return d;
    }

    struct Sim {
        EmitterRuntime runtime;
        EmitterInput input;
        explicit Sim(const ParticleEffectDocument& d){
            auto effect = ParticleCompiler::compile(d);
            EXPECT_TRUE(effect->valid) << ParticleEffectCodec::format(effect->diagnostics);
            runtime.bind(effect);
            input.gravity[0] = 0.0f;
            input.gravity[1] = -10.0f;
            input.gravity[2] = 0.0f;
            runtime.play();
        }
        void ticks(int n){ for (int i = 0; i < n; i++) runtime.step(input); }
        float ch(Channel c, uint32_t i = 0) const { return runtime.particleChannels().get(c)[i]; }
    };

}

// a burst particle is born at the end of the first tick; each later tick integrates g exactly
TEST(ParticleModules, GravityParabola){
    ParticleEffectDocument d = single(Vector3(3.0f, 5.0f, 0.0f));
    d.main.gravityModifier = MinMaxCurve(1.0f);
    Sim sim(d);
    sim.ticks(1);
    ASSERT_EQ(sim.runtime.count(), 1u);
    sim.ticks(60);
    const float t = 1.0f;
    EXPECT_NEAR(sim.ch(ChPosX), 3.0f * t, 1e-4f);
    EXPECT_NEAR(sim.ch(ChPosY), 5.0f * t - 0.5f * 10.0f * t * t, 1e-4f);
    EXPECT_NEAR(sim.ch(ChVelY), 5.0f - 10.0f * t, 1e-4f);
    EXPECT_NEAR(sim.ch(ChAge), t, 1e-5f);
}
