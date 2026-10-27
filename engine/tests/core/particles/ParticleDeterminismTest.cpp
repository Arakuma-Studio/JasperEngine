// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// AC3 and AC4: the same channels whatever the worker count, the chunk size or the SIMD back
// end; interpolation keeps a constant-velocity particle on its line.

#include "ParticleTestEffects.h"
#include "ParticleTestUtil.h"

#include "jobs/JobSystem.h"
#include "memory/Arena.h"
#include "particles/ParticleKernelContext.h"
#include "particles/ParticleRender.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

using namespace particletest;

namespace {

    // the reference stack with plane collisions and a death sub-emitter (and its own)
    std::shared_ptr<const CompiledEffect> parentEffect(){
        ParticleEffectDocument d = referenceStack(true, 1000.0f, 1u << 14);
        d.collision.present = true;
        d.collision.type = CollisionType::Planes;
        CollisionPlane floor;
        floor.point = Vector3(0.0f, -0.5f, 0.0f);
        d.collision.planes = {floor};
        d.collision.bounce = MinMaxCurve(0.5f);
        d.collision.sendEvents = true;
        d.subEmittersModule.present = true;
        SubEmitterEntry death;
        death.trigger = SubEmitterTrigger::Death;
        death.effect = "sparks.particles";
        death.emitCount = 2;
        death.inheritColor = true;
        death.inheritVelocity = true;
        death.inheritFactor = 0.25f;
        SubEmitterEntry hit;
        hit.trigger = SubEmitterTrigger::Collision;
        hit.effect = "sparks.particles";
        hit.emitCount = 1;
        hit.probability = 0.5f;
        d.subEmitters = {death, hit};
        return compileOrFail(d);
    }

    // a child whose own death spawns another: bound to the depth limit (3 levels)
    std::shared_ptr<const CompiledEffect> childEffect(){
        ParticleEffectDocument d;
        d.simulationSpace = SimulationSpace::World;
        d.maxParticles = 1u << 14;
        d.main.startLifetime = MinMaxCurve::twoConstants(0.2f, 0.6f);
        d.main.startSpeed = MinMaxCurve::twoConstants(1.0f, 2.0f);
        d.main.gravityModifier = MinMaxCurve(1.0f);
        d.shape.shape = ShapeType::Sphere;
        d.shape.radius = 0.1f;
        d.emission.rateOverTime = MinMaxCurve(0.0f);
        d.subEmittersModule.present = true;
        SubEmitterEntry e;
        e.trigger = SubEmitterTrigger::Death;
        e.effect = "child.particles";
        e.emitCount = 1;
        e.probability = 0.5f;
        d.subEmitters = {e};
        return compileOrFail(d);
    }

    struct Family {
        EmitterRuntime runtime;
        EmitterInput input;
        Family(jobs::JobSystem& js, uint32_t chunk){
            runtime.bind(parentEffect());
            // the parent, its children and theirs: three levels
            const auto child = childEffect();
            runtime.bindSubEmitters([&](const SubEmitterEntry&){ return child; });
            runtime.reset(4242);
            input.seed = 4242;
            input.jobs = &js;
            input.chunkSize = chunk;
            runtime.play();
        }
    };

    uint64_t simulate(int workers, uint32_t chunk, int ticks){
        jobs::Config config;
        config.workers = workers;
        jobs::JobSystem js(config);
        Family run(js, chunk);
        for (int t = 0; t < ticks; t++) run.runtime.step(run.input);
        EXPECT_GT(run.runtime.count(), 500u);
        // the depth-3 chain is bound and ran
        EXPECT_NE(run.runtime.child(0), nullptr);
        EXPECT_NE(run.runtime.child(0)->child(0), nullptr);
        EXPECT_EQ(run.runtime.child(0)->child(0)->child(0), nullptr);
        EXPECT_GT(run.runtime.child(0)->count() + run.runtime.child(0)->child(0)->count(), 0u);
        const uint64_t hash = run.runtime.stateHash();
        js.shutdown();
        return hash;
    }

}

TEST(ParticleDeterminism, WorkerCountIndependent){
    const uint64_t reference = simulate(0, 4096, 600);
    for (int workers : {0, 1, 2, 8}){
        for (uint32_t chunk : {256u, 4096u}){
            EXPECT_EQ(simulate(workers, chunk, 600), reference) << workers << " workers, chunks of " << chunk;
        }
    }
}

TEST(ParticleDeterminism, ScalarAndSimdAreBitIdentical){
    jobs::Config config;
    config.workers = 2;
    jobs::JobSystem js(config);
    setForceScalarKernels(true);
    uint64_t scalar = 0;
    {
        Family run(js, 4096);
        for (int t = 0; t < 300; t++) run.runtime.step(run.input);
        scalar = run.runtime.stateHash();
    }
    setForceScalarKernels(false);
    uint64_t native = 0;
    {
        Family run(js, 4096);
        for (int t = 0; t < 300; t++) run.runtime.step(run.input);
        native = run.runtime.stateHash();
    }
    js.shutdown();
    EXPECT_EQ(scalar, native) << "the " << nativeKernels().backend << " kernels differ from the scalar ones";
}

TEST(ParticleDeterminism, InterpolationContinuity){
    // 30 Hz ticks, 144 Hz frames: the drawn position of a constant-velocity particle is on its
    // line, one tick behind the frame
    ParticleEffectDocument d = singleParticle(Vector3(2.0f, 1.0f, -3.0f), 30);
    d.interpolate = true;
    const auto effect = compileOrFail(d);
    EmitterRuntime runtime;
    runtime.bind(effect);
    runtime.play();
    EmitterInput input;
    memory::Arena arena("InterpolationContinuity");
    const double frame = 1.0 / 144.0;
    double now = 0.0;
    float worst = 0.0f;
    int checked = 0;
    for (int f = 0; f < 600; f++){
        runtime.advance(input, frame);
        now += frame;
        if (runtime.count() == 0)
            continue;
        const PackResult pack = ParticleRenderPrep::pack(runtime, RenderView(), arena);
        ASSERT_EQ(pack.count, 1u);
        // born at the end of the first tick (1/30 s), drawn one tick late
        const double shown = now - 2.0 / 30.0;
        if (shown < 0.0)
            continue;
        const float expected[3] = {(float)(2.0 * shown), (float)(1.0 * shown), (float)(-3.0 * shown)};
        for (int c = 0; c < 3; c++)
            worst = std::max(worst, std::fabs(pack.instances[0].position[c] - expected[c]));
        checked++;
        arena.reset();
    }
    EXPECT_GT(checked, 500);
    EXPECT_LE(worst, 1e-4f);
}
