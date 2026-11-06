// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// AC7: Birth, Death, Collision and Trigger spawn the expected counts with what they inherit;
// the children are pooled (nothing allocated once warm).

#include "ParticleTestUtil.h"

#include "AllocationCounter.h"
#include "Scene.h"
#include "object/ParticleSystem.h"
#include "particles/ParticleEffectLibrary.h"
#include "particles/ParticleWorld.h"
#include "subsystem/ParticleSubsystem.h"

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

// BF23 F2: what the stats and the budget read counts the children

TEST(ParticleSubEmitters, WorldStatsCountChildrenWhenOnlyTheyAreAlive){
    Family f(parent(SubEmitterTrigger::Death, 3));
    ParticleWorld world;
    std::vector<EmitterTask> tasks(1);
    tasks[0].runtime = &f.runtime;
    tasks[0].dt = 1.0 / 30.0;
    for (int frame = 0; frame < 30; frame++) world.simulate(tasks);
    ASSERT_EQ(f.runtime.count(), 0u) << "the parents died at 0.5 s";
    ASSERT_EQ(f.kid().count(), 30u);
    EXPECT_EQ(world.stats().live, 30u);
}

TEST(ParticleSubEmitters, SystemStatsCountChildrenWhenOnlyTheyAreAlive){
    ParticleEffectDocument kid;
    kid.tickRate = 30;
    kid.main.startLifetime = MinMaxCurve(10.0f);
    kid.emission.rateOverTime = MinMaxCurve(0.0f);
    ParticleEffectLibrary::set("fx/f2-child.particles", kid);
    Scene scene;
    ParticleSystem system(&scene);
    system.setPlayOnAwake(false);
    ASSERT_TRUE(system.setInlineEffect(R"({"version": 1, "duration": 5, "looping": false, "seed": 4, "maxParticles": 100, "tickRate": 30,
        "subEmitters": [{"trigger": "death", "effect": "fx/f2-child.particles", "emitCount": 2}],
        "modules": [{"type": "main", "startLifetime": 0.2, "startSpeed": 0},
                    {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 5}]},
                    {"type": "subEmitters"}]})"));
    system.play();
    system.step(15);
    ParticleSubsystem& subsystem = *scene.getSystem<ParticleSubsystem>();
    const particles::ParticleSystemState* state = subsystem.getState(system.getEntity());
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->runtime.count(), 0u) << "the parents are dead";
    ASSERT_EQ(state->runtime.child(0)->count(), 10u);
    EXPECT_EQ(subsystem.getStats(system.getEntity()).live, 10u) << "get_particle_stats' particle_count";
    EXPECT_EQ(system.getParticleCount(), 10u) << "Lua's particleCount, and what an Action's controller waits for";
    ParticleEffectLibrary::clear();
}

TEST(ParticleSubEmitters, BudgetShareCountsChildren){
    // the first system's children hold 900 of a world budget of 1 000, and it comes first: the
    // second, which wants 500, gets what is left
    Family holder(parent(SubEmitterTrigger::Birth, 90));
    holder.ticks(1);
    ASSERT_EQ(holder.kid().count(), 900u);

    ParticleEffectDocument other;
    other.tickRate = 30;
    other.maxParticles = 500;
    other.main.startLifetime = MinMaxCurve(1.0f);
    other.emission.rateOverTime = MinMaxCurve(500.0f);
    EmitterRuntime wants;
    wants.bind(compileOrFail(other));
    wants.reset(3);
    wants.play();
    ASSERT_GE(wants.program()->capacityHint(), 500u);

    ParticleWorld world;
    world.setBudget(1000);
    std::vector<EmitterTask> tasks(2);
    tasks[0].runtime = &holder.runtime;
    tasks[1].runtime = &wants;
    tasks[0].priority = 1;
    world.shareBudget(tasks);
    EXPECT_EQ(tasks[0].budget, 910u);
    EXPECT_EQ(tasks[1].budget, 90u);
    EXPECT_LT(tasks[1].emissionScale, 1.0f);
}

TEST(ParticleSubEmitters, AChildIsNotCappedByItsParentsMaxParticles){
    // Max Particles is each emitter's own: a parent of 10 does not hold its children to 10
    ParticleEffectDocument d = parent(SubEmitterTrigger::Birth, 50);
    d.maxParticles = 10;
    Family f(d);
    ParticleWorld world;
    std::vector<EmitterTask> tasks(1);
    tasks[0].runtime = &f.runtime;
    tasks[0].dt = 1.0 / 30.0;
    world.simulate(tasks);
    ASSERT_EQ(f.runtime.count(), 10u);
    EXPECT_EQ(f.kid().count(), 500u);
}

TEST(ParticleSubEmitters, SiblingsShareTheBudget){
    // two entries that each ask for 500: a budget of 600 is shared, it is not given to each
    ParticleEffectDocument d = parent(SubEmitterTrigger::Birth, 50);
    d.subEmitters.push_back(d.subEmitters[0]);
    Family f(d);
    ParticleWorld world;
    world.setBudget(600);
    std::vector<EmitterTask> tasks(1);
    tasks[0].runtime = &f.runtime;
    tasks[0].dt = 1.0 / 30.0;
    world.simulate(tasks);
    ASSERT_EQ(f.runtime.count(), 10u);
    ASSERT_EQ(f.runtime.childCount(), 2u);
    EXPECT_EQ(f.runtime.count() + f.runtime.child(0)->count() + f.runtime.child(1)->count(), 600u);
    EXPECT_FALSE(world.stats().overBudget);
}

TEST(ParticleSubEmitters, ChildrenRenderBetweenTheSameTicksAsTheirParent){
    // BF23 F1: the interpolation factor a frame leaves on its parent is the children's, this frame
    Family f(parent(SubEmitterTrigger::Birth, 1));
    f.ticks(1);
    f.runtime.advance(f.input, 1.5 / 30.0);
    EXPECT_GT(f.runtime.interpolation(), 0.0f);
    EXPECT_LT(f.runtime.interpolation(), 1.0f);
    EXPECT_FLOAT_EQ(f.kid().interpolation(), f.runtime.interpolation());
}

TEST(ParticleSubEmitters, TotalsWalkTheTree){
    Family f(parent(SubEmitterTrigger::Birth, 3));
    f.ticks(1);
    EXPECT_EQ(f.runtime.totalCount(), 40u);
    EXPECT_EQ(f.runtime.totalCapacity(), f.runtime.capacity() + f.kid().capacity());
    EXPECT_EQ(f.runtime.totalMaxParticles(), 1024u + 4096u);
}

// Review round: a live parent follows what its children are made of

namespace {

    // a child whose fields say which version of it is bound (Max Particles is compiled in)
    void defineChild(const std::string& path, uint32_t maxParticles, bool trails = false){
        ParticleEffectDocument document;
        document.tickRate = 30;
        document.maxParticles = maxParticles;
        document.main.startLifetime = MinMaxCurve(10.0f);
        document.emission.rateOverTime = MinMaxCurve(0.0f);
        if (trails)
            document.trails.present = true;
        ParticleEffectLibrary::set(path, document);
    }

    std::string parentWith(const std::string& entries){
        return R"({"version": 1, "duration": 5, "seed": 4, "maxParticles": 100, "tickRate": 30, "subEmitters": [)" + entries + R"(],
            "modules": [{"type": "main", "startLifetime": 10, "startSpeed": 0},
                        {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 5}]},
                        {"type": "subEmitters"}]})";
    }

    std::string entry(const std::string& effect, const std::string& more = ""){
        return R"({"trigger": "birth", "effect": ")" + effect + R"(", "emitCount": 2)" + more + "}";
    }

}

TEST(ParticleSubEmitters, EditingTheListBindsTheChildrenItNames){
    defineChild("fx/list-a.particles", 111);
    defineChild("fx/list-b.particles", 222);
    Scene scene;
    ParticleSystem system(&scene);
    system.setPlayOnAwake(false);
    ASSERT_TRUE(system.setInlineEffect(parentWith(entry("fx/list-a.particles"))));
    ParticleSubsystem& subsystem = *scene.getSystem<ParticleSubsystem>();
    const particles::ParticleSystemState* state = subsystem.getState(system.getEntity());
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->runtime.childCount(), 1u);
    EXPECT_EQ(state->runtime.child(0)->program()->maxParticles, 111u);

    // another effect in the entry
    ASSERT_TRUE(system.setInlineEffect(parentWith(entry("fx/list-b.particles"))));
    state = subsystem.getState(system.getEntity());
    ASSERT_EQ(state->runtime.childCount(), 1u);
    EXPECT_EQ(state->runtime.child(0)->program()->maxParticles, 222u);

    // one more entry
    ASSERT_TRUE(system.setInlineEffect(parentWith(entry("fx/list-b.particles") + "," + entry("fx/list-a.particles"))));
    state = subsystem.getState(system.getEntity());
    ASSERT_EQ(state->runtime.childCount(), 2u);
    EXPECT_EQ(state->runtime.child(1)->program()->maxParticles, 111u);

    // one entry fewer: the slots follow the list
    ASSERT_TRUE(system.setInlineEffect(parentWith(entry("fx/list-a.particles"))));
    state = subsystem.getState(system.getEntity());
    ASSERT_EQ(state->runtime.childCount(), 1u);
    EXPECT_EQ(state->runtime.child(0)->program()->maxParticles, 111u);
    ParticleEffectLibrary::clear();
}

TEST(ParticleSubEmitters, EditingAnEntrysFiguresKeepsTheLiveParticles){
    defineChild("fx/keep.particles", 111);
    Scene scene;
    ParticleSystem system(&scene);
    system.setPlayOnAwake(false);
    ASSERT_TRUE(system.setInlineEffect(parentWith(entry("fx/keep.particles"))));
    system.play();
    system.step(2);
    ParticleSubsystem& subsystem = *scene.getSystem<ParticleSubsystem>();
    ASSERT_EQ(subsystem.getStats(system.getEntity()).live, 15u);
    // the probability is read when an entry fires: nothing to bind again
    ASSERT_TRUE(system.setInlineEffect(parentWith(entry("fx/keep.particles", R"(, "probability": 0.5)"))));
    EXPECT_EQ(subsystem.getStats(system.getEntity()).live, 15u) << "a figure of the entry does not start it over";
    ParticleEffectLibrary::clear();
}

TEST(ParticleSubEmitters, EditingAChildsFileReachesALiveParent){
    defineChild("fx/edit.particles", 111);
    Scene scene;
    ParticleSystem system(&scene);
    system.setPlayOnAwake(false);
    ASSERT_TRUE(system.setInlineEffect(parentWith(entry("fx/edit.particles"))));
    system.play();
    system.step(2);
    ParticleSubsystem& subsystem = *scene.getSystem<ParticleSubsystem>();
    const particles::ParticleSystemState* state = subsystem.getState(system.getEntity());
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->runtime.child(0)->count(), 10u);

    // a parameter: the child's particles stay, as a root's do
    ParticleEffectDocument edited = *ParticleEffectLibrary::get("fx/edit.particles").document;
    edited.main.startLifetime = MinMaxCurve(20.0f);
    ParticleEffectLibrary::set("fx/edit.particles", edited);
    state = subsystem.getState(system.getEntity());
    EXPECT_EQ(state->runtime.child(0)->effect()->document.main.startLifetime.constant, 20.0f);
    EXPECT_EQ(state->runtime.child(0)->count(), 10u) << "a parameter edit keeps the live particles";

    // another structure: it starts over with the new child
    defineChild("fx/edit.particles", 333, true);
    state = subsystem.getState(system.getEntity());
    EXPECT_EQ(state->runtime.child(0)->program()->maxParticles, 333u);
    EXPECT_TRUE(state->runtime.child(0)->program()->trails.enabled);
    ParticleEffectLibrary::clear();
}

TEST(ParticleSubEmitters, EditingAGrandchildsFileReachesALiveParent){
    defineChild("fx/leaf.particles", 111);
    ParticleEffectDocument middle;
    middle.tickRate = 30;
    middle.emission.rateOverTime = MinMaxCurve(0.0f);
    middle.subEmittersModule.present = true;
    SubEmitterEntry toLeaf;
    toLeaf.trigger = SubEmitterTrigger::Birth;
    toLeaf.effect = "fx/leaf.particles";
    middle.subEmitters = {toLeaf};
    ParticleEffectLibrary::set("fx/middle.particles", middle);
    Scene scene;
    ParticleSystem system(&scene);
    system.setPlayOnAwake(false);
    ASSERT_TRUE(system.setInlineEffect(parentWith(entry("fx/middle.particles"))));
    ParticleSubsystem& subsystem = *scene.getSystem<ParticleSubsystem>();
    const particles::ParticleSystemState* state = subsystem.getState(system.getEntity());
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->runtime.child(0)->child(0)->program()->maxParticles, 111u);
    defineChild("fx/leaf.particles", 444, true);
    state = subsystem.getState(system.getEntity());
    EXPECT_EQ(state->runtime.child(0)->child(0)->program()->maxParticles, 444u);
    ParticleEffectLibrary::clear();
}

TEST(ParticleSubEmitters, ABudgetFullOfChildrenLeavesTheParentNoRoom){
    // the tree shares one budget: children holding 90 of 100 leave its root 10
    ParticleEffectDocument d = parent(SubEmitterTrigger::Birth, 9);
    d.maxParticles = 1000;
    d.main.startLifetime = MinMaxCurve(100.0f);
    d.emission.bursts.clear();
    d.emission.rateOverTime = MinMaxCurve(300.0f);
    Family f(d);
    f.input.budget = 100;
    f.ticks(30);
    EXPECT_LE(f.runtime.totalCount(), 100u);
    EXPECT_GT(f.kid().count(), 0u);
    EXPECT_LE(f.runtime.count(), 100u - f.kid().count());
}

TEST(ParticleSubEmitters, ALaterEntrysParticlesCountAgainstAnEarlierOne){
    // both entries fire every tick: neither may spend what the other holds
    ParticleEffectDocument d = parent(SubEmitterTrigger::Birth, 5);
    d.subEmitters.push_back(d.subEmitters[0]);
    d.maxParticles = 1000;
    d.main.startLifetime = MinMaxCurve(100.0f);
    d.emission.bursts.clear();
    d.emission.rateOverTime = MinMaxCurve(300.0f);
    Family f(d);
    f.input.budget = 200;
    f.ticks(30);
    EXPECT_LE(f.runtime.totalCount(), 200u);
}
