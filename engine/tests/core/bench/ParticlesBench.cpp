// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// je-bench-particles: the particle system's budgets (docs/specs/S20-particles.md R20-R26, AC10-AC12)
// on 8 workers (hardware threads - 2 on a smaller machine, as je-bench-jobs).
//
//   je-bench-particles [--json <file>] [--workers <n>] [--count-allocs]
//
// Scenarios: ref_100k (the reference stack at 100 000 live particles), ref_100k_nonoise,
// ref_100k_sorted (the same packed for an alpha view, back to front), ref_10k, trails_10k,
// collision_world_10k (World collision, Medium quality, against a floor answered by a ray
// provider), many_small_1000x100. Each reports the wall time of a simulation tick (all
// workers, and 1 thread), of the render prep of a view, scalar against SIMD kernels, the
// channel bytes per particle; then the empty scheduler's and a culled emitter's frame cost, and
// with --count-allocs the heap allocations of 600 steady-state frames. tools/perf/check_bench.py
// gates the JSON with tools/perf/bench-thresholds.json.

#include "BenchUtil.h"
#include "AllocationCounter.h"
#include "JobsTestSystem.h"

#include "Engine.h"
#include "Scene.h"
#include "component/ParticleSystemComponent.h"
#include "component/Transform.h"
#include "object/Camera.h"
#include "jobs/JobSystem.h"
#include "memory/Arena.h"
#include "particles/EmitterRuntime.h"
#include "particles/ParticleCompiler.h"
#include "particles/ParticleKernelContext.h"
#include "particles/ParticleRender.h"
#include "particles/ParticleWorld.h"
#include "subsystem/ParticleSubsystem.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    int kWorkers = 8;
    constexpr double kTick = 1.0 / 30.0;

    jobs::Config benchConfig(int workers){
        jobs::Config config;
        config.workers = workers;
        config.reserveForFrame = 2;
        config.detectWaitCycles = false;
        return config;
    }

    // Rate + Cone, Start Lifetime/Speed/Size/Color ranges, gravity, Limit Velocity (drag), Noise
    // High (1 octave), Color + Size over Lifetime, Texture Sheet 8x8 with frame blending,
    // Billboard, additive (the spec's reference stack); `rate` per second, lifetimes 1.5-2.5 s
    ParticleEffectDocument referenceStack(bool noise, float rate){
        ParticleEffectDocument d;
        d.maxParticles = 1u << 17;
        d.duration = 5.0f;
        d.looping = true;
        d.simulationSpace = SimulationSpace::World;
        d.seed = 12345;
        d.main.startLifetime = MinMaxCurve::twoConstants(1.5f, 2.5f);
        d.main.startSpeed = MinMaxCurve::twoConstants(2.0f, 4.0f);
        d.main.startSize = MinMaxCurve::twoConstants(0.05f, 0.15f);
        d.main.startColor.mode = GradientValueMode::TwoColors;
        d.main.startColor.color = Vector4(1.0f, 0.6f, 0.2f, 1.0f);
        d.main.startColor.colorMax = Vector4(1.0f, 0.9f, 0.5f, 1.0f);
        d.main.gravityModifier = MinMaxCurve(0.3f);
        d.emission.rateOverTime = MinMaxCurve(rate);
        d.shape.shape = ShapeType::Cone;
        d.shape.emitFrom = ShapeEmitFrom::Base;
        d.shape.radius = 0.5f;
        d.shape.angle = 20.0f;
        d.limitVelocity.present = true;
        d.limitVelocity.drag = MinMaxCurve(0.5f);
        d.limitVelocity.multiplyBySize = false;
        d.limitVelocity.limit = MinMaxCurve(100.0f);
        if (noise){
            d.noise.present = true;
            d.noise.strength = MinMaxCurve(1.0f);
            d.noise.frequency = 0.5f;
            d.noise.octaves = 1;
            d.noise.quality = NoiseQuality::High;
        }
        d.colorOverLifetime.present = true;
        d.colorOverLifetime.color.mode = GradientValueMode::Gradient;
        d.colorOverLifetime.color.gradient = Gradient::twoColors(Vector3(1.0f, 1.0f, 1.0f), Vector3(1.0f, 0.3f, 0.1f), 1.0f, 0.0f);
        d.sizeOverLifetime.present = true;
        d.sizeOverLifetime.size = MinMaxCurve::fromCurve(Curve::polyline({{0.0f, 0.2f}, {0.3f, 1.0f}, {1.0f, 0.5f}}));
        d.textureSheetAnimation.present = true;
        d.textureSheetAnimation.tilesX = 8;
        d.textureSheetAnimation.tilesY = 8;
        d.textureSheetAnimation.frameBlending = true;
        d.renderer.renderMode = RenderMode::Billboard;
        d.renderer.blend = ParticleBlend::Additive;
        return d;
    }

    // a floor at y = -2 for World collision (the ray provider stands in for Jolt's narrow phase)
    bool floorRay(const void*, const float from[3], const float to[3], float radius, uint32_t, CollisionHit& hit){
        const float floorY = -2.0f + radius;
        if (from[1] < floorY || to[1] >= floorY)
            return false;
        const float t = (from[1] - floorY) / (from[1] - to[1]);
        hit.position[0] = from[0] + (to[0] - from[0]) * t;
        hit.position[1] = floorY;
        hit.position[2] = from[2] + (to[2] - from[2]) * t;
        hit.normal[0] = 0.0f;
        hit.normal[1] = 1.0f;
        hit.normal[2] = 0.0f;
        return true;
    }

    const CollisionProvider& floorProvider(){
        static CollisionProvider provider = [](){
            CollisionProvider p;
            p.ray = floorRay;
            return p;
        }();
        return provider;
    }

    // Emitters stepped through a ParticleWorld, one tick per frame
    struct Scenario {
        std::vector<std::unique_ptr<EmitterRuntime>> runtimes;
        std::vector<EmitterTask> tasks;
        ParticleWorld world;
        float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

        Scenario(const ParticleEffectDocument& document, int emitters, const CollisionProvider* collision = nullptr){
            auto effect = ParticleCompiler::compile(document);
            if (!effect->valid){
                std::fprintf(stderr, "the bench effect does not compile\n");
                std::exit(1);
            }
            world.setBudget(1u << 22);
            for (int i = 0; i < emitters; i++){
                runtimes.push_back(std::make_unique<EmitterRuntime>());
                EmitterRuntime& runtime = *runtimes.back();
                runtime.bind(effect);
                runtime.reset(1000u + (uint32_t)i);
                runtime.play();
                EmitterTask task;
                task.runtime = &runtime;
                task.input.transform = transform;
                task.input.seed = 1000u + (uint32_t)i;
                task.input.collision = collision;
                tasks.push_back(task);
            }
        }

        void frame(jobs::JobSystem& jobs){
            for (EmitterTask& task : tasks){
                task.dt = kTick;
                task.input.jobs = &jobs;
            }
            world.simulate(tasks, &jobs);
        }

        uint32_t live() const{
            uint32_t n = 0;
            for (const auto& r : runtimes) n += r->count();
            return n;
        }

        // run to the steady state (every lifetime has turned over)
        void settle(jobs::JobSystem& jobs, int frames = 100){
            for (int i = 0; i < frames; i++) frame(jobs);
        }

        // median wall time of a frame (one tick), milliseconds
        double tickMs(jobs::JobSystem& jobs, int frames = 40){
            std::vector<double> samples;
            for (int i = 0; i < frames; i++){
                const auto start = bench::Clock::now();
                frame(jobs);
                samples.push_back(bench::secondsSince(start) * 1000.0);
            }
            return bench::percentile(samples, 0.5);
        }
    };

    // median wall time of packing every emitter for one view, milliseconds
    double prepMs(Scenario& s, jobs::JobSystem& jobs, bool sorted, int frames = 40){
        memory::Arena arena("je-bench-particles");
        RenderView view;
        view.position[0] = 0.0f;
        view.position[1] = 2.0f;
        view.position[2] = 12.0f;
        view.sort = sorted;
        std::vector<double> samples;
        for (int i = 0; i < frames; i++){
            arena.reset();
            const auto start = bench::Clock::now();
            uint32_t packed = 0;
            for (const auto& runtime : s.runtimes){
                packed += ParticleRenderPrep::pack(*runtime, view, arena, &jobs).count;
                if (runtime->trails().ringCount() > 0)
                    packed += ParticleRenderPrep::packTrails(*runtime, arena).strips;
            }
            samples.push_back(bench::secondsSince(start) * 1000.0);
            if (packed == 0){
                std::fprintf(stderr, "nothing was packed\n");
                std::exit(1);
            }
        }
        return bench::percentile(samples, 0.5);
    }

    ParticleEffectDocument trails(){
        ParticleEffectDocument d = referenceStack(false, 5000.0f);
        d.textureSheetAnimation.present = false;
        d.trails.present = true;
        d.trails.mode = TrailMode::PerParticle;
        d.trails.lifetime = MinMaxCurve(0.5f);
        d.trails.maxPoints = 16;
        d.trails.minVertexDistance = 0.1f;
        return d;
    }

    ParticleEffectDocument worldCollision(){
        ParticleEffectDocument d = referenceStack(false, 5000.0f);
        d.noise.present = false;
        d.main.gravityModifier = MinMaxCurve(1.0f);
        d.collision.present = true;
        d.collision.type = CollisionType::World;
        d.collision.quality = CollisionQuality::Medium;
        d.collision.bounce = MinMaxCurve(0.3f);
        return d;
    }

    ParticleEffectDocument small(){
        ParticleEffectDocument d = referenceStack(false, 50.0f);
        d.maxParticles = 256;
        d.main.startLifetime = MinMaxCurve(2.0f);
        return d;
    }

    // the reference stack with a death sub-emitter (for the allocation count)
    std::unique_ptr<EmitterRuntime> withSubEmitter(){
        ParticleEffectDocument parent = referenceStack(true, 5000.0f);
        SubEmitterEntry entry;
        entry.trigger = SubEmitterTrigger::Death;
        entry.effect = "child.particles";
        entry.emitCount = 2;
        entry.probability = 0.2f;
        parent.subEmittersModule.present = true;
        parent.subEmitters.push_back(entry);
        ParticleEffectDocument childDoc = referenceStack(false, 0.0f);
        childDoc.looping = false;
        childDoc.maxParticles = 4096;
        childDoc.main.startLifetime = MinMaxCurve(0.3f);
        auto child = ParticleCompiler::compile(childDoc);
        auto runtime = std::make_unique<EmitterRuntime>();
        runtime->bind(ParticleCompiler::compile(parent));
        runtime->bindSubEmitters([child](const SubEmitterEntry&){ return child; });
        runtime->reset(5);
        runtime->play();
        return runtime;
    }

    // ParticleSubsystem's update of a scene without particle systems, microseconds per frame
    double emptySchedulerUs(){
        Scene scene;
        auto particles = scene.getSystem<ParticleSubsystem>();
        for (int i = 0; i < 100; i++) particles->update(kTick);
        std::vector<double> samples;
        for (int i = 0; i < 2000; i++){
            const auto start = bench::Clock::now();
            particles->update(kTick);
            samples.push_back(bench::secondsSince(start) * 1.0e6);
        }
        return bench::percentile(samples, 0.5);
    }

    // the frame cost of an out-of-sight Automatic (looping) emitter: what 100 more of them add to
    // a scene's particle update, per emitter (the camera is past their Distance LOD's far end)
    double culledEmitterUs(){
        auto frameUs = [](int emitters){
            Scene scene;
            Camera camera(&scene);
            camera.setPosition(0.0f, 0.0f, 10000.0f);
            // no transform system runs here: the world position the culling reads, set directly
            scene.getComponent<Transform>(camera.getEntity()).worldPosition = Vector3(0.0f, 0.0f, 10000.0f);
            scene.setCamera(&camera);
            for (int e = 0; e < emitters; e++){
                Entity entity = scene.createEntity();
                scene.addComponent<Transform>(entity, {});
                ParticleSystemComponent component;
                component.inlineEffect = referenceStack(false, 200.0f);
                component.inlineEffect.maxParticles = 1024;
                component.inlineEffect.lod.enabled = true;
                component.inlineEffect.lod.maxDistance = 100.0f;
                scene.addComponent<ParticleSystemComponent>(entity, component);
            }
            auto particles = scene.getSystem<ParticleSubsystem>();
            for (int i = 0; i < 100; i++) particles->update(kTick);
            std::vector<double> samples;
            for (int i = 0; i < 1000; i++){
                const auto start = bench::Clock::now();
                particles->update(kTick);
                samples.push_back(bench::secondsSince(start) * 1.0e6);
            }
            return bench::percentile(samples, 0.5);
        };
        const double one = frameUs(1);
        const double many = frameUs(101);
        return std::max(0.0, (many - one) / 100.0);
    }

}

int main(int argc, char** argv){
    bool countAllocs = false;
    for (int i = 1; i < argc; i++){
        const std::string arg = argv[i];
        if (arg == "--workers" && i + 1 < argc)
            kWorkers = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--count-allocs")
            countAllocs = true;
    }
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware > 2 && (int)hardware - 2 < kWorkers)
        kWorkers = (int)hardware - 2;
    Engine::systemInit(argc, argv, new testsupport::JobsTestSystem());
    std::printf("je-bench-particles: %d workers, %u hardware threads%s\n", kWorkers, hardware,
                forceScalarKernels() ? " (scalar kernels forced)" : "");

    jobs::JobSystem pool(benchConfig(kWorkers));
    jobs::JobSystem single(benchConfig(0));
    bench::Results results("je-bench-particles");

    // ---- ref_100k: simulation (all workers, one thread, scalar), render prep, sorted prep
    {
        Scenario ref(referenceStack(true, 50000.0f), 1);
        ref.settle(pool);
        results.add("ref_100k_live", ref.live(), "particles");
        results.add("ref_100k_bytes_per_particle", (double)ref.runtimes[0]->particleChannels().bytesPerParticle(), "B");
        const double sim = ref.tickMs(pool);
        const double sim1 = ref.tickMs(single, 20);
        setForceScalarKernels(true);
        const double simScalar1 = ref.tickMs(single, 20);
        setForceScalarKernels(false);
        results.add("ref_100k_sim_ms", sim, "ms");
        results.add("ref_100k_sim_1t_ms", sim1, "ms");
        results.add("ref_100k_sim_scalar_1t_ms", simScalar1, "ms");
        results.add("ref_100k_parallel_speedup_ratio", sim1 / std::max(sim, 1e-9) / (kWorkers + 1), "of workers+1");
        results.add("ref_100k_simd_speedup", simScalar1 / std::max(sim1, 1e-9), "x");
        const double prep = prepMs(ref, pool, false);
        const double prep1 = prepMs(ref, single, false);
        results.add("ref_100k_prep_ms", prep, "ms");
        results.add("ref_100k_prep_1t_ms", prep1, "ms");
        // the 8-worker CI budgets (R21, R22) on a machine with fewer cores: the single-thread
        // time over 9 threads at R24's least parallel efficiency (0.6, gated above as measured)
        results.add("ref_100k_sim_8w_projected_ms", sim1 / (9.0 * 0.6), "ms");
        results.add("ref_100k_prep_8w_projected_ms", prep1 / (9.0 * 0.6), "ms");

        // ref_100k_sorted: the stack alpha-blended and sorted by distance, packed for a view
        // that sorts (back to front) and for one that does not
        ParticleEffectDocument sortedDoc = referenceStack(true, 50000.0f);
        sortedDoc.renderer.blend = ParticleBlend::Alpha;
        sortedDoc.renderer.sortMode = SortMode::ByDistance;
        Scenario sortedRef(sortedDoc, 1);
        sortedRef.settle(pool);
        const double unsorted1 = prepMs(sortedRef, single, false, 20);
        const double sorted1 = prepMs(sortedRef, single, true, 20);
        results.add("ref_100k_sorted_prep_1t_ms", sorted1, "ms");
        results.add("ref_100k_sort_extra_1t_ms", std::max(0.0, sorted1 - unsorted1), "ms");

        Scenario quiet(referenceStack(false, 50000.0f), 1);
        quiet.settle(pool);
        const double quietSim = quiet.tickMs(pool);
        const double quiet1 = quiet.tickMs(single, 20);
        setForceScalarKernels(true);
        const double quietScalar1 = quiet.tickMs(single, 20);
        setForceScalarKernels(false);
        results.add("ref_100k_nonoise_sim_ms", quietSim, "ms");
        results.add("ref_100k_nonoise_sim_1t_ms", quiet1, "ms");
        results.add("ref_100k_nonoise_sim_8w_projected_ms", quiet1 / (9.0 * 0.6), "ms");
        // the Noise kernel alone: what it adds to a tick, scalar against native
        results.add("noise_simd_speedup", std::max(simScalar1 - quietScalar1, 0.0) / std::max(sim1 - quiet1, 1e-9), "x");
    }

    // ---- ref_10k
    {
        Scenario ref(referenceStack(true, 5000.0f), 1);
        ref.settle(pool);
        results.add("ref_10k_sim_ms", ref.tickMs(pool), "ms");
        results.add("ref_10k_prep_ms", prepMs(ref, pool, false), "ms");
    }

    // ---- trails_10k
    {
        Scenario s(trails(), 1);
        s.settle(pool);
        results.add("trails_10k_sim_ms", s.tickMs(pool), "ms");
        results.add("trails_10k_prep_ms", prepMs(s, pool, false), "ms");
        {
            memory::Arena arena("je-bench-particles");
            const TrailPack t = ParticleRenderPrep::packTrails(*s.runtimes[0], arena);
            results.add("trails_10k_strips", t.strips, "strips");
            results.add("trails_10k_points_per_strip", t.pointsPerStrip, "points");
        }
        const ParticleTrails& t = s.runtimes[0]->trails();
        const double points = (double)t.ringCount() * (double)t.pointsPerRing();
        results.add("trails_bytes_per_point", points > 0.0 ? (double)t.memoryBytes() / points : 0.0, "B");
    }

    // ---- collision_world_10k
    {
        Scenario s(worldCollision(), 1, &floorProvider());
        s.settle(pool);
        results.add("collision_world_10k_sim_ms", s.tickMs(pool), "ms");
        uint32_t rays = 0;
        for (int i = 0; i < 30; i++){
            s.frame(pool);
            rays = std::max(rays, s.runtimes[0]->raysLastTick());
        }
        results.add("collision_world_10k_rays_per_tick", rays, "rays");
    }

    // ---- many_small_1000x100
    {
        Scenario s(small(), 1000);
        s.settle(pool);
        results.add("many_small_live", s.live(), "particles");
        results.add("many_small_1000x100_sim_ms", s.tickMs(pool), "ms");
        results.add("many_small_1000x100_prep_ms", prepMs(s, pool, false, 20), "ms");
    }

    // ---- frame costs without work
    results.add("empty_scheduler_us", emptySchedulerUs(), "us");
    results.add("culled_emitter_us", culledEmitterUs(), "us");

    // ---- R25: 600 steady-state frames of ref_100k + trails_10k + sub-emitters, no allocation
    if (countAllocs){
        if (!testsupport::countsAllocations()){
            std::fprintf(stderr, "--count-allocs: this build cannot count allocations\n");
            return 1;
        }
        Scenario ref(referenceStack(true, 50000.0f), 1);
        Scenario trail(trails(), 1);
        auto parent = withSubEmitter();
        EmitterInput parentInput;
        parentInput.jobs = &pool;
        memory::Arena arena("je-bench-particles");
        RenderView view;
        auto frame = [&](){
            ref.frame(pool);
            trail.frame(pool);
            parent->advance(parentInput, kTick);
            arena.reset();
            ParticleRenderPrep::pack(*ref.runtimes[0], view, arena, &pool);
            ParticleRenderPrep::pack(*trail.runtimes[0], view, arena, &pool);
            ParticleRenderPrep::packTrails(*trail.runtimes[0], arena);
            ParticleRenderPrep::pack(*parent, view, arena, &pool);
        };
        // to the steady state: every pool and arena block has grown to what the frames need
        for (int i = 0; i < 300; i++) frame();
        testsupport::AllocationCountScope scope;
        for (int i = 0; i < 600; i++) frame();
        results.add("steady_state_allocations", (double)scope.count(), "allocations");
    }

    const char* path = bench::jsonPath(argc, argv, "je-bench-particles.json");
    if (!results.write(path)){
        std::fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    std::printf("wrote %s\n", path);
    jobs::shutdown();
    return 0;
}
