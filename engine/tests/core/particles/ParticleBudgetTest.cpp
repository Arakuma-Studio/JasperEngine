// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// AC10: the world budget, distance thinning, culling.

#include "ParticleTestUtil.h"

#include "memory/Arena.h"
#include "particles/ParticleRender.h"
#include "particles/ParticleWorld.h"

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <vector>

using namespace particletest;

TEST(ParticleBudget, ManyEmittersConvergeUnderTheBudget){
    ParticleEffectDocument d;
    d.tickRate = 30;
    d.maxParticles = 4096;
    d.main.startLifetime = MinMaxCurve(2.0f);
    d.emission.rateOverTime = MinMaxCurve(1000.0f);
    const auto effect = compileOrFail(d);
    std::vector<std::unique_ptr<EmitterRuntime>> runtimes;
    std::vector<EmitterTask> tasks;
    for (int i = 0; i < 100; i++){
        runtimes.push_back(std::make_unique<EmitterRuntime>());
        runtimes.back()->bind(effect);
        runtimes.back()->reset((uint32_t)i + 1);
        runtimes.back()->play();
        EmitterTask t;
        t.runtime = runtimes.back().get();
        t.input.seed = (uint32_t)i + 1;
        // half of them matter more; within a priority, the first ones are bigger on screen
        t.priority = i < 50 ? 1 : 0;
        t.significance = 1.0f - (float)(i % 50) / 100.0f;
        t.dt = 1.0 / 30.0;
        tasks.push_back(t);
    }
    ParticleWorld world;
    world.setBudget(50000);
    for (int frame = 0; frame < 150; frame++){
        world.simulate(tasks);
        uint32_t live = 0;
        for (auto& r : runtimes) live += r->count();
        ASSERT_LE(live, 50000u) << "frame " << frame;
    }
    uint32_t high = 0, low = 0;
    for (int i = 0; i < 100; i++) (i < 50 ? high : low) += runtimes[i]->count();
    // the higher priority takes the budget (it wants 100 000)
    EXPECT_GT(high, 45000u);
    EXPECT_LT(low, 1000u);
    // within the priority, by significance
    EXPECT_GT(runtimes[0]->count(), runtimes[49]->count());
    EXPECT_GE(world.stats().live, 45000u);
}

TEST(ParticleBudget, UnderTheBudgetNothingIsHeldBack){
    ParticleEffectDocument d;
    d.main.startLifetime = MinMaxCurve(1.0f);
    d.emission.rateOverTime = MinMaxCurve(300.0f);
    const auto effect = compileOrFail(d);
    EmitterRuntime runtime;
    runtime.bind(effect);
    runtime.play();
    std::vector<EmitterTask> tasks(1);
    tasks[0].runtime = &runtime;
    tasks[0].dt = 1.0 / 30.0;
    ParticleWorld world;
    world.setBudget(1000);
    for (int f = 0; f < 60; f++) world.simulate(tasks);
    EXPECT_NEAR((float)runtime.count(), 300.0f, 15.0f);
    EXPECT_FLOAT_EQ(tasks[0].emissionScale, 1.0f);
}

TEST(ParticleBudget, ThinningIsStable){
    ParticleEffectDocument d;
    d.main.startLifetime = MinMaxCurve(100.0f);
    d.emission.rateOverTime = MinMaxCurve(0.0f);
    Burst b;
    b.count = MinMaxCurve(2000.0f);
    d.emission.bursts = {b};
    d.maxParticles = 2000;
    Sim sim(d);
    sim.ticks(1);
    memory::Arena arena("ParticleBudgetTest");
    RenderView view;
    view.keep = 0.4f;
    auto kept = [&](){
        const PackResult pack = ParticleRenderPrep::pack(sim.runtime, view, arena);
        std::set<std::pair<float, float>> positions;
        for (uint32_t i = 0; i < pack.count; i++)
            positions.insert({pack.instances[i].position[0], pack.instances[i].position[2]});
        return std::make_pair(pack.count, positions);
    };
    const auto first = kept();
    EXPECT_NEAR((float)first.first, 800.0f, 3.0f * std::sqrt(2000.0f * 0.4f * 0.6f));
    sim.ticks(10);
    // the same particles across frames (they moved: compare who is drawn by count and a second pass)
    const auto second = kept();
    EXPECT_EQ(second.first, first.first);
    const auto third = kept();
    EXPECT_EQ(second.second, third.second);
    // a smaller share keeps a subset of a larger one's particles
    view.keep = 0.2f;
    const auto fewer = kept();
    for (const auto& p : fewer.second) EXPECT_TRUE(second.second.count(p)) << "thinning is not nested";
}

TEST(ParticleBudget, CullingModes){
    CullState state;
    uint32_t maxTicks = 0;
    // Always Simulate: every frame
    EXPECT_DOUBLE_EQ(cullingStep(CullingMode::AlwaysSimulate, true, false, 0.1, 30, state, maxTicks), 0.1);
    // Pause: nothing off screen, nothing made up
    EXPECT_DOUBLE_EQ(cullingStep(CullingMode::Pause, true, false, 0.1, 30, state, maxTicks), 0.0);
    EXPECT_DOUBLE_EQ(cullingStep(CullingMode::Pause, true, true, 0.1, 30, state, maxTicks), 0.1);
    // Pause and Catch-up: the missed time on view, at most 0.5 s, with the ticks for it
    state = CullState();
    for (int i = 0; i < 10; i++)
        EXPECT_DOUBLE_EQ(cullingStep(CullingMode::PauseAndCatchUp, true, false, 0.1, 30, state, maxTicks), 0.0);
    EXPECT_DOUBLE_EQ(cullingStep(CullingMode::PauseAndCatchUp, true, true, 0.1, 30, state, maxTicks), 0.1 + 0.5);
    EXPECT_EQ(maxTicks, kMaxTicksPerFrame + 15u);
    EXPECT_DOUBLE_EQ(cullingStep(CullingMode::PauseAndCatchUp, true, true, 0.1, 30, state, maxTicks), 0.1);
    // Automatic: a looping effect pauses, a one-shot keeps simulating
    state = CullState();
    EXPECT_DOUBLE_EQ(cullingStep(CullingMode::Automatic, true, false, 0.1, 30, state, maxTicks), 0.0);
    EXPECT_DOUBLE_EQ(cullingStep(CullingMode::Automatic, false, false, 0.1, 30, state, maxTicks), 0.1);
}

TEST(ParticleBudget, CatchUpRunsTheMissedTicks){
    ParticleEffectDocument d;
    d.tickRate = 30;
    d.main.startLifetime = MinMaxCurve(10.0f);
    d.emission.rateOverTime = MinMaxCurve(30.0f);
    const auto effect = compileOrFail(d);
    EmitterRuntime runtime;
    runtime.bind(effect);
    runtime.play();
    EmitterInput input;
    CullState state;
    uint32_t maxTicks = 0;
    for (int f = 0; f < 30; f++){
        const double dt = cullingStep(CullingMode::PauseAndCatchUp, true, false, 1.0 / 30.0, 30, state, maxTicks);
        runtime.planFrame(input, dt, maxTicks);
    }
    EXPECT_EQ(runtime.tickCount(), 0u);
    const double dt = cullingStep(CullingMode::PauseAndCatchUp, true, true, 1.0 / 30.0, 30, state, maxTicks);
    const uint32_t ticks = runtime.planFrame(input, dt, maxTicks);
    for (uint32_t k = 0; k < ticks; k++){
        runtime.beginTick(input, k);
        for (uint32_t c = 0; c < runtime.chunkCount(); c++) runtime.updateChunk(c);
        runtime.finishTick(input);
    }
    runtime.endFrame();
    // the frame's tick and 0.5 s of the missed second
    EXPECT_EQ(ticks, 16u);
}

TEST(ParticleBudget, TheProjectBudgetIsForWorldsWithoutTheirOwn){
    ParticleWorld follows;
    ParticleWorld own;
    own.setBudget(5000);
    EXPECT_EQ(follows.budget(), defaultWorldBudget());
    ParticleWorld::setProjectBudget(1000);
    EXPECT_EQ(follows.budget(), 1000u) << "a world already made follows it";
    EXPECT_EQ(own.budget(), 5000u);
    ParticleWorld::setProjectBudget(0);
    EXPECT_EQ(follows.budget(), defaultWorldBudget());
}

namespace {

    // `parents` particles burst at once, each spawning `emitCount` children when it dies
    struct Explosion {
        EmitterRuntime runtime;
        Explosion(uint32_t parents, int emitCount, uint32_t childMax, uint32_t seed){
            ParticleEffectDocument kid;
            kid.tickRate = 30;
            kid.maxParticles = childMax;
            kid.main.startLifetime = MinMaxCurve(100.0f);
            kid.emission.rateOverTime = MinMaxCurve(0.0f);
            const auto child = compileOrFail(kid);

            ParticleEffectDocument d;
            d.tickRate = 30;
            d.maxParticles = parents;
            d.looping = false;
            d.main.startLifetime = MinMaxCurve(0.2f);
            d.emission.rateOverTime = MinMaxCurve(0.0f);
            Burst burst;
            burst.count = MinMaxCurve((float)parents);
            d.emission.bursts = {burst};
            d.subEmittersModule.present = true;
            SubEmitterEntry entry;
            entry.trigger = SubEmitterTrigger::Death;
            entry.effect = "child.particles";
            entry.emitCount = emitCount;
            d.subEmitters = {entry};
            runtime.bind(compileOrFail(d));
            runtime.bindSubEmitters([&](const SubEmitterEntry&){ return child; });
            runtime.reset(seed);
            runtime.play();
        }
    };

}

TEST(ParticleBudget, ThePriorityGetsWhatItsChildrenWillNeed){
    // a high-priority explosion about to spawn 1 600 children, against a system that wants it all:
    // its share covers them before they exist
    Explosion explosion(16, 100, 2000, 1);
    ParticleEffectDocument other;
    other.tickRate = 30;
    other.maxParticles = 200000;
    other.main.startLifetime = MinMaxCurve(2.0f);
    other.emission.rateOverTime = MinMaxCurve(50000.0f);
    EmitterRuntime wants;
    wants.bind(compileOrFail(other));
    wants.reset(2);
    wants.play();

    ParticleWorld world;
    world.setBudget(65536);
    std::vector<EmitterTask> tasks(2);
    tasks[0].runtime = &explosion.runtime;
    tasks[0].priority = 10;
    tasks[1].runtime = &wants;
    world.shareBudget(tasks);
    EXPECT_EQ(tasks[0].budget, 1616u);
    EXPECT_EQ(tasks[0].emissionScale, 1.0f);
    EXPECT_EQ(tasks[1].budget, 65536u - 1616u);
}

TEST(ParticleBudget, ManyExplosionsStayUnderTheBudgetWhenTheyGoOff){
    // each would spawn 16 000 children at once: 8 of them on a budget of 65 536
    std::vector<std::unique_ptr<Explosion>> explosions;
    std::vector<EmitterTask> tasks;
    for (uint32_t i = 0; i < 8; i++){
        explosions.push_back(std::make_unique<Explosion>(16, 1000, 20000, i + 1));
        EmitterTask t;
        t.runtime = &explosions.back()->runtime;
        t.dt = 1.0 / 30.0;
        tasks.push_back(t);
    }
    ParticleWorld world;
    world.setBudget(65536);
    for (int frame = 0; frame < 30; frame++){
        world.simulate(tasks);
        ASSERT_LE(world.stats().live, 65536u) << "frame " << frame;
    }
    EXPECT_GT(world.stats().live, 0u);
}

TEST(ParticleBudget, OneSystemOverTheBudgetIsThinnedEvenly){
    // no sub-emitters: wanting more than the world has scales its emission down, as it always did,
    // instead of spawning at the full rate and hitting the cap
    ParticleEffectDocument d;
    d.tickRate = 30;
    d.maxParticles = 200000;
    d.main.startLifetime = MinMaxCurve(1.0f);
    d.emission.rateOverTime = MinMaxCurve(100000.0f);
    EmitterRuntime runtime;
    runtime.bind(compileOrFail(d));
    runtime.reset(1);
    runtime.play();
    ParticleWorld world;
    world.setBudget(65536);
    std::vector<EmitterTask> tasks(1);
    tasks[0].runtime = &runtime;
    world.shareBudget(tasks);
    EXPECT_LT(tasks[0].emissionScale, 0.9f);
    EXPECT_GT(tasks[0].emissionScale, 0.4f);
    EXPECT_EQ(tasks[0].budget, 65536u);
}
