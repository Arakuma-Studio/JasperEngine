// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Timeline audio fired through ActionSystem::update without an audio device (docs/specs/S40-timeline-audio.md
// R4-R7, R15, R17-R21): an Animation Player's source cue asks its AudioSource for a start placed inside the
// frame and a state_exit cue stops it with the fade when the state is left; the legacy timeline starts an
// audio action once per pass (the target restarted an ended action every frame) and places it like a cue;
// and 200 players crossing a cue a frame cost little more than without cues (ClipAudioBench).

#include <gtest/gtest.h>

#include "AnimTestEnvironment.h"
#include "support/AllocationCounter.h"

#include "Engine.h"
#include "Scene.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/Transform.h"
#include "object/AnimationPlayer.h"
#include "pool/AnimationClipPool.h"
#include "subsystem/ActionSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr double kDt = 1.0 / 60.0;

    ClipAudioCue sourceCue(float time, const std::string& path, ClipCueStop stop = ClipCueStop::None){
        ClipAudioCue cue;
        cue.time = time;
        cue.kind = ClipCueKind::Source;
        cue.path = path;
        cue.stop = stop;
        return cue;
    }

    ClipData loopClip(float length, std::vector<ClipAudioCue> cues){
        ClipData data;
        data.wrap = ClipWrapMode::Loop;
        data.length = length;
        ClipTrackData track;
        track.path = "";
        track.channel = ClipChannel::Position;
        ClipKey a;
        a.time = 0.0f;
        a.value = {0, 0, 0};
        ClipKey b;
        b.time = length;
        b.value = {1, 0, 0};
        track.keys = {a, b};
        data.tracks.push_back(track);
        data.audio = std::move(cues);
        return data;
    }

    class ClipCues : public ::testing::Test {
    protected:
        Scene scene;
        Entity root = NULL_ENTITY;
        Entity engine = NULL_ENTITY;
        std::string prefix;

        void SetUp() override {
            static int run = 0;
            prefix = "cues/r" + std::to_string(run++) + "/";
            AnimationClipPool::clear();
            root = scene.createEntity();
            scene.setEntityName(root, "Hero");
            scene.addComponent<Transform>(root, {});
            engine = scene.createEntity();
            scene.setEntityName(engine, "Engine");
            scene.addComponent<Transform>(engine, {});
            scene.addEntityChild(root, engine, false);
            AudioSourceComponent audio;
            audio.filename = "sounds/engine.wav";
            scene.addComponent<AudioSourceComponent>(engine, audio);
        }
        void TearDown() override { AnimationClipPool::clear(); }

        std::string clip(const std::string& name, const ClipData& data){
            animtest::writeAsset(prefix + name + ".anim", anim::writeClip(data));
            return prefix + name + ".anim";
        }
        void step(int count = 1){
            for (int i = 0; i < count; i++) scene.getSystem<ActionSystem>()->update(kDt);
        }
        AudioSourceComponent& source(){ return scene.getComponent<AudioSourceComponent>(engine); }
    };

}

TEST_F(ClipCues, SourceCueAsksForAStartPlacedInsideTheFrame){
    AnimationPlayer player(&scene, root);
    // 0.26 s: crossed in the update from 15/60 to 16/60, 0.6 of the way through it
    player.addClip(clip("engine", loopClip(1.0f, {sourceCue(0.26f, "Engine")})), "Run");
    player.play("Run");
    step(15);
    EXPECT_FALSE(source().startTrigger) << "not crossed yet";
    step();
    ASSERT_TRUE(source().startTrigger);
    EXPECT_TRUE(source().timelineStart);
    const double fraction = (0.26 - 15.0 / 60.0) * 60.0;
    EXPECT_EQ(source().timelineDelayFrames, (uint32_t)std::llround(fraction * kDt * 48000.0));
    EXPECT_FLOAT_EQ(source().timelineSpeedScale, 1.0f);
    // once per crossing: the next update leaves it alone
    source().startTrigger = false;
    step();
    EXPECT_FALSE(source().startTrigger);
}

TEST_F(ClipCues, StateExitStopsTheSourceWhenTheStateIsLeft){
    AnimationPlayer player(&scene, root);
    player.addClip(clip("engine", loopClip(1.0f, {sourceCue(0.0f, "Engine", ClipCueStop::StateExit)})), "Run");
    player.addClip(clip("idle", loopClip(1.0f, {})), "Idle");
    player.play("Run");
    step();
    ASSERT_TRUE(source().startTrigger) << "fired at the state's start";
    source().startTrigger = false;
    source().state = AudioSourceState::Playing;
    step(3);
    EXPECT_FALSE(source().stopTrigger) << "still in its state";
    player.crossFade("Idle", 0.05f);
    step(2);
    EXPECT_FALSE(source().stopTrigger) << "still fading out";
    step(3);
    EXPECT_TRUE(source().stopTrigger) << "its crossfade out ended";
    EXPECT_TRUE(source().timelineStopFade) << "with the 10 ms fade (reason cue_exit)";
}

TEST_F(ClipCues, ACueThatBindsNothingWarnsOncePerPlay){
    AnimationPlayer player(&scene, root);
    player.addClip(clip("lost", loopClip(0.1f, {sourceCue(0.05f, "Nobody")})), "Run");
    player.play("Run");
    animtest::takeWarnings();
    step(30);
    std::vector<std::string> warnings = animtest::takeWarnings();
    const auto count = std::count_if(warnings.begin(), warnings.end(), [](const std::string& w){
        return w.find("Nobody binds nothing") != std::string::npos && w.find("cue 0 of " ) != std::string::npos;
    });
    EXPECT_EQ(count, 1) << "five passes, one warning";
}

namespace {

    // a legacy Animation with one audio action frame
    struct LegacyTimeline {
        Entity animation = NULL_ENTITY;
        Entity step = NULL_ENTITY;
    };

    LegacyTimeline legacyTimeline(Scene& scene, float startTime, float duration, bool loop){
        LegacyTimeline timeline;
        timeline.step = scene.createEntity();
        scene.addComponent<ActionComponent>(timeline.step, {});
        AudioSourceComponent audio;
        audio.filename = "sounds/step.wav";
        scene.addComponent<AudioSourceComponent>(timeline.step, audio);
        timeline.animation = scene.createEntity();
        scene.setEntityName(timeline.animation, "Footsteps");
        scene.addComponent<ActionComponent>(timeline.animation, {});
        AnimationComponent animation;
        animation.loop = loop;
        ActionFrame frame;
        frame.startTime = startTime;
        frame.duration = duration;
        frame.action = timeline.step;
        animation.actions.push_back(frame);
        scene.addComponent<AnimationComponent>(timeline.animation, animation);
        scene.getComponent<ActionComponent>(timeline.animation).startTrigger = true;
        return timeline;
    }

}

TEST(TimelineActions, AnAudioActionStartsOncePerPass){
    Scene scene;
    // a 0.51 s loop (frame 0.11 + 0.4) over 1.2 s: three passes, three starts
    LegacyTimeline timeline = legacyTimeline(scene, 0.11f, 0.4f, true);
    std::vector<int> starts;
    for (int frame = 0; frame < 72; frame++){
        scene.getSystem<ActionSystem>()->update(kDt);
        AudioSourceComponent& audio = scene.getComponent<AudioSourceComponent>(timeline.step);
        if (audio.startTrigger){
            starts.push_back(frame);
            EXPECT_TRUE(audio.timelineStart);
            // the audio plays until its frame ends: the test stands in for the AudioSystem
            audio.startTrigger = false;
            audio.state = AudioSourceState::Playing;
        }
        if (audio.stopTrigger){
            audio.stopTrigger = false;
            audio.state = AudioSourceState::Stopped;
        }
    }
    ASSERT_EQ(starts.size(), 3u) << "once a pass, never again to hold a last value";
    // placed by lookahead: in the update whose advance reaches 0.11 s
    EXPECT_EQ(starts[0], 6);
}

TEST(TimelineActions, AStartIsPlacedWhereTheAnimationReachesTheFrame){
    Scene scene;
    LegacyTimeline timeline = legacyTimeline(scene, 0.26f, 0.0f, false);
    AudioSourceComponent* audio = nullptr;
    int frame = 0;
    for (; frame < 30; frame++){
        scene.getSystem<ActionSystem>()->update(kDt);
        audio = &scene.getComponent<AudioSourceComponent>(timeline.step);
        if (audio->startTrigger) break;
    }
    ASSERT_LT(frame, 30);
    EXPECT_EQ(frame, 15) << "the update in which the animation goes from 15/60 to 16/60";
    EXPECT_EQ(audio->timelineDelayFrames, (uint32_t)std::llround((0.26 - 15.0 / 60.0) * 48000.0));
}

TEST(ClipAudioBench, TwoHundredPlayersCrossingACueAFrame){
    // R15: each player crosses one source cue every frame (a loop of one frame); the cost above the
    // same players without cues, this thread's CPU time per update, the median of 300 frames
    auto run = [](bool cues){
        Scene scene;
        static int run = 0;
        const std::string path = "cues/bench" + std::to_string(run++) + ".anim";
        animtest::writeAsset(path, anim::writeClip(loopClip((float)kDt, cues ? std::vector<ClipAudioCue>{sourceCue(0.0f, "Engine")}
                                                                                 : std::vector<ClipAudioCue>{})));
        for (int p = 0; p < 200; p++){
            Entity root = scene.createEntity();
            scene.addComponent<Transform>(root, {});
            Entity engine = scene.createEntity();
            scene.setEntityName(engine, "Engine");
            scene.addComponent<Transform>(engine, {});
            scene.addComponent<AudioSourceComponent>(engine, {});
            scene.addEntityChild(root, engine, false);
            AnimationPlayer player(&scene, root);
            player.addClip(path, "Run");
            player.play("Run");
        }
        for (int i = 0; i < 5; i++) scene.getSystem<ActionSystem>()->update(kDt);
        std::vector<double> ms;
        for (int i = 0; i < 300; i++){
            timespec a, b;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &a);
            scene.getSystem<ActionSystem>()->update(kDt);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b);
            ms.push_back((double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) * 1e-6);
        }
        std::sort(ms.begin(), ms.end());
        AnimationClipPool::clear();
        return ms[ms.size() / 2];
    };
    const double without = run(false);
    const double with = run(true);
    std::printf("[bench] ClipAudioBench: 200 players %.3f ms a frame without cues, %.3f ms crossing a cue each (+%.3f ms; budget 0.3 ms)\n",
                without, with, with - without);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    const double slack = testsupport::perfSlack();
    if (slack > 0.0) EXPECT_LT(with - without, 0.3 * slack);
#endif
}
