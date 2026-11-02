// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S20's test project: a particle effect with the reference stack's heavy modules (curl noise
// High, colour and size over lifetime, a sprite-less soft disc, additive) and a ribbon trail,
// seeked to 2 s and held, so the web build runs the particle kernels and the particles shader in
// WebGL2 (WEB_PROJECT=particles tools/build.sh web, GATE=particles tools/build.sh web-gate; with
// WASM_SIMD=0 both build the scalar kernels). The gate checks the live count and the state hash,
// which the simd128 and the scalar kernels must give alike (bit-identical), and the canvas centre.
// On Android (ANDROID_PROJECT=particles tools/build.sh android) the emulator test checks the same
// lines (android-expect.txt) with the SSE2 or NEON kernels and the GLES3 particles shader.
//
// With ?particles_bench=1 (Release builds on the benchmark server, docs/perf/particles.md) it
// first measures the web rows of R21 and R22 on one thread: the reference stack at 30 000 live
// particles with and without Noise and its render prep. The simd128 build gates them (a budget
// missed is an error line; GATE_ARGS='--query=particles_bench=1
// --expect=particles_web_budgets=pass'); tools/perf/check_web_particles.py compares its figures
// with the scalar build's for R24 (in one build, the scalar kernels are auto-vectorized for
// simd128 and would not measure what a browser without simd128 runs).

#include "JasperEngine.h"
#include "math/noise/CurlNoiseBatch.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/ParticleSystemState.h"
#include "jobs/JobSystem.h"
#include "memory/Arena.h"
#include "particles/EmitterRuntime.h"
#include "particles/ParticleCompiler.h"
#include "particles/ParticleRender.h"
#include "particles/ParticleWorld.h"

#include "../../../engine/tests/core/bench/BenchUtil.h"
#include "../../../engine/tests/core/bench/ParticleReferenceStack.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(int, particlesBenchRequested, (), {
    return new URLSearchParams(globalThis.location ? globalThis.location.search : "").get("particles_bench") === "1" ? 1 : 0;
});
#endif

using namespace jasperengine;

Scene scene;
Camera sceneCamera(&scene);
ParticleSystem fire(&scene);
ParticleSystem ribbon(&scene);

static const char* kFire = R"({"version": 1, "duration": 3, "looping": true, "seed": 21, "maxParticles": 4000, "simulationSpace": "world",
  "modules": [
    {"type": "main", "startLifetime": {"mode": "twoConstants", "constant": 1.5, "constantMax": 2.5}, "startSpeed": {"mode": "twoConstants", "constant": 1, "constantMax": 2},
     "startSize": {"mode": "twoConstants", "constant": 0.2, "constantMax": 0.4}, "startColor": [1, 0.55, 0.2, 1], "gravityModifier": -0.1},
    {"type": "emission", "rateOverTime": 800},
    {"type": "shape", "shape": "cone", "angle": 15, "radius": 0.4},
    {"type": "noise", "strength": 0.8, "frequency": 0.7, "quality": "high"},
    {"type": "colorOverLifetime", "color": {"mode": "gradient", "gradient": {"colorKeys": [[0, 1, 1, 1], [1, 1, 0.3, 0.1]], "alphaKeys": [[0, 1], [1, 0]]}}},
    {"type": "sizeOverLifetime", "size": {"mode": "curve", "curve": [[0, 0.3, 0, 0], [0.3, 1, 0, 0], [1, 0.5, 0, 0]]}},
    {"type": "renderer", "renderMode": "billboard", "blend": "additive"}
  ]})";

static const char* kRibbon = R"({"version": 1, "duration": 4, "looping": true, "seed": 5, "maxParticles": 200, "simulationSpace": "world",
  "modules": [
    {"type": "main", "startLifetime": 2, "startSpeed": 2, "startSize": 0.2, "startColor": [0.4, 0.7, 1, 1]},
    {"type": "emission", "rateOverTime": 30},
    {"type": "shape", "shape": "circle", "radius": 0.5},
    {"type": "trails", "mode": "ribbon", "ribbonCount": 1, "lifetime": 1},
    {"type": "renderer", "renderMode": "none", "blend": "additive"}
  ]})";

static uint64_t hashOf(ParticleSystem& system){
    particles::ParticleSystemState* state = scene.getSystem<ParticleSubsystem>()->getState(system.getEntity());
    return state ? state->runtime.stateHash() : 0;
}

namespace webbench {

    using namespace jasperengine::particles;

    constexpr double kTick = 1.0 / 30.0;

    // one emitter stepped through a ParticleWorld on the calling thread, one tick per frame
    struct Emitter {
        std::unique_ptr<EmitterRuntime> runtime = std::make_unique<EmitterRuntime>();
        std::vector<EmitterTask> tasks;
        ParticleWorld world;
        float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

        explicit Emitter(const ParticleEffectDocument& document){
            runtime->bind(ParticleCompiler::compile(document));
            runtime->reset(1000u);
            runtime->play();
            world.setBudget(1u << 22);
            EmitterTask task;
            task.runtime = runtime.get();
            task.input.transform = transform;
            task.input.seed = 1000u;
            tasks.push_back(task);
            for (int i = 0; i < 100; i++) frame();    // every lifetime has turned over
        }

        void frame(){
            tasks[0].dt = kTick;
            world.simulate(tasks, &jobs::system());
        }

        // median wall time of a tick, milliseconds
        double tickMs(int frames){
            std::vector<double> samples;
            for (int i = 0; i < frames; i++){
                const auto start = bench::Clock::now();
                frame();
                samples.push_back(bench::secondsSince(start) * 1000.0);
            }
            return bench::percentile(samples, 0.5);
        }

        // median wall time of packing it for one view, milliseconds
        double prepMs(int frames){
            memory::Arena arena("particles-web-bench");
            RenderView view;
            view.position[1] = 2.0f;
            view.position[2] = 12.0f;
            std::vector<double> samples;
            for (int i = 0; i < frames; i++){
                arena.reset();
                const auto start = bench::Clock::now();
                ParticleRenderPrep::pack(*runtime, view, arena, &jobs::system());
                samples.push_back(bench::secondsSince(start) * 1000.0);
            }
            return bench::percentile(samples, 0.5);
        }
    };

    void report(const char* metric, double value){
        Log::print("[bench] particles web %s %.3f", metric, value);
    }

    bool withinBudget(const char* metric, double value, double budget){
        if (value <= budget)
            return true;
        Log::error("[bench] particles web %s %.3f misses its budget (<= %.3f)", metric, value, budget);
        return false;
    }

    // the web rows of R21 (30 k, one thread) and R22 (prep), gated in the simd128 build; R24's
    // SIMD speedups compare it with the scalar build's figures (tools/perf/check_web_particles.py)
    void run(){
        Emitter ref(referencestack::referenceStack(true, 15000.0f));
        const double sim = ref.tickMs(40);
        const double prep = ref.prepMs(40);
        Emitter quiet(referencestack::referenceStack(false, 15000.0f));
        const double quietSim = quiet.tickMs(40);

        Log::print("[bench] particles web: %u live, kernels %s", ref.runtime->count(), noise::curlNativeBackend());
        report("ref_30k_live", (double)ref.runtime->count());
        report("ref_30k_sim_ms", sim);
        report("ref_30k_nonoise_sim_ms", quietSim);
        report("ref_30k_prep_ms", prep);
        // the engine's kernels say which build this is (this file is not built for simd128)
        if (std::strcmp(noise::curlNativeBackend(), "scalar") == 0){
            Log::print("[bench] particles_web_budgets=scalar (the fallback build: no budget)");
            return;
        }
        bool pass = withinBudget("ref_30k_sim_ms", sim, 3.0);
        pass = withinBudget("ref_30k_nonoise_sim_ms", quietSim, 1.5) && pass;
        pass = withinBudget("ref_30k_prep_ms", prep, 1.2) && pass;
        Log::print("[bench] particles_web_budgets=%s", pass ? "pass" : "fail");
    }

}

JASPERENGINE_INIT void init(){
#ifdef __EMSCRIPTEN__
    if (particlesBenchRequested())
        webbench::run();
#endif

    Engine::setCanvasSize(640, 360);
    scene.setBackgroundColor(0.05f, 0.05f, 0.08f, 1.0f);

    fire.setPosition(0.0f, 0.0f, 0.0f);
    fire.setPlayOnAwake(false);
    fire.setInlineEffect(kFire);
    ribbon.setPosition(2.5f, 0.5f, 0.0f);
    ribbon.setPlayOnAwake(false);
    ribbon.setInlineEffect(kRibbon);

    sceneCamera.setType(CameraType::CAMERA_PERSPECTIVE);
    sceneCamera.setPosition(0.0f, 1.5f, 8.0f);
    sceneCamera.setTarget(0.0f, 1.5f, 0.0f);
    sceneCamera.activate();

    // the state at 2 s, held: every frame draws the same particles
    fire.seek(2.0f);
    fire.pause();
    ribbon.seek(2.0f);
    ribbon.pause();

    Engine::setScene(&scene);
    Log::print("[gate] particle kernels: %s", noise::curlNativeBackend());
    Log::print("[gate] particles %u + %u live, state %016llx %016llx", fire.getParticleCount(), ribbon.getParticleCount(),
               (unsigned long long)hashOf(fire), (unsigned long long)hashOf(ribbon));
}
