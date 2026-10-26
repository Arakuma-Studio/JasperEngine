// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S20's test project: a particle effect with the reference stack's heavy modules (curl noise
// High, colour and size over lifetime, a sprite-less soft disc, additive) and a ribbon trail,
// seeked to 2 s and held, so the web build runs the particle kernels and the particles shader in
// WebGL2 (WEB_PROJECT=particles tools/build.sh web, GATE=particles tools/build.sh web-gate; with
// WASM_SIMD=0 both build the scalar kernels). The gate checks the live count and the state hash,
// which the simd128 and the scalar kernels must give alike (bit-identical), and the canvas centre.

#include "JasperEngine.h"
#include "math/noise/CurlNoiseBatch.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/ParticleSystemState.h"

using namespace jasperengine;

Scene scene;
Camera camera(&scene);
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

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(640, 360);
    scene.setBackgroundColor(0.05f, 0.05f, 0.08f, 1.0f);

    fire.setPosition(0.0f, 0.0f, 0.0f);
    fire.setPlayOnAwake(false);
    fire.setInlineEffect(kFire);
    ribbon.setPosition(2.5f, 0.5f, 0.0f);
    ribbon.setPlayOnAwake(false);
    ribbon.setInlineEffect(kRibbon);

    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.setPosition(0.0f, 1.5f, 8.0f);
    camera.setTarget(0.0f, 1.5f, 0.0f);
    camera.activate();

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
