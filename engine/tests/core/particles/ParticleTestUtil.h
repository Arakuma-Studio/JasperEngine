// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TESTS_PARTICLETESTUTIL_H
#define TESTS_PARTICLETESTUTIL_H

// A lone emitter driven tick by tick, for the particle tests.

#include "particles/EmitterRuntime.h"
#include "particles/ParticleCompiler.h"
#include "particles/ParticleEffectCodec.h"

#include <gtest/gtest.h>

#include <memory>

namespace particletest {

    using namespace jasperengine;
    using namespace jasperengine::particles;

    inline std::shared_ptr<const CompiledEffect> compileOrFail(const ParticleEffectDocument& d, const CompileEnvironment& env = {}){
        auto effect = ParticleCompiler::compile(d, env);
        EXPECT_TRUE(effect->valid) << ParticleEffectCodec::format(effect->diagnostics);
        return effect;
    }

    // one particle at the origin born at the first tick with velocity v0, living long
    inline ParticleEffectDocument singleParticle(const Vector3& v0, int tickRate = 60){
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
        float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

        explicit Sim(const ParticleEffectDocument& d, uint32_t seed = 1, const CompileEnvironment& env = {}){
            runtime.bind(compileOrFail(d, env));
            runtime.reset(seed);
            input.seed = seed;
            input.gravity[0] = 0.0f;
            input.gravity[1] = -10.0f;
            input.gravity[2] = 0.0f;
            input.transform = transform;
            runtime.play();
        }
        void ticks(int n){ for (int i = 0; i < n; i++) runtime.step(input); }
        uint32_t count() const { return runtime.count(); }
        float ch(Channel c, uint32_t i = 0) const { return runtime.particleChannels().get(c)[i]; }
        uint32_t chu(Channel c, uint32_t i = 0) const { return runtime.particleChannels().getU(c)[i]; }
        void moveTo(float x, float y, float z){ transform[12] = x; transform[13] = y; transform[14] = z; }
    };

}

#endif // TESTS_PARTICLETESTUTIL_H
