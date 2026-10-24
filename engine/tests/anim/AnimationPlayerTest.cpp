// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation Player (spec 3.2-3.6, 9.R1/R2/R9/R10/R14-R19/R21-R25/R42-R44) through
// ActionSystem::update: play / crossFade / blend / queue / stop / rewind / sample timelines,
// wrap modes, the rest term of the blend, rest kept across rebinding and clip reloads, relative
// tracks, rebinding after renames, model-load deferral, events and onFinish (C++ and Lua),
// cloning, Model:playAnimation, edit-mode preview, and the R9 performance budget.

#include <gtest/gtest.h>

#include "AnimTestEnvironment.h"

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

#include "lua.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cmath>

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
    EXPECT_NEAR(object.getState("wave").getTime(), 0.0f, 1e-6f) << "once resets its time";
    scene.getComponent<Transform>(arm).position.x = 3.0f;
    step(0.5);
    EXPECT_NEAR(x(arm), 3.0f, 1e-6f) << "a stopped player no longer writes";
    EXPECT_EQ(finished.size(), 1u);
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
        data.tracks.push_back(track(path, ClipChannel::Position, {key(0, {0, 0, 0}), key(0.5f, {1, 0, 0}), key(1, {0, 0, 0})}));
        data.tracks.push_back(track(path, ClipChannel::Rotation, {key(0, {0, 0, 0, 1}), key(1, {0, 0.7071f, 0, 0.7071f})}));
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

    // the median frame of this thread's CPU time (the host runs other builds in parallel: wall
    // time and averages carry their preemptions)
    struct clock{
        using time_point = std::chrono::duration<double>;
        static time_point now(){
            timespec spec;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &spec);
            return time_point((double)spec.tv_sec + (double)spec.tv_nsec * 1e-9);
        }
    };
    auto measure = [&](bool spawn){
        std::vector<double> times;
        for (int f = 0; f < 120; f++){
            if (spawn){
                // one spawn a frame under a parent (bumps the hierarchy revision), one parentless
                child(bullets, "Bullet");
                Entity loose = scene.createEntity();
                scene.addComponent<Transform>(loose, {});
            }
            clock::time_point start = clock::now();
            scene.getSystem<ActionSystem>()->update(1.0 / 60.0);
            times.push_back(std::chrono::duration<double, std::milli>(clock::now() - start).count());
        }
        std::sort(times.begin(), times.end());
        return times[times.size() / 2];
    };
    const double still = measure(false);
    const double spawning = measure(true);
    std::printf("[ perf ] 100 players x 60 bones (12000 tracks): %.3f ms a frame, %.3f ms with a spawn per frame (medians)\n",
                still, spawning);
#ifdef NDEBUG
    EXPECT_LT(spawning, 2.0);
#endif
    // they play
    AnimationPlayerComponent& any = scene.getComponentFromIndex<AnimationPlayerComponent>(0);
    ASSERT_TRUE(any.runtime);
    EXPECT_EQ(any.runtime->states[0].bindings.resolvedCount, 120u);
}
