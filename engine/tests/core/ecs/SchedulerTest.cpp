// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SystemScheduler: order-preserving wave packing, dependsOn, Exclusive, every-N-frames, the
// seven SubSystems' schedule, serial/parallel equivalence, timings, the wave trace, the
// per-system overhead and the debug access check.

#include "Scene.h"
#include "EcsTestUtil.h"
#include "jobs/JobSystem.h"
#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

using namespace jasperengine;
using jasperengine::test::disableSubSystems;
using jasperengine::test::subSystemNames;

namespace {

    SystemDescriptor descriptor(const std::string& name, std::vector<size_t> reads, std::vector<size_t> writes, uint32_t resources = 0){
        SystemDescriptor d;
        d.name = name;
        for (size_t bit : reads) d.reads.set(bit);
        for (size_t bit : writes) d.writes.set(bit);
        d.resources = resources;
        return d;
    }

    std::vector<std::vector<std::string>> waveNames(SystemScheduler& scheduler, SystemPhase phase){
        std::vector<std::vector<std::string>> result;
        for (const auto& wave : scheduler.getWaves(phase)){
            std::vector<std::string> names;
            for (const ScheduledSystem* system : wave) names.push_back(system->getName());
            result.push_back(names);
        }
        return result;
    }

    void nothing(SystemContext&){}

    struct CompA { double value = 1.0; };
    struct CompB { double value = 2.0; };
    struct CompC { uint64_t value = 3; };
    struct CompD { uint64_t value = 0; };
    struct Marker { int frame = 0; };

    class SyntheticScene : public Scene {
    public:
        SyntheticScene(){
            registerComponent<CompA>();
            registerComponent<CompB>();
            registerComponent<CompC>();
            registerComponent<CompD>();
            registerComponent<Marker>();
        }
    };

    // eight systems with overlapping access over 50k entities; the state hash after 300 frames
    uint64_t runSynthetic(bool serial){
        SyntheticScene scene;
        disableSubSystems(scene);
        scene.getScheduler().setSerial(serial);
        std::vector<Entity> entities;
        for (int i = 0; i < 50000; i++){
            const Entity entity = scene.createEntity();
            scene.addComponent<Transform>(entity);
            scene.addComponent<CompA>(entity);
            if (i % 2) scene.addComponent<CompB>(entity);
            if (i % 3) scene.addComponent<CompC>(entity);
            scene.addComponent<CompD>(entity);
            entities.push_back(entity);
        }
        const ComponentId t = scene.getComponentId<Transform>();
        const ComponentId a = scene.getComponentId<CompA>();
        const ComponentId b = scene.getComponentId<CompB>();
        const ComponentId c = scene.getComponentId<CompC>();
        const ComponentId d = scene.getComponentId<CompD>();
        const ComponentId m = scene.getComponentId<Marker>();

        scene.addSystemFunction(descriptor("IntegrateA", {t}, {a}), [](SystemContext& ctx){
            ctx.scene.view<Read<Transform>, Write<CompA>>().each([&](Entity, const Transform& tr, CompA& ca){ ca.value = ca.value * 0.5 + tr.position.x + ctx.dt; });
        });
        scene.addSystemFunction(descriptor("IntegrateB", {}, {b}), [](SystemContext& ctx){
            ctx.scene.view<Write<CompB>>().each([&](Entity e, CompB& cb){ cb.value += (double)(e % 7) * ctx.dt; });
        });
        scene.addSystemFunction(descriptor("CountC", {}, {c}), [](SystemContext&ctx){
            ctx.scene.view<Write<CompC>>().each([](Entity e, CompC& cc){ cc.value = cc.value * 31 + e; });
        });
        scene.addSystemFunction(descriptor("MixAB", {a, b}, {d}), [](SystemContext& ctx){
            ctx.scene.view<Read<CompA>, Read<CompB>, Write<CompD>>().each([](Entity, const CompA& ca, const CompB& cb, CompD& cd){
                cd.value ^= (uint64_t)(ca.value * 1000.0) + (uint64_t)(cb.value * 10.0);
            });
        });
        scene.addSystemFunction(descriptor("ReadAll", {a, c}, {}), [](SystemContext& ctx){
            uint64_t sum = 0;
            ctx.scene.view<Read<CompA>, Optional<CompC>>().each([&sum](Entity, const CompA& ca, CompC* cc){ sum += (uint64_t)ca.value + (cc ? cc->value : 0); });
            (void)sum;
        });
        scene.addSystemFunction(descriptor("Spawn", {d}, {m}), [](SystemContext& ctx){
            // structural changes go through the command buffer
            ctx.scene.view<Read<CompD>, Optional<Marker>>().each([&](Entity e, const CompD& cd, Marker* marker){
                if (cd.value % 97 == 0 && !marker) ctx.cmd.add<Marker>(ctx.scene.makeRef(e), Marker{1});
                else if (marker && cd.value % 5 == 0) ctx.cmd.remove<Marker>(ctx.scene.makeRef(e));
            });
        });
        scene.addSystemFunction(descriptor("TouchC", {m}, {c}), [](SystemContext& ctx){
            ctx.scene.view<Read<Marker>, Write<CompC>>().each([&](Entity e, const Marker&, CompC& cc){
                cc.value += 1;
                ctx.scene.markChanged<CompC>(e);
            });
        });
        scene.addSystemFunction(descriptor("Changed", {c}, {b}), [](SystemContext& ctx){
            ctx.scene.view<Read<CompC>, Write<CompB>>().changedSince(ctx.gate).each([](Entity, const CompC& cc, CompB& cb){ cb.value += (double)(cc.value % 3); });
        });

        for (int frame = 0; frame < 300; frame++){
            scene.update(0.016);
        }

        uint64_t hash = 1469598103934665603ull;
        auto mix = [&hash](uint64_t value){ hash = (hash ^ value) * 1099511628211ull; };
        for (Entity e : entities){
            mix((uint64_t)(scene.getComponent<CompA>(e).value * 1e6));
            if (CompB* cb = scene.findComponent<CompB>(e)) mix((uint64_t)(cb->value * 1e6));
            if (CompC* cc = scene.findComponent<CompC>(e)) mix(cc->value);
            mix(scene.getComponent<CompD>(e).value);
            mix(scene.findComponent<Marker>(e) ? 1 : 0);
        }
        return hash;
    }

}

TEST(SchedulerTest, WavePacking){
    SystemScheduler scheduler;
    scheduler.add(descriptor("A", {}, {1}), nothing);
    scheduler.add(descriptor("B", {2}, {}), nothing);
    scheduler.add(descriptor("C", {1}, {}), nothing);                         // reads what A writes
    scheduler.add(descriptor("D", {}, {2}), nothing);                         // writes what B reads
    scheduler.add(descriptor("E", {}, {}, SystemResource::Lua), nothing);
    scheduler.add(descriptor("F", {3}, {}, SystemResource::Lua), nothing);   // shares Lua with E
    SystemDescriptor g = descriptor("G", {4}, {});
    g.dependsOn = {"C"};
    scheduler.add(g, nothing);
    scheduler.add(descriptor("H", {1, 2}, {}, SystemResource::MainThread), nothing); // reads what A and D write

    EXPECT_EQ(waveNames(scheduler, SystemPhase::Update), (std::vector<std::vector<std::string>>{
        {"A", "B", "E"},
        {"C", "D", "F"},
        {"G", "H"}
    }));
    EXPECT_TRUE(scheduler.getWaves(SystemPhase::Draw).empty());
}

TEST(SchedulerTest, ConflictingSystemsKeepRegistrationOrder){
    SystemScheduler scheduler;
    scheduler.add(descriptor("First", {}, {5}), nothing);
    scheduler.add(descriptor("Second", {5}, {5}), nothing);
    scheduler.add(descriptor("Third", {}, {5}), nothing);
    scheduler.add(descriptor("Other", {}, {6}), nothing);
    EXPECT_EQ(waveNames(scheduler, SystemPhase::Update), (std::vector<std::vector<std::string>>{
        {"First", "Other"}, {"Second"}, {"Third"}
    }));
}

TEST(SchedulerTest, DependsOnAndExclusive){
    SystemScheduler scheduler;
    SystemDescriptor early = descriptor("Early", {}, {});
    early.dependsOn = {"Late"};     // registered before what it depends on: it moves after it
    scheduler.add(early, nothing);
    scheduler.add(descriptor("Late", {}, {}), nothing);
    scheduler.add(descriptor("Solo", {}, {}, SystemResource::MainThread | SystemResource::Exclusive), nothing);
    scheduler.add(descriptor("After", {}, {}), nothing);
    EXPECT_EQ(waveNames(scheduler, SystemPhase::Update), (std::vector<std::vector<std::string>>{
        {"Late"}, {"Early"}, {"Solo"}, {"After"}
    }));
}

TEST(SchedulerTest, EveryNFramesAccumulatesDt){
    Scene scene;
    disableSubSystems(scene);
    std::vector<double> runs;
    SystemDescriptor every3 = descriptor("Every3", {}, {});
    every3.everyNFrames = 3;
    scene.addSystemFunction(every3, [&runs](SystemContext& ctx){ runs.push_back(ctx.dt); });
    for (int frame = 0; frame < 9; frame++){
        scene.update(0.1);
    }
    ASSERT_EQ(runs.size(), 3u);
    for (double dt : runs){
        EXPECT_NEAR(dt, 0.3, 1e-9);
    }
}

// (the sixteen SubSystems: upstream's PoseAnimatorSystem, LatticeSystem, VertexAnimatorSystem, FluidSystem and TrailSystem,
// S20's ParticleSubsystem, S19's NavigationSystem, S18's SplineSystem and S21's AnimatorSystem joined the seven)
TEST(SchedulerTest, SubSystemsOneWaveEachInOrder){
    Scene scene;
    for (SystemPhase phase : {SystemPhase::FixedUpdate, SystemPhase::Update, SystemPhase::Draw}){
        const auto waves = waveNames(scene.getScheduler(), phase);
        ASSERT_EQ(waves.size(), subSystemNames().size()) << systemPhaseName(phase);
        for (size_t i = 0; i < waves.size(); i++){
            EXPECT_EQ(waves[i], std::vector<std::string>{subSystemNames()[i]}) << systemPhaseName(phase);
        }
    }
    EXPECT_TRUE(scene.getScheduler().getWaves(SystemPhase::PostUpdate).empty());
    for (const auto& system : scene.getScheduler().getSystems()){
        EXPECT_TRUE(system->isExclusive());
        EXPECT_TRUE(system->getDescriptor().resources & SystemResource::MainThread);
    }
}

// over 5 s: a Slow suite (docs/testing.md)
TEST(SchedulerTestSlow, ParallelEqualsSerial){
    // the parallel run uses S06's workers (the process job system the engine started)
    if (std::thread::hardware_concurrency() > 1){
        ASSERT_GT(SystemScheduler::workerCount(), 0u);
    }
    const uint64_t serial = runSynthetic(true);
    const uint64_t parallel = runSynthetic(false);
    EXPECT_EQ(serial, parallel);
}

namespace {

    void spinFor(std::chrono::microseconds duration){
        const auto until = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < until){}
    }

}

// A wave of independent systems runs on more than one thread; MainThread systems stay on the
// calling thread; per-system timings and the trace stay complete
TEST(SchedulerTest, WaveRunsOnWorkers){
    if (SystemScheduler::workerCount() == 0){
        GTEST_SKIP() << "no job-system workers on this machine";
    }
    Scene scene;
    disableSubSystems(scene);
    std::mutex mutex;
    std::set<std::thread::id> threads;
    std::set<std::thread::id> mainThreadRuns;
    std::atomic<int> concurrent{0};
    std::atomic<int> maxConcurrent{0};
    for (int i = 0; i < 6; i++){
        scene.addSystemFunction(descriptor("Pooled" + std::to_string(i), {}, {}), [&](SystemContext&){
            const int now = ++concurrent;
            int seen = maxConcurrent.load();
            while (now > seen && !maxConcurrent.compare_exchange_weak(seen, now)){}
            spinFor(std::chrono::microseconds(3000));
            --concurrent;
            std::lock_guard<std::mutex> lock(mutex);
            threads.insert(std::this_thread::get_id());
        });
    }
    scene.addSystemFunction(descriptor("OnMain", {}, {}, SystemResource::MainThread), [&](SystemContext&){
        spinFor(std::chrono::microseconds(1000));
        std::lock_guard<std::mutex> lock(mutex);
        mainThreadRuns.insert(std::this_thread::get_id());
    });
    // one wave for the seven (the disabled SubSystems keep their own waves)
    bool oneWave = false;
    for (const auto& names : waveNames(scene.getScheduler(), SystemPhase::Update)){
        if (!names.empty() && names[0] == "Pooled0") oneWave = names.size() == 7;
    }
    ASSERT_TRUE(oneWave);

    for (int frame = 0; frame < 5; frame++){
        scene.update(0.016);
        scene.getScheduler().runPhase(scene, SystemPhase::Draw, 0.0);
    }

    EXPECT_GT(threads.size(), 1u);
    EXPECT_GT(maxConcurrent.load(), 1);
    ASSERT_EQ(mainThreadRuns.size(), 1u);
    EXPECT_EQ(*mainThreadRuns.begin(), std::this_thread::get_id());
    for (const auto& system : scene.getScheduler().getSystems()){
        if (!system->getDescriptor().enabled) continue;
        EXPECT_EQ(system->getTiming().callCount(), 5u) << system->getName();
        EXPECT_GT(system->getTiming().lastMs(), 0.5) << system->getName();
    }
    const auto& trace = scene.getScheduler().getLastTrace();
    ASSERT_EQ(trace.size(), 7u);
    for (size_t i = 0; i < 6; i++){
        EXPECT_EQ(*trace[i].system, "Pooled" + std::to_string(i)); // the wave's order
    }
    EXPECT_EQ(*trace[6].system, "OnMain");
}

// A system that throws on a worker: the wave finishes, the exception reaches the caller and
// the next frame runs normally
TEST(SchedulerTest, WaveExceptionReachesCaller){
    if (SystemScheduler::workerCount() == 0){
        GTEST_SKIP() << "no job-system workers on this machine";
    }
    Scene scene;
    disableSubSystems(scene);
    std::atomic<int> runs{0};
    bool fail = true;
    for (int i = 0; i < 4; i++){
        scene.addSystemFunction(descriptor("S" + std::to_string(i), {}, {}), [&, i](SystemContext&){
            spinFor(std::chrono::microseconds(500));
            runs++;
            if (i == 2 && fail) throw std::runtime_error("system failed");
        });
    }
    EXPECT_THROW(scene.getScheduler().runPhase(scene, SystemPhase::Update, 0.016), std::runtime_error);
    // the systems that started finished (one not yet started may be skipped, as serially)
    const int first = runs.load();
    EXPECT_GE(first, 3);
    EXPECT_LE(first, 4);
    fail = false;
    scene.getScheduler().runPhase(scene, SystemPhase::Update, 0.016);
    EXPECT_EQ(runs.load(), first + 4);
}

TEST(SchedulerTest, Timings){
    Scene scene;
    disableSubSystems(scene);
    for (int i = 0; i < 20; i++){
        scene.addSystemFunction(descriptor("Empty" + std::to_string(i), {}, {}), nothing);
    }
    SystemDescriptor busy = descriptor("Busy", {}, {});
    busy.phase = SystemPhase::PostUpdate;
    scene.addSystemFunction(busy, [](SystemContext&){
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(200);
        while (std::chrono::steady_clock::now() < until){}
    });
    for (int frame = 0; frame < 5; frame++){
        scene.update(0.016);
        scene.getScheduler().runPhase(scene, SystemPhase::Draw, 0.0);
    }
    const ScheduledSystem* busySystem = nullptr;
    for (const auto& system : scene.getScheduler().getSystems()){
        if (system->getName() == "Busy") busySystem = system.get();
    }
    ASSERT_NE(busySystem, nullptr);
    EXPECT_EQ(busySystem->getTiming().callCount(), 5u);
    EXPECT_GE(busySystem->getTiming().lastMs(), 0.2);
    EXPECT_GE(busySystem->getTiming().maxMs(), busySystem->getTiming().averageMs());

    const auto& trace = scene.getScheduler().getLastTrace();
    ASSERT_EQ(trace.size(), 21u); // the 20 empty systems and Busy; the disabled ones do not run
    for (const WaveTraceEntry& entry : trace){
        EXPECT_LE(entry.startUs, entry.endUs);
        EXPECT_GE(entry.startUs, 0.0);
    }
    EXPECT_EQ(*trace.back().system, "Busy");
    EXPECT_EQ(trace.back().phase, SystemPhase::PostUpdate);
}

TEST(SchedulerTest, OverheadOfTwentyEmptySystems){
    Scene scene;
    disableSubSystems(scene);
    for (int i = 0; i < 20; i++){
        // each one conflicts with the previous: twenty waves, the worst case
        scene.addSystemFunction(descriptor("Empty" + std::to_string(i), {}, {7}), nothing);
    }
    for (int frame = 0; frame < 100; frame++){
        scene.update(0.016);
    }
    const int frames = 2000;
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < frames; frame++){
        scene.update(0.016);
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / frames;
    std::printf("[ecs] 20 empty systems: %.2f us/frame\n", us);
#if defined(__SANITIZE_THREAD__)
    EXPECT_LE(us, 2000.0); // TSan instruments every access (5-15x)
#elif defined(__SANITIZE_ADDRESS__) || !defined(NDEBUG)
    EXPECT_LE(us, 400.0);
#else
    EXPECT_LE(us, 40.0); // R27: at most 2 us per system
#endif
}

TEST(SchedulerTest, SingletonWaveAllocationsDoNotGrowWithSystemCount){
    auto allocationsFor = [](int count, bool exclusive){
        Scene scene;
        disableSubSystems(scene);
        for (int i = 0; i < count; i++){
            scene.addSystemFunction(descriptor("Empty" + std::to_string(i), {}, {7},
                exclusive ? static_cast<uint32_t>(SystemResource::Exclusive) : 0u), nothing);
        }
        auto frame = [&]{
            scene.getScheduler().runPhase(scene, SystemPhase::Update, 0.016);
            scene.getScheduler().runPhase(scene, SystemPhase::Draw, 0.0);
        };
        // Warm both alternating trace buffers and the cached phase plans.
        for (int i = 0; i < 4; i++) frame();
        testsupport::ThreadAllocationCount allocations;
        frame();
        return allocations.count();
    };
    for (bool exclusive : {false, true}){
        const uint64_t one = allocationsFor(1, exclusive);
        const uint64_t twenty = allocationsFor(20, exclusive);
        if (testsupport::countsAllocations()){
            EXPECT_EQ(twenty, one) << "exclusive=" << exclusive;
        }
    }
}

TEST(SchedulerTest, SingletonWaveTracesCommandsAndExceptions){
    Scene scene;
    disableSubSystems(scene);
    std::vector<int> events;
    bool fail = true;
    SystemDescriptor first = descriptor("First", {}, {}, SystemResource::Exclusive);
    first.dependsOn = {"Second"};
    scene.addSystemFunction(first, [&](SystemContext& ctx){
        events.push_back(1);
        ctx.cmd.defer([&](Scene&){ events.push_back(10); });
    });
    scene.addSystemFunction(descriptor("Second", {}, {}, SystemResource::Exclusive), [&](SystemContext& ctx){
        events.push_back(2);
        ctx.cmd.defer([&](Scene&){ events.push_back(20); });
    });
    scene.addSystemFunction(descriptor("Throw", {}, {}, SystemResource::Exclusive), [&](SystemContext&){
        if (fail) throw std::runtime_error("expected");
        events.push_back(3);
    });
    auto& scheduler = scene.getScheduler();
    EXPECT_THROW(scheduler.runPhase(scene, SystemPhase::Update, 0.016), std::runtime_error);
    EXPECT_EQ(events, (std::vector<int>{2, 20, 1, 10}));
    scheduler.runPhase(scene, SystemPhase::Draw, 0.0);
    ASSERT_EQ(scheduler.getLastTrace().size(), 2u);
    EXPECT_EQ(*scheduler.getLastTrace()[0].system, "Second");
    EXPECT_EQ(*scheduler.getLastTrace()[1].system, "First");

    fail = false;
    events.clear();
    scheduler.runPhase(scene, SystemPhase::Update, 0.016);
    scheduler.runPhase(scene, SystemPhase::Draw, 0.0);
    EXPECT_EQ(events, (std::vector<int>{2, 20, 1, 10, 3}));
    ASSERT_EQ(scheduler.getLastTrace().size(), 3u);
    EXPECT_EQ(*scheduler.getLastTrace().back().system, "Throw");
}

TEST(SchedulerTest, DebugOptionsAndReset){
    Scene scene;
    disableSubSystems(scene);
    int runs = 0;
    scene.addSystemFunction(descriptor("Counted", {}, {}), [&runs](SystemContext&){ runs++; });
    EXPECT_TRUE(scene.getScheduler().setEnabled("Counted", false));
    scene.update(0.016);
    EXPECT_EQ(runs, 0);
    EXPECT_FALSE(scene.getScheduler().setEnabled("Nope", false));
    scene.getScheduler().resetDebugOptions();
    scene.update(0.016);
    EXPECT_EQ(runs, 1);
    // the SubSystems come back too
    for (const auto& system : scene.getScheduler().getSystems()){
        EXPECT_TRUE(system->isEnabled());
    }
}

#if JASPERENGINE_ECS_ACCESS_CHECKS
TEST(SchedulerTest, AccessCheckFires){
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_DEATH({
        Scene scene;
        disableSubSystems(scene);
        SystemDescriptor d = descriptor("Sneaky", {}, {});
        d.reads.set(scene.getComponentId<Transform>());
        scene.addSystemFunction(d, [](SystemContext& ctx){
            ctx.scene.getComponentArray<MeshComponent>();
        });
        scene.update(0.016);
    }, "");
}
#endif
