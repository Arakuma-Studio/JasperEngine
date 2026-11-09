// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S49 R21a: PixelParticles::emitAt, the ember entry point S48 calls. One engine-owned emitter per
// (scene, effect), made on first use and reused; one particle at each position asked for, with its
// velocity; an Analytic or unreadable effect refused without leaving an emitter behind; a scene's
// emitters forgotten when it goes.

#include "Scene.h"
#include "component/ParticleSystemComponent.h"
#include "object/ParticleSystem.h"
#include "particles/PixelParticles.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/ParticleSystemState.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace {

    const std::string kEmbers = R"({"version": 1, "duration": 5, "maxParticles": 64, "simulationSpace": "world",
        "modules": [{"type": "main", "startLifetime": 10, "startSpeed": 0, "gravityModifier": 0},
                    {"type": "emission", "rateOverTime": 0}, {"type": "shape", "enabled": false}]})";

    size_t systemsOf(Scene& scene){
        return scene.getComponentArray<ParticleSystemComponent>()->size();
    }

}

TEST(PixelParticles, EmitAtAddsAParticleAtEachPosition){
    Scene scene;
    ASSERT_TRUE(PixelParticles::emitAt(&scene, kEmbers, {Vector3(1.0f, 2.0f, 0.0f), Vector3(-3.0f, 4.0f, 0.0f)},
                                       {Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 0.0f)}));
    const Entity emitter = PixelParticles::emitterOf(&scene, kEmbers);
    ASSERT_NE(emitter, NULL_ENTITY);
    ParticleSystem system(&scene, emitter);
    system.step(1);
    ASSERT_EQ(system.getParticleCount(), 2u);
    const ParticleSystemState* state = scene.getSystem<ParticleSubsystem>()->getState(emitter);
    ASSERT_NE(state, nullptr);
    const ParticleChannels& ch = state->runtime.particleChannels();
    float xs[2] = {ch.get(ChPosX)[0], ch.get(ChPosX)[1]};
    if (xs[0] > xs[1]) std::swap(xs[0], xs[1]);
    EXPECT_NEAR(xs[0], -3.0f, 1e-4f);
    EXPECT_NEAR(xs[1], 1.0f, 1e-4f);
    // the same effect again: the same emitter, one more particle
    ASSERT_TRUE(PixelParticles::emitAt(&scene, kEmbers, {Vector3(0.0f, 0.0f, 0.0f)}));
    EXPECT_EQ(PixelParticles::emitterOf(&scene, kEmbers), emitter);
    system.step(1);
    EXPECT_EQ(system.getParticleCount(), 3u);
    EXPECT_EQ(systemsOf(scene), 1u);
}

TEST(PixelParticles, ARefusedCallLeavesNoEmitter){
    Scene scene;
    const std::string analytic = R"({"version": 1, "duration": 4, "looping": true, "simulationMode": "analytic",
        "modules": [{"type": "main", "startLifetime": 1, "startSpeed": 2}, {"type": "emission", "rateOverTime": 6}]})";
    std::string error;
    EXPECT_FALSE(PixelParticles::emitAt(&scene, analytic, {Vector3(0.0f, 0.0f, 0.0f)}, {}, &error));
    EXPECT_NE(error.find("analytic_no_events"), std::string::npos) << error;
    EXPECT_EQ(PixelParticles::emitterOf(&scene, analytic), NULL_ENTITY);
    EXPECT_EQ(systemsOf(scene), 0u) << "the refused emitter was destroyed";
    error.clear();
    EXPECT_FALSE(PixelParticles::emitAt(&scene, "{not json", {Vector3(0.0f, 0.0f, 0.0f)}, {}, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(systemsOf(scene), 0u);
}

TEST(PixelParticles, ForgetDropsTheScenesEmitters){
    Scene scene;
    ASSERT_TRUE(PixelParticles::emitAt(&scene, kEmbers, {Vector3(0.0f, 0.0f, 0.0f)}));
    ASSERT_NE(PixelParticles::emitterOf(&scene, kEmbers), NULL_ENTITY);
    PixelParticles::forget(&scene);
    EXPECT_EQ(PixelParticles::emitterOf(&scene, kEmbers), NULL_ENTITY);
}
