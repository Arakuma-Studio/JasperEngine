// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The runtime features of docs/specs/S91-effect-authoring.md: the Ease mode of MinMaxCurve (R6,
// AC6), trigger actions and triggers with no live particle (R9, AC8), exposed parameters written
// without compiling (R10, AC9), Follow Parent sub-emitters (R11), Sound Events of bursts,
// sub-emitters and triggers (R13) and the Ring renderer's mesh (R8).

#include "ParticleTestUtil.h"

#include "AllocationCounter.h"

#include "Scene.h"
#include "action/Ease.h"
#include "component/Transform.h"
#include "object/ParticleSystem.h"
#include "particles/ParticleEffectLibrary.h"
#include "particles/ParticleMeshes.h"
#include "particles/ParticleModules.h"
#include "subsystem/AudioSystem.h"
#include "subsystem/ParticleSubsystem.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace particletest;

namespace s91test {

    std::string readText(const std::filesystem::path& path){
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    ParticleEffectDocument readOrFail(const std::string& json){
        ParticleEffectDocument d;
        std::vector<Diagnostic> diagnostics;
        EXPECT_TRUE(ParticleEffectCodec::read(json, d, diagnostics)) << ParticleEffectCodec::format(diagnostics);
        return d;
    }

    // a parent that emits nothing of its own, with one Trigger sub-emitter named "boom"
    ParticleEffectDocument triggerParent(){
        ParticleEffectDocument d;
        d.tickRate = 30;
        d.simulationSpace = SimulationSpace::World;
        d.main.startLifetime = MinMaxCurve(1.0f);
        d.emission.rateOverTime = MinMaxCurve(0.0f);
        d.shape.enabled = false;
        d.subEmittersModule.present = true;
        SubEmitterEntry e;
        e.trigger = SubEmitterTrigger::Trigger;
        e.triggerName = "boom";
        e.effect = "child.particles";
        e.emitCount = 5;
        d.subEmitters = {e};
        return d;
    }

    std::shared_ptr<const CompiledEffect> stillChild(const CompileEnvironment& env = {}, const Vector3& offset = Vector3(0.0f, 0.0f, 0.0f)){
        ParticleEffectDocument d;
        d.tickRate = 30;
        d.simulationSpace = SimulationSpace::World;
        d.main.startLifetime = MinMaxCurve(100.0f);
        d.main.startSpeed = MinMaxCurve(0.0f);
        d.emission.rateOverTime = MinMaxCurve(0.0f);
        // a point at `offset` from where it is fired
        d.shape.shape = ShapeType::Box;
        d.shape.scale = Vector3(0.0f, 0.0f, 0.0f);
        d.shape.position = offset;
        return compileOrFail(d, env);
    }

    // one parent particle with `velocity`, its Birth child following it
    struct Follow {
        Sim parent;
        explicit Follow(const ParticleEffectDocument& d, const SubEmitterEntry& entry, const Vector3& offset): parent(d){
            CompileEnvironment env;
            env.followsParent = entry.followParent;
            env.followsRotation = entry.followRotation;
            env.followsScale = entry.followScale;
            std::shared_ptr<const CompiledEffect> child = stillChild(env, offset);
            parent.runtime.bindSubEmitters([child](const SubEmitterEntry&){ return child; });
            parent.runtime.reset(1);
            parent.runtime.play();
        }
        const EmitterRuntime& kid() const { return *parent.runtime.child(0); }
        Vector3 kidAt(uint32_t i = 0) const {
            const ParticleChannels& c = kid().particleChannels();
            return Vector3(c.get(ChPosX)[i], c.get(ChPosY)[i], c.get(ChPosZ)[i]);
        }
        Vector3 parentAt(uint32_t i = 0) const {
            return Vector3(parent.ch(ChPosX, i), parent.ch(ChPosY, i), parent.ch(ChPosZ, i));
        }
    };

    ParticleEffectDocument followParent(const Vector3& velocity, bool rotation, bool scale){
        ParticleEffectDocument d = singleParticle(velocity, 30);
        d.main.startLifetime = MinMaxCurve(2.0f);
        d.subEmittersModule.present = true;
        SubEmitterEntry e;
        e.trigger = SubEmitterTrigger::Birth;
        e.effect = "child.particles";
        e.emitCount = 1;
        e.followParent = true;
        e.followRotation = rotation;
        e.followScale = scale;
        d.subEmitters = {e};
        return d;
    }

}

using namespace s91test;

// ---------------------------------------------------------------- R6: Ease

TEST(ParticleEase, IsStoredAsItsCurveAndEvaluatesAsIt){
    // AC6: written as an ordinary curve plus its preset, read back as the same Ease, and a reader
    // that knows no Ease (the preset dropped) evaluates the very same curve
    ParticleEffectDocument d;
    d.main.startSize = MinMaxCurve::fromEase(EaseType::QUAD_OUT, 0.5f, 2.0f);
    ASSERT_EQ(d.main.startSize.mode, CurveMode::Ease);
    ASSERT_GE(d.main.startSize.curve.keys.size(), 2u);
    EXPECT_NEAR(d.main.startSize.evaluate(0.0f, 0.0f), 0.5f, 1.0e-5f);
    EXPECT_NEAR(d.main.startSize.evaluate(1.0f, 0.0f), 2.0f, 1.0e-5f);
    // quad out: past the straight line in the middle
    EXPECT_GT(d.main.startSize.evaluate(0.5f, 0.0f), 1.25f + 0.2f);

    const nlohmann::ordered_json j = ParticleEffectCodec::toJson(d);
    nlohmann::ordered_json stored;
    for (const auto& module : j["modules"]){
        if (module["type"] == "main") stored = module["startSize"];
    }
    ASSERT_TRUE(stored.is_object()) << j.dump(2);
    EXPECT_EQ(stored["mode"], "curve");
    EXPECT_EQ(stored["ease"], "quadOut");
    EXPECT_TRUE(stored.contains("curve"));

    const ParticleEffectDocument back = readOrFail(ParticleEffectCodec::write(d));
    EXPECT_EQ(back.main.startSize.mode, CurveMode::Ease);
    EXPECT_EQ(back.main.startSize.ease, EaseType::QUAD_OUT);
    EXPECT_EQ(back.main.startSize.curve, d.main.startSize.curve);
    EXPECT_EQ(ParticleEffectCodec::write(back), ParticleEffectCodec::write(d));

    nlohmann::ordered_json old = j;
    for (auto& module : old["modules"]){
        if (module["type"] == "main") module["startSize"].erase("ease");
    }
    ParticleEffectDocument older;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(ParticleEffectCodec::fromJson(old, older, diagnostics)) << ParticleEffectCodec::format(diagnostics);
    EXPECT_EQ(older.main.startSize.mode, CurveMode::Curve);
    for (float t : {0.0f, 0.1f, 0.37f, 0.5f, 0.8f, 1.0f})
        EXPECT_EQ(older.main.startSize.evaluate(t, 0.0f), back.main.startSize.evaluate(t, 0.0f)) << t;

    // it compiles as that curve does
    const auto ease = compileOrFail(back), curve = compileOrFail(older);
    EXPECT_EQ(ease->program.pool, curve->program.pool);
}

TEST(ParticleEase, AnEditOfItsPresetOrEndsMakesTheCurveAgain){
    ParticleEffectDocument d;
    d.main.startSize = MinMaxCurve::fromEase(EaseType::LINEAR, 0.0f, 1.0f);
    // a patch of the preset (the stored curve left as it was)
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(ParticleEffectCodec::applyMergePatch(d, nlohmann::ordered_json::parse(R"({"modules": {"main": {"startSize": {"ease": "step"}}}})"),
                                                     diagnostics)) << ParticleEffectCodec::format(diagnostics);
    EXPECT_EQ(d.main.startSize.mode, CurveMode::Ease);
    EXPECT_EQ(d.main.startSize.curve, MinMaxCurve::easeCurve(EaseType::STEP, 0.0f, 1.0f));
    // of its end
    ASSERT_TRUE(ParticleEffectCodec::applyMergePatch(d, nlohmann::ordered_json::parse(R"({"modules": {"main": {"startSize": {"constantMax": 3}}}})"),
                                                     diagnostics)) << ParticleEffectCodec::format(diagnostics);
    EXPECT_EQ(d.main.startSize.curve, MinMaxCurve::easeCurve(EaseType::STEP, 0.0f, 3.0f));
    // asked for as {mode: ease}, the way an agent writes it
    ASSERT_TRUE(ParticleEffectCodec::applyMergePatch(d, nlohmann::ordered_json::parse(
        R"({"modules": {"emission": {"rateOverTime": {"mode": "ease", "ease": "sineIn", "constant": 10, "constantMax": 40}}}})"), diagnostics));
    EXPECT_EQ(d.emission.rateOverTime.mode, CurveMode::Ease);
    EXPECT_NEAR(d.emission.rateOverTime.evaluate(1.0f, 0.0f), 40.0f, 1.0e-4f);
    // and back to a plain curve: the preset goes
    ASSERT_TRUE(ParticleEffectCodec::applyMergePatch(d, nlohmann::ordered_json::parse(R"({"modules": {"main": {"startSize": {"mode": "curve"}}}})"),
                                                     diagnostics));
    EXPECT_EQ(d.main.startSize.mode, CurveMode::Curve);
    // an exposed end moves the curve too
    d.exposed = {{"Top", "main.startSize.constantMax", ExposedType::Float}};
    d.main.startSize = MinMaxCurve::fromEase(EaseType::QUAD_IN, 0.0f, 1.0f);
    ASSERT_TRUE(ParticleModules::setExposed(d, "Top", Vector4(5.0f, 0.0f, 0.0f, 0.0f)));
    EXPECT_NEAR(d.main.startSize.evaluate(1.0f, 0.0f), 5.0f, 1.0e-4f);
}

TEST(ParticleEase, APatchMakesACurveEaseWithItsOwnCurve){
    // review I1: a value that was a curve, patched to Ease (as the guide writes it, or by its preset
    // alone), takes the ease's curve, not the one it had; review M3: a patch of an Ease value's
    // curve alone leaves Ease
    auto patched = [](const char* patch){
        ParticleEffectDocument d;
        d.main.startSize = MinMaxCurve::fromCurve(Curve::linear(1.0f, 0.0f));
        d.main.startSpeed = MinMaxCurve::fromEase(EaseType::QUAD_IN, 0.0f, 1.0f);
        std::vector<Diagnostic> diagnostics;
        EXPECT_TRUE(ParticleEffectCodec::applyMergePatch(d, nlohmann::ordered_json::parse(patch), diagnostics))
            << ParticleEffectCodec::format(diagnostics);
        return d;
    };
    ParticleEffectDocument d = patched(R"({"modules": {"main": {"startSize": {"mode": "ease", "ease": "quadOut", "constant": 0, "constantMax": 2}}}})");
    EXPECT_EQ(d.main.startSize.mode, CurveMode::Ease);
    EXPECT_EQ(d.main.startSize.curve, MinMaxCurve::easeCurve(EaseType::QUAD_OUT, 0.0f, 2.0f));
    EXPECT_NEAR(d.main.startSize.evaluate(1.0f, 0.0f), 2.0f, 1.0e-4f);
    d = patched(R"({"modules": {"main": {"startSize": {"ease": "sineIn"}}}})");
    EXPECT_EQ(d.main.startSize.mode, CurveMode::Ease);
    EXPECT_EQ(d.main.startSize.curve, MinMaxCurve::easeCurve(EaseType::SINE_IN, d.main.startSize.constant, d.main.startSize.constantMax));
    d = patched(R"({"modules": {"main": {"startSpeed": {"curve": [[0, 0], [1, 5]]}}}})");
    EXPECT_EQ(d.main.startSpeed.mode, CurveMode::Curve) << "a hand-made curve is no longer the ease";
    EXPECT_NEAR(d.main.startSpeed.evaluate(1.0f, 0.0f), 5.0f, 1.0e-4f);
}

TEST(ParticleEase, WritingItsEndsDoesNotFitItAgain){
    // review I2: a script or a drag writes an Ease value's end every frame; the preset's fit is made
    // once, and the curve is only scaled, in the storage it has
    ASSERT_TRUE(testsupport::countsAllocations());
    MinMaxCurve value = MinMaxCurve::fromEase(EaseType::BOUNCE_IN_OUT, 0.0f, 1.0f);
    ParticleEffectDocument d;
    d.main.startSize = value;
    d.exposed = {{"Size", "main.startSize.constantMax", ExposedType::Float}};
    testsupport::ThreadAllocationCount allocations;
    for (int frame = 0; frame < 30; frame++){
        value.constantMax = 1.0f + 0.1f * (float)frame;
        value.rebuildEase();
        ASSERT_TRUE(ParticleModules::writeExposedMember(d.main.startSize, "constantMax", 2.0f + 0.1f * (float)frame));
    }
    EXPECT_EQ(allocations.count(), 0u);
    EXPECT_EQ(value.curve, MinMaxCurve::easeCurve(EaseType::BOUNCE_IN_OUT, 0.0f, 3.9f));
    EXPECT_NEAR(d.main.startSize.evaluate(1.0f, 0.0f), 4.9f, 1.0e-4f);
}

TEST(ParticleEase, AnExposedValueWithoutAMemberMovesItsEnd){
    // review M2: the Inspector shows Start, End and the preset; a member-less exposed value is the
    // end, not the multiplier it does not show
    ParticleEffectDocument d;
    d.main.startSize = MinMaxCurve::fromEase(EaseType::QUAD_OUT, 0.0f, 1.0f);
    d.exposed = {{"Size", "main.startSize", ExposedType::Float}};
    ASSERT_TRUE(ParticleModules::setExposed(d, "Size", Vector4(3.0f, 0.0f, 0.0f, 0.0f)));
    EXPECT_EQ(d.main.startSize.constantMax, 3.0f);
    EXPECT_EQ(d.main.startSize.multiplier, 1.0f);
    EXPECT_NEAR(d.main.startSize.evaluate(1.0f, 0.0f), 3.0f, 1.0e-4f);
    Vector4 read;
    ASSERT_TRUE(ParticleModules::getExposed(d, "Size", read));
    EXPECT_EQ(read.x, 3.0f);
}

TEST(ParticleEase, EveryEffectOfTheRepositoryStillLoads){
    // AC6: the committed .particles files read as before (no error), and write back unchanged
    const std::filesystem::path root = std::filesystem::path(JASPER_TEST_REPO_DIR) / "tools";
    int files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)){
        if (!entry.is_regular_file() || entry.path().extension() != ".particles")
            continue;
        files++;
        const std::string text = readText(entry.path());
        ParticleEffectDocument d;
        std::vector<Diagnostic> diagnostics;
        EXPECT_TRUE(ParticleEffectCodec::read(text, d, diagnostics)) << entry.path() << ": " << ParticleEffectCodec::format(diagnostics);
        ParticleEffectDocument again;
        diagnostics.clear();
        ASSERT_TRUE(ParticleEffectCodec::read(ParticleEffectCodec::write(d), again, diagnostics)) << entry.path();
        EXPECT_EQ(ParticleEffectCodec::write(again), ParticleEffectCodec::write(d)) << entry.path();
    }
    EXPECT_GT(files, 0) << "no .particles under " << root;
}

// ---------------------------------------------------------------- R9: triggers

TEST(ParticleTriggers, FireTheirSubEmittersWithNoLiveParticle){
    // AC8: a parent with no particle, stopped or playing, fires its Trigger children once at the emitter
    for (bool playing : {false, true}){
        // (the entry follows its parent: a child fired from no particle has none)
        ParticleEffectDocument d = triggerParent();
        d.subEmitters[0].followParent = true;
        Sim sim(d);
        CompileEnvironment env;
        env.followsParent = true;
        std::shared_ptr<const CompiledEffect> child = stillChild(env);
        sim.runtime.bindSubEmitters([child](const SubEmitterEntry&){ return child; });
        sim.runtime.reset(1);
        if (!playing) sim.runtime.stop(true);
        sim.moveTo(3.0f, 0.0f, 0.0f);
        sim.ticks(2);
        ASSERT_EQ(sim.count(), 0u);
        sim.runtime.sendTrigger(nameHash("boom"));
        sim.ticks(1);
        ASSERT_NE(sim.runtime.child(0), nullptr);
        EXPECT_EQ(sim.runtime.child(0)->count(), 5u) << (playing ? "playing" : "stopped");
        EXPECT_NEAR(sim.runtime.child(0)->particleChannels().get(ChPosX)[0], 3.0f, 1.0e-4f) << "at the emitter";
        // another name fires nothing
        sim.runtime.sendTrigger(nameHash("other"));
        sim.ticks(1);
        EXPECT_EQ(sim.runtime.child(0)->count(), 5u);
        // fired from no particle, a child has no parent to follow
        ASSERT_TRUE(sim.runtime.child(0)->program()->has(ChParentId));
        EXPECT_EQ(sim.runtime.child(0)->particleChannels().getU(ChParentId)[0], kNoParent);
    }
}

TEST(ParticleTriggers, StartStopAndPauseTheEmitter){
    // AC8: the effect's triggers play, pause, stop and clear the emitter that receives them
    ParticleEffectDocument d = readOrFail(R"({"version": 1, "duration": 5, "looping": true, "tickRate": 30, "maxParticles": 100,
        "triggers": [{"name": "go"}, {"name": "hold", "action": "pause"}, {"name": "halt", "action": "stop"},
                     {"name": "wipe", "action": "stopAndClear"}, {"name": "nothing", "action": "none"}],
        "modules": [{"type": "main", "startLifetime": 10, "startSpeed": 0}, {"type": "emission", "rateOverTime": 30}]})");
    ASSERT_EQ(d.triggers.size(), 5u);
    EXPECT_EQ(d.triggers[0].action, TriggerAction::Play) << "Play by default";
    Sim sim(d);
    sim.runtime.stop(true);
    sim.ticks(5);
    ASSERT_EQ(sim.count(), 0u);
    sim.runtime.sendTrigger(nameHash("go"));
    EXPECT_TRUE(sim.runtime.isPlaying());
    sim.ticks(10);
    const uint32_t emitted = sim.count();
    EXPECT_GT(emitted, 5u);
    sim.runtime.sendTrigger(nameHash("hold"));
    EXPECT_TRUE(sim.runtime.isPaused());
    sim.runtime.sendTrigger(nameHash("nothing"));
    EXPECT_TRUE(sim.runtime.isPaused());
    sim.runtime.sendTrigger(nameHash("go"));
    EXPECT_TRUE(sim.runtime.isPlaying());
    sim.runtime.sendTrigger(nameHash("halt"));
    EXPECT_FALSE(sim.runtime.isEmitting());
    sim.ticks(3);
    EXPECT_EQ(sim.count(), emitted) << "a stop keeps the particles";
    sim.runtime.sendTrigger(nameHash("wipe"));
    EXPECT_EQ(sim.count(), 0u);
    EXPECT_EQ(sim.runtime.state(), PlayState::Stopped);

    std::vector<Diagnostic> diagnostics;
    ParticleEffectDocument unnamed = d;
    unnamed.triggers.push_back(EffectTrigger());
    EXPECT_FALSE(ParticleEffectCodec::validate(unnamed, diagnostics)) << "a trigger has a name";
}

TEST(ParticleTriggers, ActFromTheParticleSystem){
    // the component's way (Lua's ps:sendTrigger, control_particles' trigger): a stopped system
    Scene scene;
    ParticleSystem system(&scene);
    system.setPlayOnAwake(false);
    ASSERT_TRUE(system.setInlineEffect(R"({"version": 1, "duration": 5, "tickRate": 30, "maxParticles": 100,
        "triggers": [{"name": "start"}, {"name": "freeze", "action": "pause"}],
        "modules": [{"type": "main", "startLifetime": 10, "startSpeed": 0}, {"type": "emission", "rateOverTime": 60}]})"));
    ParticleSubsystem& subsystem = *scene.getSystem<ParticleSubsystem>();
    ASSERT_NE(subsystem.getState(system.getEntity()), nullptr);
    EXPECT_FALSE(system.isPlaying());
    system.sendTrigger("start");
    EXPECT_TRUE(system.isPlaying());
    for (int i = 0; i < 10; i++) subsystem.update(1.0 / 30.0);
    EXPECT_GT(system.getParticleCount(), 0u);
    system.sendTrigger("freeze");
    EXPECT_TRUE(system.isPaused());
}

TEST(ParticleTriggers, APlayTriggerDrawsASeedAsPlayDoes){
    // review M1: a seedless effect started again by its trigger gets a new seed each play, as
    // ParticleSystem.play does (seed 0: "a new seed each play")
    Scene scene;
    ParticleSystem system(&scene);
    system.setPlayOnAwake(false);
    ASSERT_TRUE(system.setInlineEffect(R"({"version": 1, "duration": 5, "tickRate": 30, "maxParticles": 100,
        "triggers": [{"name": "start"}],
        "modules": [{"type": "main", "startLifetime": 10, "startSpeed": 1}, {"type": "emission", "rateOverTime": 30}]})"));
    ParticleSubsystem& subsystem = *scene.getSystem<ParticleSubsystem>();
    ASSERT_NE(subsystem.getState(system.getEntity()), nullptr);
    system.sendTrigger("start");
    for (int i = 0; i < 5; i++) subsystem.update(1.0 / 30.0);
    const uint32_t first = subsystem.getStats(system.getEntity()).seed;
    system.stop(true);
    system.sendTrigger("start");
    for (int i = 0; i < 5; i++) subsystem.update(1.0 / 30.0);
    EXPECT_NE(subsystem.getStats(system.getEntity()).seed, first);
    EXPECT_GT(system.getParticleCount(), 0u);
}

// ---------------------------------------------------------------- R10: exposed parameters

TEST(ParticleExposed, ChangingAValueEachFrameCompilesNothing){
    // AC9: a script driving numeric parameters every frame compiles nothing; a structural change compiles
    Scene scene;
    ParticleSystem system(&scene);
    ASSERT_TRUE(system.setInlineEffect(R"({"version": 1, "duration": 5, "tickRate": 30, "maxParticles": 1000,
        "exposed": [{"name": "Rate", "path": "emission.rateOverTime"}, {"name": "Tint", "path": "main.startColor", "type": "color"},
                    {"name": "Gravity", "path": "main.gravityModifier"}],
        "modules": [{"type": "main", "startLifetime": 1, "startSpeed": 1}, {"type": "emission", "rateOverTime": 10}]})"));
    ParticleSubsystem& subsystem = *scene.getSystem<ParticleSubsystem>();
    subsystem.update(1.0 / 30.0);
    const ParticleSystemState* state = subsystem.getState(system.getEntity());
    ASSERT_NE(state, nullptr);
    const uint32_t compiles = ParticleCompiler::compileCount();
    for (int frame = 0; frame < 30; frame++){
        ASSERT_TRUE(system.setFloat("Rate", 20.0f + (float)frame));
        ASSERT_TRUE(system.setColor("Tint", Vector4(1.0f, 0.5f, 0.25f + 0.01f * (float)frame, 1.0f)));
        subsystem.update(1.0 / 30.0);
    }
    EXPECT_EQ(ParticleCompiler::compileCount(), compiles) << "a value changed per frame compiles nothing";
    state = subsystem.getState(system.getEntity());
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->effect->document.emission.rateOverTime.constant, 49.0f);
    EXPECT_EQ(state->runtime.program()->emission.rate.c0, 49.0f) << "the kernels read the new rate";
    EXPECT_EQ(state->runtime.effect(), state->effect.get());
    EXPECT_GT(system.getParticleCount(), 30u);

    // gravity from none to some adds the Forces op: that compiles
    ASSERT_TRUE(system.setFloat("Gravity", 1.0f));
    subsystem.update(1.0 / 30.0);
    EXPECT_EQ(ParticleCompiler::compileCount(), compiles + 1) << "a structural change compiles";
    state = subsystem.getState(system.getEntity());
    EXPECT_EQ(state->effect->document.main.gravityModifier.constant, 1.0f);
    EXPECT_EQ(state->effect->document.emission.rateOverTime.constant, 49.0f) << "the other values kept";
}

TEST(ParticleExposed, AnEffectWithoutThemPaysNothing){
    const auto effect = compileOrFail(singleParticle(Vector3(1.0f, 0.0f, 0.0f)));
    EXPECT_TRUE(effect->exposedSlots.empty());
    EXPECT_TRUE(effect->exposedBindings.empty());
    for (const Op& op : effect->program.spawnOps){
        if (op.code == OpCode::SpawnStartValues)
            EXPECT_EQ(effect->program.param<StartParams>(op.params).lifetime.pad, 0u) << "no slot tags";
    }
}

TEST(ParticleExposed, WritingAValueGivesWhatCompilingItGives){
    // every curve and colour field of every module, exposed: the value written into the compiled
    // effect gives the program a compile of the new value gives (pool, params, hints and the
    // particles it simulates), or it is refused (and compiled instead)
    ParticleEffectDocument base = readOrFail(R"({"version": 1, "duration": 2, "looping": true, "tickRate": 30, "maxParticles": 400,
        "lodEnabled": true, "modules": [
        {"type": "main", "startLifetime": 1.5, "startSpeed": 2, "gravityModifier": 0.3, "startRotation": 10},
        {"type": "emission", "rateOverTime": 120},
        {"type": "shape", "shape": "cone"},
        {"type": "velocityOverLifetime", "x": 0.2, "radial": 0.1, "orbitalY": 20, "speedModifier": 0.9},
        {"type": "limitVelocity", "limit": 3, "drag": 0.1, "dampen": 0.5},
        {"type": "forceOverLifetime", "y": 0.5},
        {"type": "colorOverLifetime", "color": [0.9, 0.8, 0.7, 1]},
        {"type": "colorBySpeed", "color": [0.9, 0.9, 0.7, 1]},
        {"type": "sizeOverLifetime", "size": 0.8},
        {"type": "sizeBySpeed", "size": 0.9},
        {"type": "rotationOverLifetime", "angularVelocity": 30},
        {"type": "rotationBySpeed", "angularVelocity": 20},
        {"type": "noise", "strength": 0.5, "scrollSpeed": 0.2, "sizeAmount": 0.1},
        {"type": "orbit", "angularSpeed": 45, "radialPull": 0.1},
        {"type": "textureSheetAnimation", "tilesX": 2, "tilesY": 2},
        {"type": "trails", "lifetime": 0.5},
        {"type": "customData", "x": 0.5},
        {"type": "collision", "collisionType": "planes", "planes": [{"point": [0, -5, 0], "normal": [0, 1, 0]}]}]})");
    std::set<std::string> refused, rewritten;
    for (int curveMode = 0; curveMode < 2; curveMode++){
        ParticleEffectDocument doc = base;
        auto each = [&](const char* module, const StructDesc& desc){
            for (const FieldDesc& f : desc.fields){
                if (f.type != FieldType::MinMaxCurve && f.type != FieldType::MinMaxGradient)
                    continue;
                const std::string path = (module ? std::string(module) + "." : std::string()) + f.key;
                ParticleEffectDocument start = doc;
                ParticleModules::Resolved r = ParticleModules::resolve(start, path);
                ASSERT_NE(r.field, nullptr) << path;
                Vector4 next(0.4f, 0.0f, 0.0f, 0.0f);
                std::string member;
                if (f.type == FieldType::MinMaxCurve){
                    MinMaxCurve& c = *static_cast<MinMaxCurve*>(r.storage);
                    if (curveMode){
                        c = MinMaxCurve::fromCurve(Curve::linear(0.2f, 1.0f), 0.7f);
                        member = ".multiplier";
                    }else{
                        c = MinMaxCurve(0.7f);
                    }
                }else{
                    MinMaxGradient& g = *static_cast<MinMaxGradient*>(r.storage);
                    if (curveMode) continue;
                    g = MinMaxGradient(Vector4(0.8f, 0.6f, 0.4f, 1.0f));
                    next = Vector4(0.2f, 0.7f, 0.9f, 0.5f);
                }
                start.exposed = {{"P", path + member, f.type == FieldType::MinMaxGradient ? ExposedType::Color : ExposedType::Float}};
                std::shared_ptr<CompiledEffect> written = ParticleCompiler::compile(start);
                if (!written->valid)
                    continue;   // (a value the field does not take in this mode: Start Delay as a curve)
                const std::string name = path + member;
                if (!ParticleCompiler::applyExposed(*written, 0, next)){
                    refused.insert(name);
                    continue;
                }
                rewritten.insert(name);
                ParticleEffectDocument changed = start;
                ASSERT_TRUE(ParticleModules::setExposed(changed, "P", next)) << name;
                std::shared_ptr<CompiledEffect> compiled = ParticleCompiler::compile(changed);
                ASSERT_TRUE(compiled->valid) << name;
                EXPECT_EQ(ParticleEffectCodec::write(written->document), ParticleEffectCodec::write(changed)) << name;
                EXPECT_EQ(written->program.pool, compiled->program.pool) << name;
                EXPECT_EQ(written->program.maxStartSize, compiled->program.maxStartSize) << name;
                EXPECT_EQ(written->program.maxSizeFactor, compiled->program.maxSizeFactor) << name;
                EXPECT_EQ(written->program.maxSpeedHint, compiled->program.maxSpeedHint) << name;
                // the params outside the pool, where the written ones are
                ASSERT_EQ(written->exposedSlots.size(), compiled->exposedSlots.size()) << name;
                for (const ExposedSlot& slot : written->exposedSlots){
                    const size_t size = slot.gradient ? sizeof(GradientParam) : sizeof(CurveParam);
                    for (uint32_t at : slot.inProgram)
                        EXPECT_EQ(std::memcmp(reinterpret_cast<const unsigned char*>(&written->program) + at,
                                              reinterpret_cast<const unsigned char*>(&compiled->program) + at, size), 0) << name;
                    for (uint32_t at : slot.inBursts)
                        EXPECT_EQ(std::memcmp(reinterpret_cast<const unsigned char*>(written->program.emission.bursts.data()) + at,
                                              reinterpret_cast<const unsigned char*>(compiled->program.emission.bursts.data()) + at, size), 0) << name;
                }
                // and what they simulate and show
                EmitterRuntime a, b;
                a.bind(written);
                b.bind(compiled);
                a.reset(7);
                b.reset(7);
                a.play();
                b.play();
                EmitterInput input;
                for (int t = 0; t < 20; t++){
                    a.step(input);
                    b.step(input);
                }
                ASSERT_EQ(a.count(), b.count()) << name;
                EXPECT_EQ(a.stateHash(), b.stateHash()) << name;
                for (uint32_t i = 0; i < a.count(); i += 7){
                    float ca[4], cb[4], sa, sb, ra, rb;
                    a.appearance(i, ca, sa, ra);
                    b.appearance(i, cb, sb, rb);
                    EXPECT_EQ(std::memcmp(ca, cb, sizeof(ca)), 0) << name;
                    EXPECT_EQ(sa, sb) << name;
                    EXPECT_EQ(ra, rb) << name;
                }
            }
        };
        each(nullptr, ParticleModules::documentFields());
        for (const ModuleDesc& m : ParticleModules::all())
            each(m.key, m.fields);
    }
    // the values the compiler reads besides their params are the ones refused: the noise's scroll
    // and the orbital offsets (as constants, so in any mode), a rotation curve's baked integral
    const std::set<std::string> readOtherwise = {"noise.scrollSpeed", "velocityOverLifetime.offsetX", "velocityOverLifetime.offsetY",
                                                 "velocityOverLifetime.offsetZ"};
    for (const std::string& name : refused){
        const std::string field = name.substr(0, name.rfind(".multiplier") == std::string::npos ? name.size() : name.rfind(".multiplier"));
        EXPECT_TRUE(readOtherwise.count(field) || name == "rotationOverLifetime.angularVelocity.multiplier") << name << " was refused";
    }
    for (const char* name : {"emission.rateOverTime", "main.startLifetime", "main.startSize", "main.startSpeed", "main.startColor",
                             "colorOverLifetime.color", "sizeOverLifetime.size.multiplier", "noise.strength", "trails.widthOverTrail"})
        EXPECT_TRUE(rewritten.count(name)) << name << " is written without compiling";
    EXPECT_GT(rewritten.size(), 60u);
}

// ---------------------------------------------------------------- R11: Follow Parent

TEST(ParticleFollowParent, TheChildrenMoveWithTheirParent){
    for (bool follow : {false, true}){
        ParticleEffectDocument d = followParent(Vector3(6.0f, 0.0f, 0.0f), false, false);
        d.subEmitters[0].followParent = follow;
        Follow f(d, d.subEmitters[0], Vector3(0.0f, 1.0f, 0.0f));
        f.parent.ticks(31);
        ASSERT_EQ(f.parent.count(), 1u);
        ASSERT_EQ(f.kid().count(), 1u);
        const Vector3 parent = f.parentAt(), kid = f.kidAt();
        EXPECT_NEAR(parent.x, 6.0f, 0.3f);
        if (follow){
            EXPECT_NEAR(kid.x, parent.x, 1.0e-3f) << "it keeps up with its parent";
            EXPECT_NEAR(kid.y, parent.y + 1.0f, 1.0e-3f) << "at the offset it was born at";
        }else{
            EXPECT_NEAR(kid.x, 0.0f, 0.3f) << "where it was born";
        }
    }
    // once its parent died it stays where it was
    ParticleEffectDocument d = followParent(Vector3(6.0f, 0.0f, 0.0f), false, false);
    d.main.startLifetime = MinMaxCurve(0.5f);
    Follow f(d, d.subEmitters[0], Vector3(0.0f, 0.0f, 0.0f));
    f.parent.ticks(30);
    ASSERT_EQ(f.parent.count(), 0u);
    ASSERT_EQ(f.kid().count(), 1u);
    const float x = f.kidAt().x;
    EXPECT_NEAR(x, 3.0f, 0.3f);
    f.parent.ticks(10);
    EXPECT_EQ(f.kidAt().x, x);
}

TEST(ParticleFollowParent, RotationAndScaleTurnAndSpreadThem){
    // a still parent turning 90 degrees a second and growing from 1 to 2 over its 2 s: after 1 s its
    // child, born one unit along X, is a quarter turn around and 1.5 units out
    ParticleEffectDocument d = followParent(Vector3(0.0f, 0.0f, 0.0f), true, true);
    d.rotationOverLifetime.present = true;
    d.rotationOverLifetime.angularVelocity = MinMaxCurve(90.0f);
    d.sizeOverLifetime.present = true;
    d.sizeOverLifetime.size = MinMaxCurve::fromCurve(Curve::linear(0.5f, 1.0f), 2.0f);
    Follow f(d, d.subEmitters[0], Vector3(1.0f, 0.0f, 0.0f));
    f.parent.ticks(31);
    ASSERT_EQ(f.kid().count(), 1u);
    const Vector3 kid = f.kidAt();
    EXPECT_NEAR(kid.x, 0.0f, 0.02f);
    EXPECT_NEAR(kid.y, 1.5f, 0.02f);
    EXPECT_NEAR(kid.z, 0.0f, 1.0e-4f);
}

TEST(ParticleFollowParent, ALocalChildFollowsAWorldParent){
    // review M6: parent and child in different simulation spaces (the conversions of followStage)
    ParticleEffectDocument d = followParent(Vector3(6.0f, 0.0f, 0.0f), false, false);
    Sim parent(d);
    parent.moveTo(5.0f, 0.0f, 0.0f);
    CompileEnvironment env;
    env.followsParent = true;
    ParticleEffectDocument kid;
    kid.tickRate = 30;
    kid.simulationSpace = SimulationSpace::Local;
    kid.main.startLifetime = MinMaxCurve(100.0f);
    kid.main.startSpeed = MinMaxCurve(0.0f);
    kid.emission.rateOverTime = MinMaxCurve(0.0f);
    kid.shape.shape = ShapeType::Box;
    kid.shape.scale = Vector3(0.0f, 0.0f, 0.0f);
    kid.shape.position = Vector3(0.0f, 1.0f, 0.0f);
    std::shared_ptr<const CompiledEffect> child = compileOrFail(kid, env);
    parent.runtime.bindSubEmitters([child](const SubEmitterEntry&){ return child; });
    parent.runtime.reset(1);
    parent.runtime.play();
    parent.ticks(31);
    ASSERT_EQ(parent.count(), 1u);
    ASSERT_EQ(parent.runtime.child(0)->count(), 1u);
    const ParticleChannels& c = parent.runtime.child(0)->particleChannels();
    // the child is in the emitter's space (at x = 5), its parent in the world's
    EXPECT_NEAR(c.get(ChPosX)[0] + 5.0f, parent.ch(ChPosX), 1.0e-3f);
    EXPECT_NEAR(c.get(ChPosY)[0], parent.ch(ChPosY) + 1.0f, 1.0e-3f);
}

TEST(ParticleFollowParent, AMeshParentTurnsThemByItsRotation){
    // review M6: a parent with a 3D rotation (a mesh particle's quaternion) turning about Z
    ParticleEffectDocument d = followParent(Vector3(0.0f, 0.0f, 0.0f), true, false);
    d.renderer.renderMode = RenderMode::Mesh;
    d.renderer.meshes = {MeshSource()};
    d.rotationOverLifetime.present = true;
    d.rotationOverLifetime.angularVelocity = MinMaxCurve(90.0f);
    Follow f(d, d.subEmitters[0], Vector3(1.0f, 0.0f, 0.0f));
    ASSERT_TRUE(f.parent.runtime.program()->has(ChQuatX));
    f.parent.ticks(31);
    ASSERT_EQ(f.kid().count(), 1u);
    const Vector3 kid = f.kidAt();
    // a quarter turn about Z, whichever way the quaternion turns
    EXPECT_NEAR(kid.x, 0.0f, 0.02f);
    EXPECT_NEAR(std::fabs(kid.y), 1.0f, 0.02f);
    EXPECT_NEAR(kid.z, 0.0f, 0.02f);
}

TEST(ParticleFollowParent, IsStructureOfTheEntry){
    // the entry's Follow flags compile into the child's channels: changing one binds again
    ParticleEffectDocument d = followParent(Vector3(1.0f, 0.0f, 0.0f), false, false);
    Sim sim(d);
    ParticleEffectDocument other = d;
    other.subEmitters[0].followRotation = true;
    EXPECT_FALSE(sim.runtime.rebind(compileOrFail(other)));
    other = d;
    other.subEmitters[0].probability = 0.5f;
    EXPECT_TRUE(sim.runtime.rebind(compileOrFail(other)));
}

// ---------------------------------------------------------------- R13: Sound Events

TEST(ParticleSoundEvents, BurstsEntriesAndTriggersAskForTheirSounds){
    ParticleEffectDocument d = readOrFail(R"({"version": 1, "duration": 5, "looping": false, "tickRate": 30, "maxParticles": 100,
        "simulationSpace": "world",
        "triggers": [{"name": "ring", "action": "none", "soundEvent": "fx/bell.soundevent"}],
        "subEmitters": [{"trigger": "death", "effect": "child.particles", "soundEvent": "fx/pop.soundevent"}],
        "modules": [{"type": "main", "startLifetime": 0.2, "startSpeed": 0},
                    {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 2, "soundEvent": "fx/boom.soundevent"}]},
                    {"type": "shape", "enabled": false},
                    {"type": "subEmitters"}]})");
    Sim sim(d);
    std::shared_ptr<const CompiledEffect> child = stillChild();
    sim.runtime.bindSubEmitters([child](const SubEmitterEntry&){ return child; });
    sim.runtime.reset(1);
    sim.runtime.play();
    sim.moveTo(2.0f, 0.0f, 0.0f);
    sim.ticks(1);
    ASSERT_EQ(sim.runtime.soundEvents().size(), 1u);
    EXPECT_EQ(sim.runtime.soundEvents()[0].source, ParticleSoundEvent::Source::Burst);
    EXPECT_TRUE(sim.runtime.soundEvents()[0].atEmitter);
    sim.runtime.takeSoundEvents();
    sim.ticks(10);
    // both particles died: the entry's sound, at each of them
    ASSERT_EQ(sim.runtime.soundEvents().size(), 2u);
    for (const ParticleSoundEvent& e : sim.runtime.soundEvents()){
        EXPECT_EQ(e.source, ParticleSoundEvent::Source::SubEmitter);
        EXPECT_FALSE(e.atEmitter);
        EXPECT_NEAR(e.position[0], 2.0f, 1.0e-4f);
    }
    sim.runtime.takeSoundEvents();
    sim.runtime.sendTrigger(nameHash("ring"));
    ASSERT_EQ(sim.runtime.soundEvents().size(), 1u);
    EXPECT_EQ(sim.runtime.soundEvents()[0].source, ParticleSoundEvent::Source::Trigger);
    sim.runtime.takeSoundEvents();
    // a seek asks for none
    sim.runtime.seek(sim.input, 1.0);
    EXPECT_TRUE(sim.runtime.soundEvents().empty());
    // nor does an effect without sounds
    Sim quiet(singleParticle(Vector3(0.0f, 0.0f, 0.0f)));
    quiet.ticks(3);
    EXPECT_TRUE(quiet.runtime.soundEvents().empty());
}

TEST(ParticleSoundEvents, TheSystemPlaysThem){
    // through Audio.play's free voices, caused by "particles" (a Null mix: the play is seen, an
    // event without a file is refused there)
    std::vector<AudioVoiceLine> lines;
    const AudioBackend backend = AudioSystem::getBackend();
    AudioSystem::close();
    AudioSystem::setBackend(AudioBackend::Null);
    AudioSystem::setVoiceObserver([&lines](const AudioVoiceLine& line){ lines.push_back(line); });
    {
        Scene scene;
        ParticleSystem system(&scene);
        ASSERT_TRUE(system.setInlineEffect(R"({"version": 1, "duration": 5, "looping": false, "tickRate": 30, "maxParticles": 10,
            "modules": [{"type": "main", "startLifetime": 1, "startSpeed": 0},
                        {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 1, "soundEvent": "fx/missing-boom.soundevent"}]}]})"));
        ParticleSubsystem& subsystem = *scene.getSystem<ParticleSubsystem>();
        for (int i = 0; i < 3; i++) subsystem.update(1.0 / 30.0);
    }
    AudioSystem::setVoiceObserver(nullptr);
    AudioSystem::close();
    AudioSystem::setBackend(backend);
    bool asked = false;
    for (const AudioVoiceLine& line : lines)
        asked = asked || (std::string(line.cause) == "particles" && std::string(line.reason) == "missing_event");
    EXPECT_TRUE(asked) << lines.size() << " voice lines";
}

TEST(ParticleSoundEvents, ATriggersSoundPlaysWhenTheSystemDoesNotTick){
    // review M4: at a time scale of 0 (or culled) the system runs no tick, and the sound its trigger
    // asked for plays all the same, not when it next ticks
    std::vector<AudioVoiceLine> lines;
    const AudioBackend backend = AudioSystem::getBackend();
    AudioSystem::close();
    AudioSystem::setBackend(AudioBackend::Null);
    AudioSystem::setVoiceObserver([&lines](const AudioVoiceLine& line){ lines.push_back(line); });
    {
        Scene scene;
        ParticleSystem system(&scene);
        ASSERT_TRUE(system.setInlineEffect(R"({"version": 1, "duration": 5, "tickRate": 30, "maxParticles": 10,
            "triggers": [{"name": "ring", "action": "none", "soundEvent": "fx/missing-bell.soundevent"}],
            "modules": [{"type": "main", "startLifetime": 1, "startSpeed": 0}, {"type": "emission", "rateOverTime": 5}]})"));
        ParticleSubsystem& subsystem = *scene.getSystem<ParticleSubsystem>();
        subsystem.update(1.0 / 30.0);
        system.sendTrigger("ring");
        subsystem.update(0.0);
    }
    AudioSystem::setVoiceObserver(nullptr);
    AudioSystem::close();
    AudioSystem::setBackend(backend);
    bool asked = false;
    for (const AudioVoiceLine& line : lines)
        asked = asked || (std::string(line.cause) == "particles" && std::string(line.reason) == "missing_event");
    EXPECT_TRUE(asked) << lines.size() << " voice lines";
}

// ---------------------------------------------------------------- R8: Ring

TEST(ParticleRing, ItsMeshCoversTheArcBetweenTheRadii){
    ParticleMeshData ring;
    ParticleMeshes::ring(0.5f, 0.25f, 180.0f, 16, ring);
    // half of 16 segments: 9 pairs of vertices, two triangles a segment
    EXPECT_EQ(ring.vertexCount(), 18u);
    EXPECT_EQ(ring.indices.size(), 8u * 6u);
    for (uint32_t v = 0; v < ring.vertexCount(); v++){
        const float* p = &ring.vertices[(size_t)v * ParticleMeshData::kFloatsPerVertex];
        const float r = std::sqrt(p[0] * p[0] + p[1] * p[1]);
        EXPECT_NEAR(r, (v % 2) ? 0.25f : 0.5f, 1.0e-5f);
        EXPECT_GE(p[1], -1.0e-5f) << "the upper half";
        EXPECT_EQ(p[5], 1.0f) << "facing +Z";
    }
    // U along the arc, V from the outer edge to the inner
    for (uint32_t v = 0; v < ring.vertexCount(); v++){
        const float* p = &ring.vertices[(size_t)v * ParticleMeshData::kFloatsPerVertex];
        EXPECT_NEAR(p[6], (float)(v / 2) / 8.0f, 1.0e-6f);
        EXPECT_EQ(p[7], (v % 2) ? 1.0f : 0.0f);
    }
    // a whole ring closes on a seam: its last pair where its first is, at U 1
    ParticleMeshData full;
    ParticleMeshes::ring(1.0f, 0.5f, 360.0f, 12, full);
    ASSERT_EQ(full.vertexCount(), 26u);
    const float* first = &full.vertices[0];
    const float* last = &full.vertices[(size_t)24 * ParticleMeshData::kFloatsPerVertex];
    EXPECT_NEAR(last[0], first[0], 1.0e-5f);
    EXPECT_NEAR(last[1], first[1], 1.0e-5f);
    EXPECT_EQ(last[6], 1.0f);
    // a Ring draws as a mesh particle: one mesh, 3D rotations, no new channel otherwise
    ParticleEffectDocument d = singleParticle(Vector3(0.0f, 0.0f, 0.0f));
    d.renderer.renderMode = RenderMode::Ring;
    const auto effect = compileOrFail(d);
    EXPECT_EQ(effect->program.render.mesh, 1u);
    EXPECT_EQ(effect->program.render.meshCount, 1u);
    EXPECT_TRUE(effect->program.has(ChQuatX));
    d.renderer.ringInnerRadius = 2.0f;
    std::vector<Diagnostic> diagnostics;
    EXPECT_FALSE(ParticleEffectCodec::validate(d, diagnostics)) << "the inner radius is not above the outer";
    // appended: the old values keep their meaning
    EXPECT_EQ((int)RenderMode::VelocityAligned, 9);
    EXPECT_EQ((int)RenderMode::Ring, 10);
}
