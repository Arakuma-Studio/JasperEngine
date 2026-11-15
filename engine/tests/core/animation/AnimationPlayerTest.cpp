// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation Player (spec 3.2-3.6, 9.R1/R2/R9/R10/R14-R19/R21-R25/R42-R44) through
// ActionSystem::update: play / crossFade / blend / queue / stop / rewind / sample timelines,
// wrap modes, the rest term of the blend, rest kept across rebinding and clip reloads, relative
// tracks, rebinding after renames, model-load deferral, events and onFinish (C++ and Lua),
// cloning, Model:playAnimation, edit-mode preview, and the R9 performance budget.

#include <gtest/gtest.h>

#include "AnimTestEnvironment.h"
#include "support/AllocationCounter.h"

#include "Engine.h"
#include "Scene.h"
#include "action/Animation.h"
#include "action/keyframe/TranslateTracks.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "object/AnimationPlayer.h"
#include "object/Model.h"
#include "pool/AnimationClipPool.h"
#include "registry/EntityCloner.h"
#include "script/LuaBinding.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/MeshSystem.h"
#include "animation/ClipBinding.h"

#include "lua.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>    // GetThreadTimes: the thread's CPU time (POSIX: CLOCK_THREAD_CPUTIME_ID)
#endif

using namespace jasperengine;

namespace {

    ClipKey key(float time, std::vector<float> value, EaseType ease = EaseType::LINEAR){
        ClipKey k;
        k.time = time;
        k.value = std::move(value);
        k.ease = ease;
        return k;
    }

    ClipTrackData track(const std::string& path, ClipChannel channel, std::vector<ClipKey> keys, bool relative = false){
        ClipTrackData t;
        t.path = path;
        t.channel = channel;
        t.relative = relative;
        t.keys = std::move(keys);
        return t;
    }

    // position x goes 0 -> `x` over 1 s on `path`
    ClipData slide(const std::string& path, float x, ClipWrapMode wrap = ClipWrapMode::Once, float length = -1.0f){
        ClipData data;
        data.wrap = wrap;
        data.length = length;
        data.tracks = {track(path, ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {x, 0, 0})})};
        return data;
    }

    ClipEventData event(float time, const std::string& function, std::vector<AnimationEventArg> args = {}){
        ClipEventData e;
        e.time = time;
        e.function = function;
        e.args = std::move(args);
        return e;
    }

    // every test writes its own files: generations outlive the pool's clear()
    class Player : public ::testing::Test {
    protected:
        void SetUp() override {
            static int run = 0;
            prefix = "players/r" + std::to_string(run++) + "/";
            AnimationClipPool::clear();
            root = scene.createEntity();
            scene.setEntityName(root, "Hero");
            scene.addComponent<Transform>(root, {});
            arm = child(root, "Arm", Vector3(5, 0, 0));
            hand = child(arm, "Hand", Vector3(0, 7, 0));
        }
        void TearDown() override { AnimationClipPool::clear(); }

        Entity child(Entity parent, const std::string& name, Vector3 position = Vector3(0, 0, 0)){
            Entity entity = scene.createEntity();
            scene.setEntityName(entity, name);
            Transform transform;
            transform.position = position;
            scene.addComponent<Transform>(entity, transform);
            scene.addEntityChild(parent, entity, false);
            return entity;
        }

        // writes the clip and returns its asset-relative path
        std::string clip(const std::string& name, const ClipData& data){
            animtest::writeAsset(prefix + name + ".anim", anim::writeClip(data));
            return prefix + name + ".anim";
        }

        AnimationPlayer player(std::vector<std::pair<std::string, ClipData>> clips, bool playAutomatically = false, Entity on = NULL_ENTITY){
            AnimationPlayer object(&scene, on == NULL_ENTITY ? root : on);
            for (auto& entry : clips){
                object.addClip(clip(entry.first, entry.second), entry.first);
            }
            object.setPlayAutomatically(playAutomatically);
            return object;
        }

        void step(double dt, int count = 1){
            for (int i = 0; i < count; i++){
                scene.getSystem<ActionSystem>()->update(dt);
            }
        }

        Vector3 position(Entity entity){ return scene.getComponent<Transform>(entity).position; }
        float x(Entity entity){ return position(entity).x; }

        Scene scene;
        std::string prefix;
        Entity root = NULL_ENTITY;
        Entity arm = NULL_ENTITY;
        Entity hand = NULL_ENTITY;
    };

}

// S77 R16: sample() shows its pose in the call, as without async loading
TEST_F(Player, SampleLoadsAnAnimInTheCallInAsyncLoading){
    // large enough for the thread pool otherwise
    animtest::writeAsset(prefix + "slide.anim", anim::writeClip(slide("Arm", 3.0f)) + std::string(70000, ' '));
    AnimationPlayer object(&scene, root);
    object.addClip(prefix + "slide.anim", "slide");
    const bool wasAsync = Engine::isAsyncLoading();
    Engine::setAsyncLoading(true);
    object.play("slide");
    object.getState("slide").setTime(0.5f);
    object.sample();
    Engine::setAsyncLoading(wasAsync);
    EXPECT_NEAR(x(arm), 1.5f, 1e-5f);
}

// AC8 (S77 R17): an idle player, one that never played and one holding its last pose, allocates
// nothing per frame and marks nothing dirty
TEST_F(Player, IdleAllocatesNothingAndMarksNothingDirty){
    if (!testsupport::countsAllocations()) GTEST_SKIP() << "this build cannot count allocations";
    constexpr double kIdleDt = 1.0 / 60.0;
    AnimationPlayer idle = player({{"slide", slide("Arm", 3.0f)}});
    step(kIdleDt, 3);
    {
        testsupport::AllocationCountScope scope;
        step(kIdleDt, 600);
        EXPECT_EQ(scope.count(), 0u) << "never played";
    }
    scene.getComponent<Transform>(arm).needUpdate = false;
    step(kIdleDt);
    EXPECT_FALSE(scene.getComponent<Transform>(arm).needUpdate);

    idle.play("slide");
    step(kIdleDt);
    EXPECT_TRUE(scene.getComponent<Transform>(arm).needUpdate) << "a value that changes marks it";
    step(1.0, 2);
    const Vector3 last = scene.getComponent<Transform>(arm).position;
    EXPECT_NEAR(last.x, 3.0f, 1e-5f);
    {
        testsupport::AllocationCountScope scope;
        step(kIdleDt, 600);
        EXPECT_EQ(scope.count(), 0u) << "holding its last pose";
    }
    scene.getComponent<Transform>(arm).needUpdate = false;
    step(kIdleDt);
    EXPECT_FALSE(scene.getComponent<Transform>(arm).needUpdate) << "the held value did not change";
    EXPECT_EQ(scene.getComponent<Transform>(arm).position, last);
}

TEST_F(Player, PlaysAutomaticallyAndSamplesTheClip){
    AnimationPlayer object = player({{"walk", slide("Arm", 10)}}, true);
    EXPECT_FALSE(object.isPlaying());
    step(0.0);
    EXPECT_TRUE(object.isPlaying("walk"));
    EXPECT_NEAR(x(arm), 0.0f, 1e-5f);
    step(0.25);
    EXPECT_NEAR(x(arm), 2.5f, 1e-5f);
    EXPECT_TRUE(object.isLoaded());
    AnimationStateInfo info = object.getStateInfo("walk");
    EXPECT_TRUE(info.loaded);
    EXPECT_NEAR(info.time, 0.25f, 1e-6f);
    EXPECT_NEAR(info.length, 1.0f, 1e-6f);
    EXPECT_EQ(info.boundTracks, 1u);
    EXPECT_EQ(info.trackCount, 1u);
    EXPECT_EQ(info.wrap, ClipWrapMode::Once);

    // determinism: the pose is a function of the time only
    AnimationState state = object.getState("walk");
    state.setTime(0.8f);
    object.sample();
    EXPECT_NEAR(x(arm), 8.0f, 1e-5f) << "sample() outside the update writes at once";
    state.setTime(0.1f);
    step(0.0);
    EXPECT_NEAR(x(arm), 1.0f, 1e-5f);
}

// R40: an authored list with duplicate state names plays the later ones as "<name> 2" and says
// so once per player (again only when the list changes)
TEST_F(Player, DuplicateStateNamesWarnOncePerPlayer){
    AnimationPlayer object = player({{"walk", slide("Arm", 10)}});
    AnimationPlayerComponent& component = scene.getComponent<AnimationPlayerComponent>(root);
    AnimationPlayerClip copy = component.clips[0];
    component.clips.push_back(copy);
    component.needsLoad = true;
    auto duplicateWarnings = [](){
        size_t count = 0;
        for (const std::string& warning : animtest::takeWarnings()){
            if (warning.find("duplicate state names") != std::string::npos){
                EXPECT_NE(warning.find("Animation Player 'Hero'"), std::string::npos) << warning;
                EXPECT_NE(warning.find("'walk' (clip 2) plays as 'walk 2'"), std::string::npos) << warning;
                count++;
            }
        }
        return count;
    };
    animtest::takeWarnings();
    step(0.0);
    EXPECT_EQ(duplicateWarnings(), 1u);
    step(0.1, 5);
    EXPECT_EQ(duplicateWarnings(), 0u) << "once per player";
    EXPECT_EQ(object.getClipName(1), "walk 2");
    object.play("walk 2");
    step(0.5);
    EXPECT_TRUE(object.isPlaying("walk 2"));

    // an edit of the list that keeps the duplicate: said again for the new list; none left: quiet
    component.clips.push_back(copy);
    component.needsLoad = true;
    step(0.0);
    EXPECT_EQ(duplicateWarnings(), 1u);
    component.clips.resize(1);
    component.needsLoad = true;
    step(0.0, 3);
    EXPECT_EQ(duplicateWarnings(), 0u);

    // another player with duplicates says it for itself
    Entity other = child(root, "Other");
    AnimationPlayer second(&scene, other);
    AnimationPlayerComponent& secondComponent = scene.getComponent<AnimationPlayerComponent>(other);
    secondComponent.clips = {copy, copy};
    secondComponent.needsLoad = true;
    step(0.0);
    std::vector<std::string> warnings = animtest::takeWarnings();
    size_t forOther = 0;
    for (const std::string& warning : warnings){
        if (warning.find("Animation Player 'Other': duplicate state names") != std::string::npos) forOther++;
    }
    EXPECT_EQ(forOther, 1u);
}

TEST_F(Player, DefaultClipAndNoAutoplay){
    AnimationPlayer object = player({{"idle", slide("Arm", 1)}, {"walk", slide("Arm", 10)}}, true);
    object.setDefaultClip("walk");
    step(0.5);
    EXPECT_TRUE(object.isPlaying("walk"));
    EXPECT_FALSE(object.isPlaying("idle"));

    Entity other = child(root, "Other");
    AnimationPlayer quiet = player({{"a", slide("", 3)}}, false, other);
    step(0.5);
    EXPECT_FALSE(quiet.isPlaying());
    EXPECT_NEAR(x(other), 0.0f, 1e-6f) << "a player that plays nothing writes nothing";
}

TEST_F(Player, OnceStopsAtItsEndAndKeepsThePose){
    AnimationPlayer object = player({{"wave", slide("Arm", 10)}});
    std::vector<std::string> finished;
    object.getOnFinish().add("test", std::function<void(const std::string&)>([&finished](const std::string& state){
        finished.push_back(state);
    }));
    object.play("wave");
    step(0.5);
    step(0.75);
    EXPECT_NEAR(x(arm), 10.0f, 1e-5f) << "the last update samples the end";
    EXPECT_FALSE(object.isPlaying());
    EXPECT_EQ(finished, std::vector<std::string>{"wave"});
    // R1b: an ended once state holds its last pose at its weight, not playing
    scene.getComponent<Transform>(arm).position.x = 3.0f;
    step(0.5);
    EXPECT_NEAR(x(arm), 10.0f, 1e-6f) << "held at its end";
    EXPECT_FALSE(object.isPlaying("wave"));
    EXPECT_NEAR(object.getState("wave").getTime(), 1.0f, 1e-6f) << "held at its end time";
    EXPECT_EQ(finished.size(), 1u);
    // stop(name) releases it: rewound, the pose kept, nothing written
    object.stop("wave");
    EXPECT_NEAR(object.getState("wave").getTime(), 0.0f, 1e-6f);
    scene.getComponent<Transform>(arm).position.x = 3.0f;
    step(0.5);
    EXPECT_NEAR(x(arm), 3.0f, 1e-6f) << "a released state no longer writes";
    // play() of a held state restarts it from 0
    object.play("wave");
    step(1.5);
    object.play("wave");
    step(0.25);
    EXPECT_NEAR(x(arm), 2.5f, 1e-5f);
    EXPECT_TRUE(object.isPlaying("wave"));
}

// R1b, the review's probe: onFinish -> crossFade("idle") blends from the end pose (10) to the
// idle (12) with no dip toward the rest (5)
TEST_F(Player, OnFinishCrossFadeDoesNotDipTowardRest){
    ClipData attack = slide("Arm", 10);
    ClipData idle = slide("Arm", 0, ClipWrapMode::Loop);
    idle.tracks[0].keys[0].value = {12, 0, 0};
    idle.tracks[0].keys[1].value = {12, 0, 0};
    AnimationPlayer object = player({{"attack", attack}, {"idle", idle}});
    object.getOnFinish().add("test", std::function<void(const std::string&)>([&object](const std::string& state){
        if (state == "attack") object.crossFade("idle");
    }));
    object.play("attack");
    step(0.0);
    float lowest = 100.0f;
    float previous = 0.0f;
    bool monotonic = true;
    for (int i = 0; i < 60; i++){
        step(1.0 / 30.0);
        if (i > 30){
            lowest = std::min(lowest, x(arm));
            if (x(arm) < previous - 1e-4f) monotonic = false;
        }
        previous = x(arm);
    }
    EXPECT_GE(lowest, 10.0f - 1e-4f) << "no dip toward the rest";
    EXPECT_TRUE(monotonic);
    EXPECT_NEAR(x(arm), 12.0f, 1e-4f);
    EXPECT_FALSE(object.isPlaying("attack"));
    EXPECT_NEAR(object.getState("attack").getWeight(), 0.0f, 1e-6f) << "faded out, then released";
}

// R1b: a crossfade (or a queue with a fade) issued shortly before a once state ends keeps its pose
// while it fades: no dip toward the rest (0 here)
TEST_F(Player, CrossFadeNearTheEndOfAOnceStateDoesNotDip){
    ClipData a = slide("Arm", 0);
    a.tracks[0].keys[0].value = {10, 0, 0};
    a.tracks[0].keys[1].value = {10, 0, 0};
    ClipData b = a;
    b.tracks[0].keys[0].value = {20, 0, 0};
    b.tracks[0].keys[1].value = {20, 0, 0};
    for (bool queued : {false, true}){
        Entity rig = child(root, queued ? "Queued" : "Crossfaded");
        Entity limb = child(rig, "Arm", Vector3(0, 0, 0));
        AnimationPlayer object = player({{"a", a}, {"b", b}}, false, rig);
        object.play("a");
        step(0.0);
        step(0.9);
        if (queued) object.queue("b", 0.5f);
        else object.crossFade("b", 0.5f);
        float lowest = 100.0f;
        for (int i = 0; i < 30; i++){
            step(1.0 / 30.0);
            lowest = std::min(lowest, x(limb));
        }
        EXPECT_GE(lowest, 10.0f - 1e-3f) << (queued ? "queue" : "crossFade");
        EXPECT_NEAR(x(limb), 20.0f, 1e-3f) << (queued ? "queue" : "crossFade");
    }
}

// R17 / I1: the playing state queued again with a fade restarts at its end
TEST_F(Player, QueueingThePlayingStateWithAFadeRestartsIt){
    AnimationPlayer object = player({{"wave", slide("Arm", 10)}, {"loop", slide("Arm", 10, ClipWrapMode::Loop)}});
    object.play("wave");
    step(0.25);
    object.queue("wave", 0.2f);
    step(1.0);
    EXPECT_TRUE(object.isPlaying("wave")) << "once: restarted";
    EXPECT_NEAR(object.getState("wave").getTime(), 0.25f, 1e-4f);
    object.play("loop");
    step(0.5);
    object.queue("loop", 0.2f);
    step(0.75);
    EXPECT_TRUE(object.isPlaying("loop")) << "loop: restarted at the end of its cycle";
    EXPECT_NEAR(object.getState("loop").getTime(), 0.25f, 1e-4f);
}

// I2: a queued crossfade's state fires its events at 0, and the outgoing state fades the same frame
TEST_F(Player, QueuedCrossfadeFiresItsStartEvents){
    ClipData a = slide("Arm", 10);
    ClipData b = slide("Arm", 20);
    b.events = {event(0.0f, "bStart"), event(0.05f, "bEarly")};
    AnimationPlayer object = player({{"a", a}, {"b", b}});
    std::vector<std::string> fired;
    object.getOnEvent().add("test", std::function<void(const AnimationEvent&)>([&](const AnimationEvent& e){
        fired.push_back(e.functionName);
    }));
    object.play("a");
    object.queue("b", 0.3f);
    step(0.5);
    EXPECT_FALSE(object.isPlaying("b"));
    step(0.25);
    EXPECT_TRUE(object.isPlaying("b"));
    EXPECT_GT(object.getState("b").getWeight(), 0.0f) << "faded in for the rest of the update";
    EXPECT_LT(object.getState("a").getWeight(), 1.0f) << "the outgoing state fades the same frame";
    step(0.1);
    EXPECT_EQ(fired, (std::vector<std::string>{"bStart", "bEarly"}));
}

// M1, M2, M7, M8
TEST_F(Player, QueueDetails){
    AnimationPlayer object = player({{"a", slide("Arm", 10)}, {"b", slide("Arm", 4)},
                                     {"back", slide("Arm", 10, ClipWrapMode::Loop)},
                                     {"empty", ClipData()}, {"pong", slide("Arm", 10, ClipWrapMode::PingPong)}});
    // stop(name) clears the queue
    object.play("a");
    object.queue("b");
    object.stop("a");
    step(0.1);
    EXPECT_FALSE(object.isPlaying("b"));

    // a loop played backwards shows its end on its first frame
    object.getState("back").setSpeed(-1.0f);
    object.play("back");
    step(0.0);
    EXPECT_NEAR(x(arm), 10.0f, 1e-5f);

    // a zero-length loop does not hold a queue forever
    AnimationPlayerComponent& component = scene.getComponent<AnimationPlayerComponent>(root);
    component.clips[3].wrap = ClipWrapMode::Loop;
    component.needsLoad = true;
    object.play("empty");
    step(0.1);
    object.queue("b");
    step(0.1);
    EXPECT_TRUE(object.isPlaying("b"));

    // a ping-pong queue waits for the way there and back (the cycle)
    object.play("pong");
    step(0.25);
    object.queue("b");
    step(1.0);
    EXPECT_TRUE(object.isPlaying("pong")) << "turned at 1, the cycle ends at 2";
    step(1.0);
    EXPECT_FALSE(object.isPlaying("pong"));
    EXPECT_TRUE(object.isPlaying("b"));
}

// I3: renames that change what a path selects re-resolve (validate-then-re-resolve)
TEST_F(Player, RenamedSiblingsChangeWhatAPathSelects){
    // [Leg, Arm]: the Leg renamed "Arm" is now the first Arm
    Entity hero = child(root, "Body");
    Entity leg = child(hero, "Leg", Vector3(1, 0, 0));
    Entity arm2 = child(hero, "Arm", Vector3(42, 0, 0));
    AnimationPlayer object = player({{"walk", slide("Arm", 10, ClipWrapMode::Loop)}}, false, hero);
    object.play("walk");
    step(0.0);
    step(0.5);
    EXPECT_NEAR(x(arm2), 5.0f, 1e-5f);
    scene.setEntityName(leg, "Arm");
    step(0.1);
    EXPECT_NEAR(x(leg), 6.0f, 1e-5f) << "the first Arm is animated now";
    scene.getComponent<Transform>(arm2).position.x = 42.0f;
    step(0.1);
    EXPECT_NEAR(x(arm2), 42.0f, 1e-6f) << "the former one is no longer written";

    // [Arm, Arm] with "Arm#1": the first renamed away, nothing is the second Arm any more
    Entity pair = child(root, "Pair");
    Entity first = child(pair, "Arm", Vector3(1, 0, 0));
    Entity second = child(pair, "Arm", Vector3(42, 0, 0));
    AnimationPlayer other = player({{"reach", slide("Arm#1", 10, ClipWrapMode::Loop)}}, false, pair);
    other.play("reach");
    step(0.0);
    step(0.5);
    EXPECT_NEAR(x(second), 5.0f, 1e-5f);
    EXPECT_NEAR(x(first), 1.0f, 1e-6f);
    scene.setEntityName(first, "Shoulder");
    scene.getComponent<Transform>(second).position.x = 42.0f;
    step(0.1);
    EXPECT_NEAR(x(second), 42.0f, 1e-6f) << "Arm#1 no longer resolves";
    EXPECT_EQ(other.getStateInfo("reach").boundTracks, 0u);
}

TEST_F(Player, WrapModes){
    AnimationPlayer object = player({{"loop", slide("Arm", 10, ClipWrapMode::Loop)},
                                     {"pong", slide("Arm/Hand", 10, ClipWrapMode::PingPong)},
                                     {"clamp", slide("", 10, ClipWrapMode::ClampForever)}});
    std::vector<std::string> finished;
    object.getOnFinish().add("test", std::function<void(const std::string&)>([&finished](const std::string& state){
        finished.push_back(state);
    }));
    object.blend("loop", 1.0f, 0.0f);
    object.blend("pong", 1.0f, 0.0f);
    object.blend("clamp", 1.0f, 0.0f);
    step(0.0);
    step(1.25);
    EXPECT_NEAR(x(arm), 2.5f, 1e-4f);
    EXPECT_NEAR(x(hand), 7.5f, 1e-4f);
    EXPECT_NEAR(x(root), 10.0f, 1e-4f);
    EXPECT_EQ(finished, std::vector<std::string>{"clamp"}) << "clamp_forever reports its end once, loops never";
    step(1.0);
    EXPECT_NEAR(x(arm), 2.5f, 1e-4f);
    EXPECT_NEAR(x(hand), 2.5f, 1e-4f);
    EXPECT_TRUE(object.isPlaying("clamp"));
    EXPECT_EQ(finished.size(), 1u);
    // a long run keeps its precision: the stored time stays within two lengths
    step(1000.25);
    EXPECT_NEAR(object.getState("loop").getTime(), 0.5f, 1e-3f);

    // the entry's wrap overrides the clip's (R14), the Lua state.wrap both
    AnimationPlayerComponent& component = scene.getComponent<AnimationPlayerComponent>(root);
    component.clips[0].wrap = ClipWrapMode::Once;
    component.needsLoad = true;
    EXPECT_EQ(object.getState("loop").getWrap(), "once");
    object.getState("loop").setWrap("ping_pong");
    EXPECT_EQ(object.getState("loop").getWrap(), "ping_pong");
    object.getState("loop").setWrap("default");
    EXPECT_EQ(object.getState("loop").getWrap(), "once");
}

TEST_F(Player, BackwardsPlayStartsAtTheEnd){
    AnimationPlayer object = player({{"wave", slide("Arm", 10)}});
    object.getState("wave").setSpeed(-1.0f);
    object.play("wave");
    step(0.25);
    EXPECT_NEAR(x(arm), 7.5f, 1e-5f);
    step(1.0);
    EXPECT_NEAR(x(arm), 0.0f, 1e-5f);
    EXPECT_FALSE(object.isPlaying());
    // the component's speed multiplies
    object.setSpeed(-2.0f);
    object.getState("wave").setSpeed(0.5f);
    object.play("wave");
    step(0.25);
    EXPECT_NEAR(x(arm), 7.5f, 1e-5f);
}

// R1: Σwᵢvᵢ + (1 - Σwᵢ)·rest
TEST_F(Player, BlendFillsWithTheRestValue){
    AnimationPlayer object = player({{"walk", slide("Arm", 10)}});
    object.blend("walk", 0.5f, 0.0f);
    object.getState("walk").setTime(1.0f);
    object.getState("walk").setSpeed(0.0f);
    step(0.1);
    // rest 5 (the scene value), sample 10
    EXPECT_NEAR(x(arm), 0.5f * 10.0f + 0.5f * 5.0f, 1e-5f);
    object.blend("walk", 0.25f, 0.0f);
    step(0.1);
    EXPECT_NEAR(x(arm), 0.25f * 10.0f + 0.75f * 5.0f, 1e-5f) << "the rest is the scene value, not last frame's";
    // over 1 the weights normalize
    object.blend("walk", 2.0f, 0.0f);
    step(0.1);
    EXPECT_NEAR(x(arm), 10.0f, 1e-5f);
}

TEST_F(Player, CrossFadeTimeline){
    ClipData a = slide("Arm", 10);
    a.tracks[0].keys[0].value = {10, 0, 0};    // constant 10
    ClipData b = slide("Arm", 0);
    b.tracks[0].keys[0].value = {20, 0, 0};    // constant 20
    b.tracks[0].keys[1].value = {20, 0, 0};
    a.wrap = ClipWrapMode::Loop;
    b.wrap = ClipWrapMode::Loop;
    AnimationPlayer object = player({{"a", a}, {"b", b}});
    object.play("a");
    step(0.1);
    EXPECT_NEAR(x(arm), 10.0f, 1e-5f);
    object.crossFade("b", 0.4f);
    step(0.1);
    EXPECT_NEAR(object.getState("a").getWeight(), 0.75f, 1e-5f);
    EXPECT_NEAR(object.getState("b").getWeight(), 0.25f, 1e-5f);
    EXPECT_NEAR(x(arm), 0.75f * 10.0f + 0.25f * 20.0f, 1e-4f);
    step(0.1, 3);
    EXPECT_FALSE(object.isPlaying("a")) << "the faded-out state stops";
    EXPECT_TRUE(object.isPlaying("b"));
    EXPECT_NEAR(x(arm), 20.0f, 1e-4f);
    // crossFade without a fade time uses defaultFadeTime (0.25)
    object.crossFade("a");
    step(0.125);
    EXPECT_NEAR(object.getState("a").getWeight(), 0.5f, 1e-4f);
    // play() is instant
    object.play("b");
    step(0.01);
    EXPECT_FALSE(object.isPlaying("a"));
    EXPECT_NEAR(x(arm), 20.0f, 1e-4f);
}

// a fade-in from nothing starts at the rest pose, not with a snap (R1)
TEST_F(Player, FadeInStartsFromRest){
    ClipData a = slide("Arm", 0);
    a.tracks[0].keys[0].value = {25, 0, 0};
    a.tracks[0].keys[1].value = {25, 0, 0};
    AnimationPlayer object = player({{"a", a}});
    object.crossFade("a", 1.0f);
    step(0.1);
    EXPECT_NEAR(x(arm), 0.1f * 25.0f + 0.9f * 5.0f, 1e-4f);
}

// two clips on disjoint bones don't drag each other to rest; a state at weight 0 writes nothing
TEST_F(Player, DisjointClipsDoNotPullEachOther){
    AnimationPlayer object = player({{"arm", slide("Arm", 10)}, {"hand", slide("Arm/Hand", 4)}});
    object.blend("arm", 1.0f, 0.0f);
    object.blend("hand", 1.0f, 0.0f);
    step(0.0);
    step(0.5);
    EXPECT_NEAR(x(arm), 5.0f, 1e-5f);
    EXPECT_NEAR(x(hand), 2.0f, 1e-5f);
    object.blend("hand", 0.0f, 0.0f);
    scene.getComponent<Transform>(hand).position.x = -3.0f;
    step(0.1);
    EXPECT_NEAR(x(hand), -3.0f, 1e-6f) << "an entry no state with weight animates is not written";
}

// an entity animation fading out while the player fades in: no jump at the first frame (R1)
TEST_F(Player, CrossfadeFromAnEntityAnimationIsContinuous){
    TranslateTracks tracks(&scene, {0.0f, 1.0f}, {Vector3(30, 0, 0), Vector3(30, 0, 0)});
    tracks.setTarget(arm);
    Animation entityAnimation(&scene);
    entityAnimation.addActionFrame(0.0f, 1000.0f, tracks.getEntity(), arm);
    entityAnimation.setLoop(true);
    entityAnimation.start();
    step(0.1, 2);
    EXPECT_NEAR(x(arm), 30.0f, 1e-4f);

    ClipData a = slide("Arm", 0);
    a.tracks[0].keys[0].value = {10, 0, 0};
    a.tracks[0].keys[1].value = {10, 0, 0};
    AnimationPlayer object = player({{"a", a}});
    entityAnimation.fadeOut(1.0f);
    object.crossFade("a", 1.0f);
    step(0.1);
    // entity 0.9 · 30 + player 0.1 · 10, W = 1: no rest term
    EXPECT_NEAR(x(arm), 0.9f * 30.0f + 0.1f * 10.0f, 0.05f);
}

TEST_F(Player, QueuePlaysAfterTheOthersFinish){
    AnimationPlayer object = player({{"a", slide("Arm", 10)}, {"b", slide("Arm/Hand", 4)}});
    std::vector<std::string> finished;
    object.getOnFinish().add("test", std::function<void(const std::string&)>([&finished](const std::string& state){
        finished.push_back(state);
    }));
    object.play("a");
    object.queue("b");
    step(0.5);
    EXPECT_TRUE(object.isPlaying("a"));
    EXPECT_FALSE(object.isPlaying("b"));
    step(0.75);
    // a ended 0.5 s into the update: b played the other 0.25 s
    EXPECT_FALSE(object.isPlaying("a"));
    EXPECT_TRUE(object.isPlaying("b"));
    EXPECT_NEAR(object.getState("b").getTime(), 0.25f, 1e-5f);
    EXPECT_NEAR(x(hand), 1.0f, 1e-5f);
    step(1.0);
    EXPECT_EQ(finished, (std::vector<std::string>{"a", "b"}));

    // a loop finishes its current cycle first
    AnimationPlayer looping = player({{"l", slide("Arm", 10, ClipWrapMode::Loop)}, {"c", slide("Arm", 2)}});
    looping.play("l");
    step(0.0);
    step(1.5);
    looping.queue("c");
    step(0.25);
    EXPECT_TRUE(looping.isPlaying("l"));
    step(0.5);
    EXPECT_FALSE(looping.isPlaying("l"));
    EXPECT_TRUE(looping.isPlaying("c"));
    EXPECT_NEAR(looping.getState("c").getTime(), 0.25f, 1e-4f);

    // queueing the playing state restarts it at its end; nothing playing plays now; play clears the queue
    looping.queue("c");
    step(1.0);
    EXPECT_TRUE(looping.isPlaying("c"));
    EXPECT_NEAR(looping.getState("c").getTime(), 0.25f, 1e-4f);
    looping.stop();
    looping.queue("l");
    EXPECT_TRUE(looping.isPlaying("l"));
    looping.queue("c");
    looping.play("l");
    step(1.25);
    EXPECT_TRUE(looping.isPlaying("l"));
    EXPECT_FALSE(looping.isPlaying("c"));
}

TEST_F(Player, QueueWithAFadeStartsBeforeTheEnd){
    ClipData a = slide("Arm", 0);
    a.tracks[0].keys[0].value = {10, 0, 0};
    a.tracks[0].keys[1].value = {10, 0, 0};
    ClipData b = a;
    b.tracks[0].keys[0].value = {20, 0, 0};
    b.tracks[0].keys[1].value = {20, 0, 0};
    AnimationPlayer object = player({{"a", a}, {"b", b}});
    object.play("a");
    object.queue("b", 0.5f);
    step(0.25);
    EXPECT_FALSE(object.isPlaying("b"));
    step(0.25);
    EXPECT_TRUE(object.isPlaying("b")) << "0.5 s before a's end";
    step(0.25);
    EXPECT_GT(object.getState("b").getWeight(), 0.0f);
    EXPECT_GT(x(arm), 10.0f);
    EXPECT_LT(x(arm), 20.0f);
    step(0.5);
    EXPECT_FALSE(object.isPlaying("a"));
    EXPECT_NEAR(x(arm), 20.0f, 1e-4f);
}

TEST_F(Player, StopRewindAndUnknownNames){
    AnimationPlayer object = player({{"walk", slide("Arm", 10, ClipWrapMode::Loop)}});
    object.play("walk");
    step(0.5);
    object.stop();
    EXPECT_FALSE(object.isPlaying());
    EXPECT_NEAR(object.getState("walk").getTime(), 0.0f, 1e-6f);
    EXPECT_NEAR(x(arm), 5.0f, 1e-5f) << "stop keeps the pose";
    object.play("walk");
    step(0.5);
    object.rewind("walk");
    step(0.0);
    EXPECT_NEAR(x(arm), 0.0f, 1e-5f);
    object.stop("walk");
    EXPECT_FALSE(object.isPlaying("walk"));

    object.play("nothing");
    object.crossFade("nothing", 0.2f);
    EXPECT_FALSE(object.isPlaying());
    EXPECT_FALSE(object.getState("nothing").isValid());
    EXPECT_FALSE(object.getStateInfo("nothing").loaded);

    // a paused state keeps its time (Unity's enabled)
    object.play("walk");
    step(0.25);
    object.getState("walk").setEnabled(false);
    step(0.5);
    EXPECT_NEAR(object.getState("walk").getTime(), 0.25f, 1e-5f);
    object.getState("walk").setEnabled(true);
    step(0.25);
    EXPECT_NEAR(object.getState("walk").getTime(), 0.5f, 1e-5f);
    object.getState("walk").setNormalizedTime(0.75f);
    EXPECT_NEAR(object.getState("walk").getNormalizedTime(), 0.75f, 1e-5f);
}

TEST_F(Player, DisabledOrInactiveStopsTime){
    AnimationPlayer object = player({{"walk", slide("Arm", 10)}});
    object.play("walk");
    step(0.25);
    object.setEnabled(false);
    scene.getComponent<Transform>(arm).position.x = -1.0f;
    step(0.25);
    EXPECT_NEAR(x(arm), -1.0f, 1e-6f);
    EXPECT_NEAR(object.getState("walk").getTime(), 0.25f, 1e-6f);
    object.setEnabled(true);
    scene.setEntityActive(root, false);
    step(0.25);
    EXPECT_NEAR(object.getState("walk").getTime(), 0.25f, 1e-6f);
    scene.setEntityActive(root, true);
    step(0.25);
    EXPECT_NEAR(x(arm), 5.0f, 1e-5f);
}

TEST_F(Player, RelativeTracksAddToTheRest){
    ClipData data;
    data.tracks = {track("Arm", ClipChannel::Position, {key(0, {0, 0, 0}), key(1, {0, 2, 0})}, true),
                   track("Arm", ClipChannel::Scale, {key(0, {1, 1, 1}), key(1, {2, 2, 2})}, true)};
    scene.getComponent<Transform>(arm).scale = Vector3(3, 3, 3);
    AnimationPlayer object = player({{"bob", data}});
    object.play("bob");
    step(0.0);
    step(0.5);
    EXPECT_NEAR(position(arm).x, 5.0f, 1e-5f);
    EXPECT_NEAR(position(arm).y, 1.0f, 1e-5f);
    EXPECT_NEAR(scene.getComponent<Transform>(arm).scale.x, 4.5f, 1e-5f);
    // deterministic: the base is the rest, never the previous frame
    object.getState("bob").setTime(0.5f);
    step(0.0);
    EXPECT_NEAR(position(arm).y, 1.0f, 1e-5f);
}

// R2: the rest survives rebinding and clip reloads; a fade-out after a rename ends at the scene pose
TEST_F(Player, RestIsKeptAcrossRenamesAndReloads){
    AnimationPlayer object = player({{"walk", slide("Arm", 10, ClipWrapMode::Loop)}});
    object.play("walk");
    step(0.0);
    step(0.5);
    EXPECT_NEAR(x(arm), 5.0f, 1e-5f);
    // the root renamed, a sibling spawned: rebinding keeps the rest (5)
    scene.setEntityName(root, "Hero 2");
    child(root, "Prop");
    step(0.25);
    EXPECT_NEAR(x(arm), 7.5f, 1e-5f);
    // the file edited: the new clip plays from the same rest
    std::string path = scene.getComponent<AnimationPlayerComponent>(root).clips[0].clip;
    animtest::writeAsset(path, anim::writeClip(slide("Arm", 20, ClipWrapMode::Loop)));
    AnimationClipPool::invalidate(path);
    step(0.0);
    EXPECT_NEAR(x(arm), 15.0f, 1e-4f);
    object.blend("walk", 0.0f, 0.5f);
    for (int i = 0; i < 6; i++) step(0.1);
    // weight 0 ends the writes: the last one was 0.1 · sample + 0.9 · rest(5)
    object.blend("walk", 0.001f, 0.0f);
    step(0.0);
    EXPECT_NEAR(x(arm), 5.0f, 0.05f);
}

// a decode (or paste) over a playing player changes the authored list and sets needsLoad, keeping
// the runtime: the states rebuild and rebind, the rest taken at the start of the run stays (R2)
TEST_F(Player, ClipListChangeRebindsAndKeepsTheRest){
    AnimationPlayer object = player({{"walk", slide("Arm", 10, ClipWrapMode::Loop)}});
    object.play("walk");
    step(0.0);
    step(0.5);
    EXPECT_NEAR(x(arm), 5.0f, 1e-5f);
    std::shared_ptr<anim::AnimationPlayerRuntime> runtime = scene.getComponent<AnimationPlayerComponent>(root).runtime;

    // the entry now names another file and a second entry appears (what Stream decode writes)
    AnimationPlayerComponent& component = scene.getComponent<AnimationPlayerComponent>(root);
    component.clips[0].clip = clip("walk2", slide("Arm", 20, ClipWrapMode::Loop));
    AnimationPlayerClip extra;
    extra.clip = clip("wave", slide("Arm/Hand", 4));
    extra.name = "wave";
    component.clips.push_back(extra);
    component.needsLoad = true;
    step(0.25);
    EXPECT_EQ(scene.getComponent<AnimationPlayerComponent>(root).runtime, runtime) << "the runtime is kept";
    EXPECT_TRUE(object.isPlaying("walk")) << "the state survives by name";
    EXPECT_NEAR(object.getState("walk").getTime(), 0.75f, 1e-5f);
    EXPECT_NEAR(x(arm), 15.0f, 1e-4f);
    EXPECT_EQ(object.getClipCount(), 2);

    // fading out ends at the rest of the run's start (5), not at a pose written since
    object.blend("walk", 0.5f, 0.0f);
    step(0.0);
    EXPECT_NEAR(x(arm), 0.5f * 15.0f + 0.5f * 5.0f, 1e-4f);
    object.blend("wave", 1.0f, 0.0f);
    object.getState("wave").setTime(0.5f);
    object.getState("wave").setSpeed(0.0f);
    step(0.1);
    EXPECT_NEAR(x(hand), 2.0f, 1e-4f) << "the new entry binds";
}

TEST_F(Player, RebindsAfterRenameAndReparent){
    AnimationPlayer object = player({{"walk", slide("Arm/Hand", 10, ClipWrapMode::Loop)}});
    object.play("walk");
    step(0.0);
    step(0.5);
    EXPECT_NEAR(x(hand), 5.0f, 1e-5f);
    EXPECT_EQ(object.getStateInfo("walk").boundTracks, 1u);

    scene.setEntityName(hand, "Palm");
    scene.getComponent<Transform>(hand).position.x = -2.0f;
    step(0.1);
    EXPECT_NEAR(x(hand), -2.0f, 1e-6f) << "unbound: skipped silently";
    EXPECT_EQ(object.getStateInfo("walk").boundTracks, 0u);

    // another Hand appears under Arm: the path binds it (after the 4 Hz throttle)
    Entity other = child(arm, "Hand", Vector3(1, 0, 0));
    for (int i = 0; i < 4; i++) step(0.1);
    EXPECT_EQ(object.getStateInfo("walk").boundTracks, 1u);
    EXPECT_NE(x(other), 1.0f);

    // a reparent moves the binding with the path
    scene.destroyEntity(other);
    scene.setEntityName(hand, "Hand");
    Entity sleeve = child(root, "Sleeve");
    scene.addEntityChild(sleeve, arm, false);
    for (int i = 0; i < 4; i++) step(0.1);
    EXPECT_EQ(object.getStateInfo("walk").boundTracks, 0u) << "Arm is now Sleeve/Arm";
    scene.addEntityChild(root, arm, false);
    for (int i = 0; i < 4; i++) step(0.1);
    EXPECT_EQ(object.getStateInfo("walk").boundTracks, 1u);
}

// R10: no writes while a model under the root loads
TEST_F(Player, WaitsForModelsToLoad){
    scene.addComponent<ModelComponent>(arm, {});
    scene.getComponent<ModelComponent>(arm).needUpdateModel = true;
    AnimationPlayer object = player({{"walk", slide("Arm", 10)}});
    object.play("walk");
    step(0.25);
    EXPECT_NEAR(x(arm), 5.0f, 1e-6f);
    EXPECT_NEAR(object.getState("walk").getTime(), 0.0f, 1e-6f);
    scene.getComponent<ModelComponent>(arm).needUpdateModel = false;
    scene.getComponent<ModelComponent>(arm).loadedFilename = "models/hero.gltf";
    step(0.25);
    EXPECT_NEAR(x(arm), 2.5f, 1e-5f);
}

TEST_F(Player, EventsFireOncePerCrossingInTimeOrder){
    ClipData data = slide("Arm", 10, ClipWrapMode::Loop);
    data.events = {event(0.0f, "onStart"), event(0.5f, "onStep", {AnimationEvent::stringArg("left"), AnimationEvent::numberArg(2)})};
    AnimationPlayer object = player({{"walk", data}});
    std::vector<std::string> fired;
    std::vector<AnimationEvent> received;
    object.getOnEvent().add("test", std::function<void(const AnimationEvent&)>([&](const AnimationEvent& e){
        fired.push_back(e.functionName + "@" + std::to_string((int)std::lround(e.time * 10)));
        received.push_back(e);
    }));
    object.play("walk");
    step(0.25);
    EXPECT_EQ(fired, (std::vector<std::string>{"onStart@0"}));
    step(0.5);
    EXPECT_EQ(fired, (std::vector<std::string>{"onStart@0", "onStep@5"}));
    ASSERT_EQ(received.back().args.size(), 2u);
    EXPECT_EQ(received.back().getStringArg(0), "left");
    EXPECT_DOUBLE_EQ(received.back().getNumberArg(1), 2.0);
    EXPECT_EQ(received.back().state, "walk");
    // the loop boundary: start of the next cycle
    step(0.5);
    EXPECT_EQ(fired.back(), "onStart@0");
    // a low frame rate: 3 cycles in one update, each event once
    fired.clear();
    step(3.0);
    EXPECT_EQ(fired, (std::vector<std::string>{"onStep@5", "onStart@0"}));
}

// R16: during a crossfade only the incoming state fires
TEST_F(Player, OnlyTheIncomingStateFiresDuringACrossfade){
    ClipData a = slide("Arm", 10, ClipWrapMode::Loop);
    a.events = {event(0.5f, "footA")};
    ClipData b = slide("Arm", 10, ClipWrapMode::Loop);
    b.events = {event(0.5f, "footB")};
    AnimationPlayer object = player({{"a", a}, {"b", b}});
    std::vector<std::string> fired;
    object.getOnEvent().add("test", std::function<void(const AnimationEvent&)>([&](const AnimationEvent& e){
        fired.push_back(e.functionName);
    }));
    object.play("a");
    step(0.25);
    object.crossFade("b", 1.0f);
    step(0.5);
    EXPECT_EQ(fired, (std::vector<std::string>{"footB"}));
    // several states at full weight each fire (no masks: disjoint bones)
    fired.clear();
    object.blend("a", 1.0f, 0.0f);
    object.blend("b", 1.0f, 0.0f);
    object.getState("a").setTime(0.0f);
    object.getState("b").setTime(0.0f);
    step(0.75);
    EXPECT_EQ(fired.size(), 2u);
}

TEST_F(Player, EventsReachLuaSubscribersAndScriptMethods){
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_TRUE(L != nullptr);
    ASSERT_EQ(luaL_dostring(L,
        "animLog = {}\n"
        "function animOnEvent(event) animLog[#animLog + 1] = event.functionName .. ':' .. tostring(event.args[1]) .. ':' .. tostring(event.args[2]) .. ':' .. event.state end\n"
        "function animOnFinish(state) animLog[#animLog + 1] = 'finish:' .. state end\n"), LUA_OK);
    ClipData data = slide("Arm", 10);
    data.events = {event(0.5f, "onStep", {AnimationEvent::stringArg("left"), AnimationEvent::boolArg(true)})};
    AnimationPlayer object = player({{"wave", data}});
    lua_getglobal(L, "animOnEvent");
    object.getOnEvent().add("lua", L);
    lua_getglobal(L, "animOnFinish");
    object.getOnFinish().add("lua", L);
    object.play("wave");
    step(0.75);
    step(0.75);
    ASSERT_EQ(luaL_dostring(L, "animResult = table.concat(animLog, '|')"), LUA_OK);
    lua_getglobal(L, "animResult");
    std::string result = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    EXPECT_EQ(result, "onStep:left:true:wave|finish:wave");

    // the script method of the event's name gets the args spread
    ASSERT_EQ(luaL_dostring(L,
        "AnimScript = {}\n"
        "function AnimScript:onStep(side, flag) animLog[#animLog + 1] = 'method:' .. side .. ':' .. tostring(flag) end\n"
        "animScriptInstance = setmetatable({}, {__index = AnimScript})\n"), LUA_OK);
    lua_getglobal(L, "animScriptInstance");
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ScriptComponent scripts;
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    entry.enabled = true;
    entry.className = "AnimScript";
    entry.instance = reinterpret_cast<void*>(static_cast<intptr_t>(ref));
    scripts.scripts.push_back(entry);
    scene.addComponent<ScriptComponent>(root, scripts);
    EXPECT_TRUE(LuaBinding::callLuaScriptMethod(&scene, root, "onStep", data.events[0].args));
    EXPECT_FALSE(LuaBinding::callLuaScriptMethod(&scene, root, "missing", {}));
    ASSERT_EQ(luaL_dostring(L, "animResult = animLog[#animLog]"), LUA_OK);
    lua_getglobal(L, "animResult");
    result = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    EXPECT_EQ(result, "method:left:true");
    object.play("wave");
    step(0.75);
    ASSERT_EQ(luaL_dostring(L, "animResult = animLog[#animLog]"), LUA_OK);
    lua_getglobal(L, "animResult");
    result = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    EXPECT_EQ(result, "method:left:true") << "subscribers first, then the script method";
    scene.getComponent<ScriptComponent>(root).scripts.clear();
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    object.getOnEvent().clear();
    object.getOnFinish().clear();
}

TEST_F(Player, ClonesBindTheirOwnHierarchy){
    AnimationPlayer object = player({{"walk", slide("Arm", 10, ClipWrapMode::Loop)}}, true);
    object.getOnEvent().add("test", std::function<void(const AnimationEvent&)>([](const AnimationEvent&){}));
    step(0.0);
    step(0.5);
    std::vector<Entity> copies = EntityCloner::clone(&scene, EntityCloner::collectHierarchy(&scene, root), &scene);
    ASSERT_EQ(copies.size(), 3u);
    AnimationPlayerComponent& copy = scene.getComponent<AnimationPlayerComponent>(copies[0]);
    EXPECT_EQ(copy.clips.size(), 1u);
    EXPECT_TRUE(copy.playAutomatically);
    EXPECT_TRUE(copy.needsLoad);
    EXPECT_FALSE(copy.started);
    EXPECT_FALSE(copy.runtime);
    EXPECT_FALSE(copy.onEvent.hasSubscribers());
    scene.getComponent<Transform>(copies[1]).position.x = 100.0f;
    step(0.25);
    EXPECT_NEAR(x(copies[1]), 2.5f, 1e-5f) << "the copy plays on its own Arm from 0";
    EXPECT_NEAR(x(arm), 7.5f, 1e-5f);
}

TEST_F(Player, ModelPlayAnimationGoesToThePlayer){
    scene.addComponent<ModelComponent>(root, {});
    scene.addComponent<MeshComponent>(root, {});
    // loaded (a model still loading defers the player, R10)
    scene.getComponent<ModelComponent>(root).needUpdateModel = false;
    AnimationPlayer object = player({{"walk", slide("Arm", 10, ClipWrapMode::Loop)}, {"run", slide("Arm", 20, ClipWrapMode::Loop)}});
    object.setDefaultFadeTime(0.5f);
    Model model(&scene, root);
    model.playAnimation("walk", 0.0f);
    EXPECT_TRUE(object.isPlaying("walk"));
    model.playAnimation("run");
    step(0.25);
    EXPECT_NEAR(object.getState("run").getWeight(), 0.5f, 1e-4f) << "defaultFadeTime";
    model.playAnimation(0);
    step(0.0);
    EXPECT_TRUE(object.isPlaying("walk")) << "index = the player's state";
    model.stopAnimations(0.0f);
    EXPECT_FALSE(object.isPlaying());
    model.playAnimation("missing", 0.0f);
    EXPECT_FALSE(object.isPlaying());

    // engine review M11: an index past the player's states falls back to the model's entity
    // animation of that index, as a name does; the player's states make room for it
    std::vector<Entity> entityAnimations;
    for (int i = 0; i < 3; i++){
        Animation animation(&scene);
        animation.setEntityOwned(false);           // the model lists it: it outlives this handle
        entityAnimations.push_back(animation.getEntity());
    }
    scene.getComponent<ModelComponent>(root).animations = entityAnimations;
    auto started = [&](Entity animation){
        const ActionComponent& action = scene.getComponent<ActionComponent>(animation);
        return action.startTrigger || action.state == ActionState::Running;
    };
    model.playAnimation(1, 0.0f);
    EXPECT_TRUE(object.isPlaying("run")) << "1 is still the player's state";
    EXPECT_FALSE(started(entityAnimations[1]));
    model.playAnimation(2);
    EXPECT_TRUE(started(entityAnimations[2])) << "2 is past the player's two states";
    EXPECT_FALSE(started(entityAnimations[0]));
    step(0.5);
    EXPECT_FALSE(object.isPlaying()) << "the player's states faded out (the Animation's defaultFadeTime, 0.15 s)";
    object.play("walk");
    model.playAnimation(3, 0.0f);
    EXPECT_TRUE(object.isPlaying("walk")) << "no state or animation 3: nothing changes";
    EXPECT_FALSE(started(entityAnimations[0]));
    scene.getComponent<ModelComponent>(root).animations.clear();
}

TEST_F(Player, PreviewWritesAndRestores){
    AnimationPlayer object = player({{"walk", slide("Arm", 10)}});
    std::vector<std::string> fired;
    ClipData withEvent = slide("Arm/Hand", 4);
    withEvent.events = {event(0.5f, "step")};
    object.getOnEvent().add("test", std::function<void(const AnimationEvent&)>([&](const AnimationEvent& e){
        fired.push_back(e.functionName);
    }));
    ActionSystem* actions = scene.getSystem<ActionSystem>().get();
    actions->setPaused(true);

    anim::ClipPreviewSession session;
    ActionSystem::PlayerPreview preview;
    ActionSystem::PlayerPreviewState walk;
    walk.state = "walk";
    walk.time = 0.5f;
    preview.states.push_back(walk);
    ActionSystem::PlayerPreviewState explicitClip;
    explicitClip.clip = AnimationClip::build(withEvent);
    explicitClip.time = 0.25f;
    preview.states.push_back(explicitClip);
    ActionSystem::PlayerPreviewResult result = actions->previewPlayer(root, preview, session);
    EXPECT_TRUE(result.ready);
    EXPECT_NEAR(x(arm), 5.0f, 1e-5f);
    EXPECT_NEAR(x(hand), 1.0f, 1e-5f);
    EXPECT_EQ(result.written, (std::vector<Entity>{arm, hand}));
    EXPECT_TRUE(result.unresolved.empty());
    EXPECT_FALSE(scene.getComponent<AnimationPlayerComponent>(root).runtime) << "no runtime in edit mode";

    // half weight blends with the snapshot; events only when asked
    preview.states[0].weight = 0.5f;
    preview.states[1].time = 0.75f;
    preview.fireEvents = true;
    actions->previewPlayer(root, preview, session);
    EXPECT_NEAR(x(arm), 0.5f * 5.0f + 0.5f * 5.0f, 1e-5f);
    EXPECT_EQ(fired, std::vector<std::string>{"step"});

    preview.states[0].state = "nothing";
    EXPECT_FALSE(actions->previewPlayer(root, preview, session).ready);

    actions->restorePlayerPreview(session);
    EXPECT_NEAR(x(arm), 5.0f, 1e-6f);
    EXPECT_NEAR(x(hand), 0.0f, 1e-6f);
    EXPECT_TRUE(session.written.empty());
    actions->setPaused(false);
}

TEST_F(Player, ResetPlayersStartsOver){
    AnimationPlayer object = player({{"walk", slide("Arm", 10, ClipWrapMode::Loop)}}, true);
    object.getOnFinish().add("test", std::function<void(const std::string&)>([](const std::string&){}));
    step(0.5);
    scene.getSystem<ActionSystem>()->resetPlayers();
    AnimationPlayerComponent& component = scene.getComponent<AnimationPlayerComponent>(root);
    EXPECT_FALSE(component.runtime);
    EXPECT_FALSE(component.started);
    EXPECT_FALSE(component.onFinish.hasSubscribers());
    step(0.25);
    EXPECT_NEAR(object.getState("walk").getTime(), 0.25f, 1e-5f);
}

// a model sub-clip played on the model it comes from: every track binds and the bones move
TEST_F(Player, PlaysModelSubClipsOnTheirModel){
    const std::string path = std::string(JASPER_TEST_REPO_DIR "/tools/fixtures/pose-animation/assets/models") + "/hero_animated.gltf";
    Model model(&scene);
    const Entity hero = model.getEntity();
    AnimationPlayer object(&scene, hero);
    object.addClip(path + "#Walk");
    object.setPlayAutomatically(true);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(hero, path, false, false, false));
    scene.getComponent<ModelComponent>(hero).needUpdateModel = false;
    step(0.0);
    ASSERT_TRUE(object.isLoaded()) << object.getStateInfo("Walk").error;
    AnimationStateInfo info = object.getStateInfo("Walk");
    EXPECT_GT(info.trackCount, 0u);
    EXPECT_EQ(info.boundTracks, info.trackCount) << "100% bound on the model's own hierarchy";
    EXPECT_NEAR(info.length, 1.0f, 1e-5f);

    // some bound bone moves between two times
    std::vector<std::pair<Entity, Quaternion>> before;
    for (const auto& entry : anim::descendantPaths(&scene, hero)){
        if (Transform* transform = scene.findComponent<Transform>(entry.second)) before.push_back({entry.second, transform->rotation});
    }
    step(0.3);
    bool moved = false;
    for (const auto& entry : before){
        if (std::fabs(std::fabs(scene.getComponent<Transform>(entry.first).rotation.dot(entry.second)) - 1.0f) > 1e-4f) moved = true;
    }
    EXPECT_TRUE(moved);
}

// Final review M1: in async loading a player whose model sub-clip is still building waits for it
// as for its models (time stops, the play call is kept), then plays from the start
TEST_F(Player, WaitsForASubClipBuildingOffTheMainThread){
    const std::string path = std::string(JASPER_TEST_REPO_DIR "/tools/fixtures/pose-animation/assets/models") + "/hero_animated.gltf";
    Model model(&scene);
    const Entity hero = model.getEntity();
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(hero, path, false, false, false));
    scene.getComponent<ModelComponent>(hero).needUpdateModel = false;
    AnimationPlayer object(&scene, hero);
    object.addClip(path + "#Walk");
    object.setPlayAutomatically(true);

    const bool wasAsync = Engine::isAsyncLoading();
    Engine::setAsyncLoading(true);
    std::atomic<bool> locked{false};
    std::atomic<bool> released{false};
    std::thread loader([&](){
        std::lock_guard<std::recursive_mutex> lock(MeshSystem::getPooledGLTFMutex());
        locked = true;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!released && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    while (!locked) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto start = std::chrono::steady_clock::now();
    step(0.1, 3);
    const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    EXPECT_FALSE(object.isLoaded());
    EXPECT_TRUE(object.isPlaying("Walk")) << "the automatic play is kept";
    EXPECT_NEAR(object.getState("Walk").getTime(), 0.0f, 1e-6f) << "time stops while the clip builds";
    released = true;
    loader.join();
    EXPECT_LT(waited, 5.0) << "the updates waited for the loader's lock";

    for (int i = 0; i < 2000 && !object.isLoaded(); i++){
        step(0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Engine::setAsyncLoading(wasAsync);
    ASSERT_TRUE(object.isLoaded()) << object.getStateInfo("Walk").error;
    step(0.25);
    EXPECT_NEAR(object.getState("Walk").getTime(), 0.25f, 1e-5f) << "plays from its start once built";
}

// R9: 100 players of 60 bones and one spawn per frame stay under 2 ms a frame (Release)
TEST_F(Player, PerformanceBudget){
    ClipData data;
    data.wrap = ClipWrapMode::Loop;
    std::vector<std::string> paths;
    std::string path;
    for (int b = 0; b < 60; b++){
        // a spine of 20 with two arms of 20
        if (b == 0 || b == 20 || b == 40) path = b == 0 ? "Bone0" : "Bone0/Bone" + std::to_string(b);
        else path += "/Bone" + std::to_string(b);
        paths.push_back(path);
        // a skeletal clip as glTF/FBX rigs have them: rotations on every bone, the position of the hips
        if (b == 0) data.tracks.push_back(track(path, ClipChannel::Position, {key(0, {0, 0, 0}), key(0.5f, {1, 0, 0}), key(1, {0, 0, 0})}));
        data.tracks.push_back(track(path, ClipChannel::Rotation, {key(0, {0, 0, 0, 1}), key(0.5f, {0, 0.3827f, 0, 0.9239f}), key(1, {0, 0.7071f, 0, 0.7071f})}));
    }
    std::string clipPath = clip("rig", data);
    Entity bullets = scene.createEntity();
    scene.addComponent<Transform>(bullets, {});
    for (int p = 0; p < 100; p++){
        Entity rigRoot = scene.createEntity();
        scene.setEntityName(rigRoot, "Rig");
        scene.addComponent<Transform>(rigRoot, {});
        std::vector<Entity> bones(60);
        for (int b = 0; b < 60; b++){
            Entity parent = (b == 0) ? rigRoot : (b == 20 || b == 40) ? bones[0] : bones[b - 1];
            bones[b] = child(parent, "Bone" + std::to_string(b));
        }
        AnimationPlayer object(&scene, rigRoot);
        object.addClip(clipPath, "idle");
        object.getState("idle");
    }
    step(1.0 / 60.0, 3);

    // The budget as a ratio, so it holds on any host. Each measured frame (this thread's CPU time:
    // wall time carries the preemptions of the other builds this host runs) is paired with a fixed
    // reference workload run right after it, under the same conditions: quaternion slerps and TRS
    // matrices like the player's sampling, plus a strided pass over ~400 KB of matrices so memory
    // traffic and shared-cache contention slow it as they slow the players' transforms and tracks.
    // The reference's time is the best of 3 runs (noise). A frame counts as `ms / reference ms`,
    // and the phase's median ratio must stay under BUDGET_RATIO: the spec's "2 ms a frame on this
    // host" (8 cores, gcc 13, Release) in units of the reference measured on it idle. Host speed and
    // load scale both sides; a regression of the player still shows in full (the reference does
    // not run the player's code). A phase over the budget runs again, up to PASSES times, and the
    // best pass counts. The absolute 2 ms is printed and asserted only when JASPER_PERF_ABSOLUTE_MS
    // is set (a reference machine or CI: JASPER_PERF_ABSOLUTE_MS=2).
    struct clock{
        using time_point = std::chrono::duration<double>;
        static time_point now(){
#ifdef _WIN32
            FILETIME created, exited, kernel, user;
            GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
            auto ticks = [](const FILETIME& t){ return ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime; };
            return time_point((double)(ticks(kernel) + ticks(user)) * 1e-7);
#else
            timespec spec;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &spec);
            return time_point((double)spec.tv_sec + (double)spec.tv_nsec * 1e-9);
#endif
        }
    };
    std::vector<Quaternion> from(400), to(400);
    std::vector<Vector3> offsets(400);
    std::vector<Matrix4> transforms(400);
    for (size_t i = 0; i < from.size(); i++){
        from[i] = Quaternion((float)(i % 360), Vector3(0, 1, 0));
        to[i] = Quaternion((float)((i * 7) % 360), Vector3(1, 0, 0));
        offsets[i] = Vector3((float)(i % 13), (float)(i % 7), (float)(i % 5));
    }
    // 6400 matrices of 64 bytes, visited in a fixed scattered order (a stride prime to the count)
    std::vector<Matrix4> memory(6400);
    const size_t STRIDE = 97;
    volatile float sink = 0.0f;
    auto referenceOnce = [&](float t){
        const clock::time_point start = clock::now();
        for (size_t i = 0; i < from.size(); i++){
            const Quaternion q = Quaternion::slerp(t, from[i], to[i]);
            transforms[i] = Matrix4::translateMatrix(offsets[i]) * q.getRotationMatrix() * Matrix4::scaleMatrix(Vector3(1, 1, 1));
        }
        for (size_t n = 0, i = 0; n < memory.size(); n++, i = (i + STRIDE) % memory.size()){
            memory[i][3][0] += transforms[n % transforms.size()][3][1];
        }
        float sum = 0.0f;
        for (size_t i = 0; i < transforms.size(); i += 61) sum += transforms[i][3][0];
        for (size_t i = 0; i < memory.size(); i += 641) sum += memory[i][3][0];
        sink = sink + sum;
        return std::chrono::duration<double, std::milli>(clock::now() - start).count();
    };
    auto reference = [&](float t){
        return std::min(referenceOnce(t), std::min(referenceOnce(t), referenceOnce(t)));
    };
    // the reference (best of 3) on this host (8 cores, gcc 13, Release) with nothing else running,
    // and the spec's 2 ms there: the ratio is the budget
    const double REFERENCE_IDLE_MS = 0.403;
    const double BUDGET_MS = 2.0;
    const double BUDGET_RATIO = BUDGET_MS / REFERENCE_IDLE_MS;
    const char* absoluteEnv = std::getenv("JASPER_PERF_ABSOLUTE_MS");
    const double ABSOLUTE_MS = absoluteEnv ? std::atof(absoluteEnv) : 0.0;
    const int PASSES = 5;
    auto median = [](std::vector<double> values){
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };
    struct Result{
        double ratio = 1e9;     // median of ms / reference ms
        double ms = 1e9;        // median frame time
    };
    // a phase of 120 frames (`step` before each), best of up to PASSES passes
    auto best = [&](const char* name, const std::function<void()>& step){
        Result result;
        for (int pass = 0; pass < PASSES && (result.ratio >= BUDGET_RATIO || (ABSOLUTE_MS > 0.0 && result.ms >= ABSOLUTE_MS)); pass++){
            std::vector<double> raw, ratios, references;
            for (int f = 0; f < 120; f++){
                step();
                const clock::time_point start = clock::now();
                scene.getSystem<ActionSystem>()->update(1.0 / 60.0);
                const double ms = std::chrono::duration<double, std::milli>(clock::now() - start).count();
                const double ref = reference((float)(f % 60) / 60.0f);
                raw.push_back(ms);
                references.push_back(ref);
                ratios.push_back(ms / ref);
            }
            const Result phase{median(ratios), median(raw)};
            std::printf("[ perf ]   %s: median %.3f ms, %.2f x the reference (median %.3f ms; budget %.2f x)\n", name, phase.ms,
                        phase.ratio, median(references), BUDGET_RATIO);
            result.ratio = std::min(result.ratio, phase.ratio);
            result.ms = std::min(result.ms, phase.ms);
        }
        return result;
    };
    const Result still = best("still", [](){});
    // one spawn a frame under a parent (bumps the hierarchy revision), one parentless
    const Result spawning = best("spawning", [&](){
        child(bullets, "Bullet");
        Entity loose = scene.createEntity();
        scene.addComponent<Transform>(loose, {});
    });
    // M5: one spawn and one destroy a frame under a bone of a rig (named after parenting)
    Entity bone = scene.findEntity("Bone30");
    ASSERT_NE(bone, NULL_ENTITY);
    Entity attached = NULL_ENTITY;
    const Result underBone = best("under a bone", [&](){
        if (attached != NULL_ENTITY) scene.destroyEntity(attached);
        attached = scene.createEntity();
        scene.addComponent<Transform>(attached, {});
        scene.addEntityChild(bone, attached, false);
        scene.setEntityName(attached, "Spark");
    });
    std::printf("[ perf ] 100 players x 60 bones (6100 tracks): %.3f ms a frame (%.2f x the reference), %.3f ms (%.2f x) with a "
                "spawn per frame, %.3f ms (%.2f x) with one under a bone (medians; budget %.2f x = %.1f ms on the reference host)\n",
                still.ms, still.ratio, spawning.ms, spawning.ratio, underBone.ms, underBone.ratio, BUDGET_RATIO, BUDGET_MS);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    // Release builds: the budget means nothing under the sanitizers, which slow everything
    // several times (and the reference less than the player). Enforced on the benchmark server
    // only, with its slack (BF1: JASPER_TEST_PERF_SLACK; 0 elsewhere: printed, as a shared
    // builder's load alone can miss it)
    const double slack = testsupport::perfSlack();
    if (slack > 0.0){
        EXPECT_LT(spawning.ratio, BUDGET_RATIO * slack);
        EXPECT_LT(underBone.ratio, BUDGET_RATIO * slack);
        if (ABSOLUTE_MS > 0.0){
            EXPECT_LT(spawning.ms, ABSOLUTE_MS * slack) << "JASPER_PERF_ABSOLUTE_MS";
            EXPECT_LT(underBone.ms, ABSOLUTE_MS * slack) << "JASPER_PERF_ABSOLUTE_MS";
        }
    }
#endif
    // they play
    AnimationPlayerComponent& any = scene.getComponentFromIndex<AnimationPlayerComponent>(0);
    ASSERT_TRUE(any.runtime);
    EXPECT_EQ(any.runtime->states[0].bindings.resolvedCount, 61u);
}
