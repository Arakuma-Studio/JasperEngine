// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// AC9: seek equals playing to the time; a backward scrub starts from a checkpoint; the
// checkpoints keep within their cap; prewarm shows the state at the duration, at most 1 ms a frame.

#include "ParticleTestEffects.h"
#include "ParticleTestUtil.h"

#include "Scene.h"
#include "object/ParticleSystem.h"
#include "particles/ParticleEffectLibrary.h"
#include "particles/ParticlePreview.h"
#include "subsystem/ParticleSubsystem.h"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

using namespace particletest;

namespace {

    ParticleEffectDocument seekEffect(){
        ParticleEffectDocument d = referenceStack(true, 400.0f, 4096);
        d.collision.present = true;
        CollisionPlane floor;
        floor.point = Vector3(0.0f, -0.5f, 0.0f);
        d.collision.planes = {floor};
        d.trails.present = true;
        d.trails.ratio = 0.2f;
        return d;
    }

}

TEST(ParticleSeek, MatchesPlayback){
    const auto effect = compileOrFail(seekEffect());
    EmitterInput input;
    input.seed = 9;
    EmitterRuntime played;
    played.bind(effect);
    played.reset(9);
    played.play();
    // 7.3 s of 30 Hz ticks: 219
    const double frame = 1.0 / 30.0;
    for (int f = 0; f < 219; f++) played.advance(input, frame);
    EmitterRuntime seeked;
    seeked.bind(effect);
    seeked.reset(9);
    seeked.seek(input, 7.3);
    EXPECT_EQ(seeked.tickCount(), played.tickCount());
    EXPECT_GT(seeked.count(), 100u);
    EXPECT_EQ(seeked.stateHash(), played.stateHash());
}

TEST(ParticleSeek, BackwardScrubStartsFromACheckpoint){
    const auto effect = compileOrFail(seekEffect());
    EmitterInput input;
    EmitterRuntime runtime;
    runtime.bind(effect);
    ParticlePreview preview;
    preview.seek(runtime, input, 20.0);
    while (!preview.update(runtime, input, 4.0)){}
    EXPECT_NEAR(runtime.time(), 20.0, 1.0 / 30.0);
    EXPECT_GE(preview.checkpointCount(), 20u);
    const uint64_t at20 = runtime.stateHash();
    // back to 19.5: from the checkpoint at 19 s
    preview.seek(runtime, input, 19.5);
    while (!preview.update(runtime, input, 4.0)){}
    EXPECT_LE(preview.simulatedSeconds(), 0.5 + 1e-6);
    // and the same state as a seek from the start
    EmitterRuntime fresh;
    fresh.bind(effect);
    fresh.seek(input, 19.5);
    EXPECT_EQ(runtime.stateHash(), fresh.stateHash());
    // forward again to 20 matches
    preview.seek(runtime, input, 20.0);
    while (!preview.update(runtime, input, 4.0)){}
    EXPECT_EQ(runtime.stateHash(), at20);
}

TEST(ParticleSeek, CheckpointsStayWithinTheirCap){
    ParticleEffectDocument d = referenceStack(false, 8000.0f, 32768);
    const auto effect = compileOrFail(d);
    EmitterInput input;
    EmitterRuntime runtime;
    runtime.bind(effect);
    // a cap a few checkpoints of 16 000 particles fill
    const size_t cap = 4u * 1024u * 1024u;
    ParticlePreview preview(cap);
    preview.seek(runtime, input, 30.0);
    while (!preview.update(runtime, input, 50.0)){}
    EXPECT_LE(preview.checkpointBytes(), cap);
    EXPECT_GT(preview.checkpointCount(), 1u);
    EXPECT_LT(preview.checkpointCount(), 31u);
    // a scrub back still lands on the same state, from an older checkpoint or from 0
    preview.seek(runtime, input, 12.0);
    while (!preview.update(runtime, input, 50.0)){}
    EmitterRuntime fresh;
    fresh.bind(effect);
    fresh.seek(input, 12.0);
    EXPECT_EQ(runtime.stateHash(), fresh.stateHash());
}

TEST(ParticleSeek, SeekRunsInBudgetedChunks){
    const auto effect = compileOrFail(seekEffect());
    EmitterInput input;
    EmitterRuntime runtime;
    runtime.bind(effect);
    ParticlePreview preview;
    preview.seek(runtime, input, 10.0);
    int calls = 0;
    while (!preview.update(runtime, input, 0.5)) calls++;
    EXPECT_GT(calls, 1);
}

TEST(ParticleSeek, PrewarmShowsTheStateAtTheDuration){
    ParticleEffectDocument d = seekEffect();
    d.prewarm = true;
    // 600 ticks: more than one frame's 1 ms budget on any machine (2 s, 60 ticks, fit in one
    // frame on Apple silicon, 0.9 ms)
    d.duration = 20.0f;
    const auto effect = compileOrFail(d);
    EmitterInput input;
    input.seed = 5;
    EmitterRuntime prewarmed;
    prewarmed.bind(effect);
    prewarmed.reset(5);
    prewarmed.play();
    double worstMs = 0.0;
    int frames = 0;
    while (true){
        const auto start = std::chrono::steady_clock::now();
        prewarmed.advance(input, 1.0 / 60.0);
        worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        frames++;
        if (!prewarmed.isPrewarming())
            break;
        ASSERT_LT(frames, 10000);
    }
    EXPECT_GT(frames, 1);
    // the first visible frame: the state at the duration (600 ticks)
    EmitterRuntime reference;
    reference.bind(effect);
    reference.reset(5);
    reference.seek(input, 20.0);
    EXPECT_EQ(prewarmed.tickCount(), reference.tickCount());
    EXPECT_EQ(prewarmed.stateHash(), reference.stateHash());
    std::printf("[bench] ParticleSeek prewarm: %d frames, worst %.3f ms (budget 1 ms + one tick)\n", frames, worstMs);
}

TEST(ParticleSeek, PrewarmAtPlayRunsAtOnce){
    ParticleEffectDocument d = seekEffect();
    d.prewarm = true;
    d.prewarmSync = true;
    d.duration = 2.0f;
    const auto effect = compileOrFail(d);
    EmitterInput input;
    EmitterRuntime runtime;
    runtime.bind(effect);
    runtime.play();
    runtime.advance(input, 1.0 / 60.0);
    EXPECT_FALSE(runtime.isPrewarming());
    EXPECT_EQ(runtime.tickCount(), 60u);
}

TEST(ParticleSeek, BeforeTheFirstFrameUsesTheTransformAsSet){
    // a seek right after a move (before any frame updated the world matrices) emits where the
    // system now is: world-space particles at rest sit at its position
    Scene scene;
    ParticleSystem system(&scene);
    system.setPosition(2.5f, 0.5f, -1.0f);
    system.setPlayOnAwake(false);
    ASSERT_TRUE(system.setInlineEffect(R"({"version": 1, "duration": 5, "seed": 4, "maxParticles": 100, "simulationSpace": "world",
        "modules": [{"type": "main", "startLifetime": 10, "startSpeed": 0},
                    {"type": "emission", "rateOverTime": 20},
                    {"type": "shape", "enabled": false}]})"));
    system.seek(1.0f);
    const particles::ParticleSystemState* state = scene.getSystem<ParticleSubsystem>()->getState(system.getEntity());
    ASSERT_NE(state, nullptr);
    const ParticleChannels& ch = state->runtime.particleChannels();
    ASSERT_GT(ch.count, 0u);
    for (uint32_t i = 0; i < ch.count; i++){
        EXPECT_NEAR(ch.get(ChPosX)[i], 2.5f, 1e-4f) << i;
        EXPECT_NEAR(ch.get(ChPosY)[i], 0.5f, 1e-4f) << i;
        EXPECT_NEAR(ch.get(ChPosZ)[i], -1.0f, 1e-4f) << i;
    }
}

TEST(ParticleSeek, EmitOnAStoppedSystemEmitsOnlyThat){
    // stop(clear) then emit(5): five particles at the next tick, and the effect's own emission
    // stays off (it once restarted the system)
    Scene scene;
    ParticleSystem system(&scene);
    system.setPlayOnAwake(false);
    ASSERT_TRUE(system.setInlineEffect(R"({"version": 1, "duration": 5, "seed": 4, "maxParticles": 100,
        "modules": [{"type": "main", "startLifetime": 10, "startSpeed": 0}, {"type": "emission", "rateOverTime": 600}]})"));
    system.play();
    system.step(10);
    ASSERT_GT(system.getParticleCount(), 50u);
    system.stop(true);
    system.emit(5);
    system.step(10);
    EXPECT_EQ(system.getParticleCount(), 5u);
    EXPECT_FALSE(system.isEmitting());
}

TEST(ParticleSeek, SendTriggerRaisesTheTriggerSubEmitters){
    // a GameObject's sendTrigger reaches its effect's Trigger sub-emitters (their child from the
    // effect library): every live particle spawns the entry's children at the next tick
    ParticleEffectDocument child;
    child.main.startLifetime = MinMaxCurve(1.0f);
    child.emission.rateOverTime = MinMaxCurve(0.0f);
    ParticleEffectLibrary::set("fx/trigger-child.particles", child);
    Scene scene;
    ParticleSystem system(&scene);
    system.setPlayOnAwake(false);
    ASSERT_TRUE(system.setInlineEffect(R"({"version": 1, "duration": 5, "seed": 4, "maxParticles": 100,
        "subEmitters": [{"trigger": "trigger", "triggerName": "boom", "effect": "fx/trigger-child.particles", "emitCount": 2}],
        "modules": [{"type": "main", "startLifetime": 10, "startSpeed": 0}, {"type": "emission", "rateOverTime": 60},
                    {"type": "subEmitters"}]})"));
    system.play();
    system.step(10);
    const uint32_t parents = system.getParticleCount();
    ASSERT_GT(parents, 0u);
    system.sendTrigger("boom");
    system.step(1);
    const particles::ParticleSystemState* state = scene.getSystem<ParticleSubsystem>()->getState(system.getEntity());
    ASSERT_NE(state, nullptr);
    EXPECT_GE(state->runtime.subEmitEvents().size(), (size_t)std::min<uint32_t>(parents, 64u));
    ParticleEffectLibrary::clear();
}

TEST(ParticleSeek, AutoRandomSeedsAreTheScenesOwnSequence){
    // Auto Random Seed (seed 0): every play in a scene draws its next seed, and a new scene starts
    // the sequence over, whatever other scenes played before it
    auto playSeeds = [](){
        Scene scene;
        ParticleSystem system(&scene);
        system.setPlayOnAwake(false);
        EXPECT_TRUE(system.setInlineEffect(R"({"version": 1, "duration": 1, "maxParticles": 100,
            "modules": [{"type": "main", "startLifetime": 0.1, "startSpeed": 0}, {"type": "emission", "rateOverTime": 60}]})"));
        std::vector<uint32_t> seeds;
        ParticleSubsystem* subsystem = scene.getSystem<ParticleSubsystem>().get();
        for (int play = 0; play < 2; play++){
            system.play();
            system.step(10);
            const particles::ParticleSystemState* state = subsystem->getState(system.getEntity());
            EXPECT_NE(state, nullptr);
            seeds.push_back(state ? state->seed : 0u);
            system.stop(true);
        }
        return seeds;
    };
    const std::vector<uint32_t> first = playSeeds();
    const std::vector<uint32_t> second = playSeeds();
    ASSERT_EQ(first.size(), 2u);
    EXPECT_NE(first[0], 0u);
    EXPECT_NE(first[0], first[1]) << "each play draws a new seed";
    EXPECT_EQ(second, first) << "a new scene starts its own sequence";
}
