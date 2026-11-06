// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// subsystem/ActionSystem timing: an action's speed applies once (its timecount already runs at
// the speed, durations are not divided by it again), a negative speed holds at the start, sprite
// animations stop on their last frame after its time (also a single frame one) and zero time
// frames cannot hang an update, SpriteAnimation::setAnimation takes a range of only frame 0.
// (The old particle sprite modifier's end-time test is ParticleModules.SetOverAgeFramesStayInsideTheList.)

#include <gtest/gtest.h>

#include "Scene.h"
#include "subsystem/ActionSystem.h"
#include "action/Action.h"
#include "action/Animation.h"
#include "action/PositionTween.h"
#include "action/SpriteAnimation.h"
#include "action/keyframe/TranslateTracks.h"
#include "component/ActionComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/MeshComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"

#include <cmath>
#include <memory>
#include <vector>

using namespace jasperengine;

namespace {

    void step(Scene& scene, double dt, int count = 1) {
        for (int i = 0; i < count; i++) {
            scene.getSystem<ActionSystem>()->update(dt);
        }
    }

    Entity createTransformTarget(Scene& scene) {
        Entity target = scene.createEntity();
        scene.addComponent<Transform>(target, Transform());
        return target;
    }

    Entity createSpriteTarget(Scene& scene) {
        Entity target = scene.createEntity();
        scene.addComponent<MeshComponent>(target, MeshComponent());
        scene.addComponent<SpriteComponent>(target, SpriteComponent());
        return target;
    }

}

// ---- speed applies once

TEST(ActionSpeed, TweenAtSpeedTwoTakesHalfItsDuration) {
    Scene scene;
    Entity target = createTransformTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 1.0f);
    tween.setTarget(target);
    tween.setSpeed(2.0f);
    tween.start();

    // 0.125 s steps at speed 2: the tween time moves 0.25 per update, from 0 on the first
    step(scene, 0.125, 3);
    EXPECT_FLOAT_EQ(tween.getTime(), 0.5f) << "half way after 0.25 s, not the end";
    EXPECT_FLOAT_EQ(scene.getComponent<Transform>(target).position.x, 5.0f);

    step(scene, 0.125, 1);
    EXPECT_FLOAT_EQ(tween.getTime(), 0.75f);
    step(scene, 0.125, 1);
    EXPECT_FLOAT_EQ(scene.getComponent<Transform>(target).position.x, 10.0f) << "the end after 0.5 s";
}

TEST(ActionSpeed, AnimationDurationIsInItsOwnTime) {
    Scene scene;
    Entity target = createTransformTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 10.0f);

    Animation animation(&scene);
    animation.setDuration(1.0f);
    animation.addActionFrame(0.0f, 10.0f, tween.getEntity(), target);
    animation.setSpeed(2.0f);
    animation.start();

    // a 1 s animation at speed 2 runs 0.5 s
    step(scene, 0.125, 4);
    EXPECT_TRUE(animation.isRunning()) << "still running after 0.375 s";
    step(scene, 0.125, 1);
    EXPECT_TRUE(animation.isStopped()) << "stopped once its timecount reached 1 (0.5 s)";
}

TEST(ActionSpeed, LoopingAnimationWrapsAtItsDuration) {
    Scene scene;
    Entity target = createTransformTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 10.0f);

    Animation animation(&scene);
    animation.setDuration(1.0f);
    animation.setLoop(true);
    animation.addActionFrame(0.0f, 10.0f, tween.getEntity(), target);
    animation.setSpeed(2.0f);
    animation.start();

    // 0.375 per update: 1.125 after three, which the fourth wraps to 0.125 and advances
    step(scene, 0.1875, 3);
    EXPECT_TRUE(animation.isRunning());
    EXPECT_FLOAT_EQ(animation.getTimeCount(), 1.125f) << "a 1 s loop has not wrapped at 0.5";
    step(scene, 0.1875, 1);
    EXPECT_FLOAT_EQ(animation.getTimeCount(), 0.5f) << "wrapped keeping the overshoot, then advanced";
}

TEST(ActionSpeed, NegativeSpeedHoldsAtTheStart) {
    Scene scene;
    Entity target = createTransformTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 1.0f);
    tween.setTarget(target);
    tween.start();

    step(scene, 0.125, 3);
    EXPECT_FLOAT_EQ(tween.getTimeCount(), 0.375f);

    tween.setSpeed(-1.0f);
    step(scene, 0.125, 10);
    EXPECT_TRUE(tween.isRunning());
    EXPECT_FLOAT_EQ(tween.getTimeCount(), 0.0f) << "plays back to the start and stays there";
    EXPECT_FLOAT_EQ(tween.getTime(), 0.0f);
    EXPECT_FLOAT_EQ(scene.getComponent<Transform>(target).position.x, 0.0f);
}

// a non-looping Animation with a duration samples its tracks at the end of the clip on its last
// update at any speed: the duration is clip time (runtime-core spec 6.3)
TEST(ActionSpeed, NonLoopingAnimationEndsOnItsLastKeyAtSpeedTwo) {
    Scene scene;
    Entity target = createTransformTarget(scene);

    TranslateTracks track(&scene, {0.0f, 1.0f}, {Vector3(0, 0, 0), Vector3(10, 0, 0)});

    Animation animation(&scene);
    animation.setDuration(1.0f);
    animation.addActionFrame(0.0f, 1.0f, track.getEntity(), target);
    animation.setSpeed(2.0f);
    animation.start();

    step(scene, 0.3, 4);
    EXPECT_TRUE(animation.isStopped());
    EXPECT_NEAR(scene.getComponent<Transform>(target).position.x, 10.0f, 1e-4f) << "the last key, not the pose at half the clip";
}

// ---- Start Offset and Random Start (runtime-core spec 6.7)

TEST(ActionStartOffset, TweenStartsAtItsOffset) {
    Scene scene;
    Entity target = createTransformTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 1.0f);
    tween.setTarget(target);
    tween.setStartOffset(0.25f);
    tween.start();

    step(scene, 0.0, 1);
    EXPECT_FLOAT_EQ(tween.getTimeCount(), 0.25f);
    EXPECT_FLOAT_EQ(scene.getComponent<Transform>(target).position.x, 2.5f);
    // an update applies the value at the time it starts from, then advances
    step(scene, 0.25, 1);
    EXPECT_FLOAT_EQ(tween.getTimeCount(), 0.5f) << "it plays on from there";
    step(scene, 0.0, 1);
    EXPECT_FLOAT_EQ(scene.getComponent<Transform>(target).position.x, 5.0f);
}

TEST(ActionStartOffset, LoopingTweenWrapsAnOffsetPastItsPeriod) {
    Scene scene;
    Entity target = createTransformTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 1.0f, true);
    tween.setTarget(target);
    tween.setStartOffset(2.25f);
    tween.start();

    step(scene, 0.0, 1);
    EXPECT_TRUE(tween.isRunning());
    EXPECT_NEAR(scene.getComponent<Transform>(target).position.x, 2.5f, 1e-4f) << "two whole cycles in, then a quarter";
}

TEST(ActionStartOffset, NonLoopingOffsetPastTheEndHoldsTheEnd) {
    Scene scene;
    Entity target = createTransformTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 1.0f);
    tween.setTarget(target);
    tween.setStartOffset(5.0f);
    tween.start();

    step(scene, 0.0, 1);
    EXPECT_TRUE(tween.isStopped());
    EXPECT_FLOAT_EQ(scene.getComponent<Transform>(target).position.x, 10.0f);
}

TEST(ActionStartOffset, ResumeAfterPauseKeepsTheTime) {
    Scene scene;
    Entity target = createTransformTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 4.0f);
    tween.setTarget(target);
    tween.setStartOffset(0.25f);
    tween.start();
    step(scene, 0.0, 1);
    step(scene, 0.5, 1);
    EXPECT_FLOAT_EQ(tween.getTimeCount(), 0.75f);

    tween.pause();
    step(scene, 0.5, 1);
    tween.start();
    step(scene, 0.0, 1);
    EXPECT_FLOAT_EQ(tween.getTimeCount(), 0.75f) << "a resume is no new run";

    tween.restart();
    step(scene, 0.0, 1);
    EXPECT_FLOAT_EQ(tween.getTimeCount(), 0.25f) << "a restart is a new run: the offset again";
}

TEST(ActionStartOffset, SpeedDoesNotScaleTheOffset) {
    Scene scene;
    Entity target = createTransformTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 4.0f);
    tween.setTarget(target);
    tween.setSpeed(2.0f);
    tween.setStartOffset(0.5f);
    tween.start();

    step(scene, 0.0, 1);
    EXPECT_FLOAT_EQ(tween.getTimeCount(), 0.5f) << "seconds of the action's own time";
    step(scene, 0.1, 1);
    EXPECT_FLOAT_EQ(tween.getTimeCount(), 0.7f);
}

TEST(ActionStartOffset, NegativeOffsetIsZero) {
    Scene scene;
    PositionTween tween(&scene);
    tween.setStartOffset(-1.0f);
    EXPECT_FLOAT_EQ(tween.getStartOffset(), 0.0f);
}

namespace {
    // the start phases of two looping tweens with Random Start, after one update of 0 s
    std::vector<float> randomPhases(Scene& scene, bool restart = false) {
        std::vector<float> phases;
        std::vector<std::unique_ptr<PositionTween>> tweens;
        for (int i = 0; i < 2; i++) {
            auto tween = std::make_unique<PositionTween>(&scene);
            tween->setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 1.0f, true);
            tween->setTarget(createTransformTarget(scene));
            tween->setRandomStart(true);
            tween->start();
            tweens.push_back(std::move(tween));
        }
        step(scene, 0.0, 1);
        if (restart) {
            for (auto& tween : tweens) tween->restart();
            step(scene, 0.0, 1);
        }
        for (auto& tween : tweens) phases.push_back(tween->getTimeCount());
        return phases;
    }
}

TEST(ActionStartOffset, RandomStartIsDeterministicAndDiffersBetweenCopies) {
    Scene first;
    const std::vector<float> a = randomPhases(first);
    ASSERT_EQ(a.size(), 2u);
    for (float phase : a) {
        EXPECT_GE(phase, 0.0f);
        EXPECT_LT(phase, 1.0f) << "a point of one cycle";
    }
    EXPECT_NE(a[0], a[1]) << "copies start apart";

    Scene second;
    EXPECT_EQ(randomPhases(second), a) << "the same scene starts the same every playthrough";

    Scene third;
    const std::vector<float> restarted = randomPhases(third, true);
    EXPECT_NE(restarted[0], a[0]) << "each new run picks again";
}

TEST(ActionStartOffset, AnimationChildOffsetShiftsItsTrack) {
    Scene scene;
    Entity target = createTransformTarget(scene);

    TranslateTracks track(&scene, {0.0f, 1.0f}, {Vector3(0, 0, 0), Vector3(10, 0, 0)});
    track.setStartOffset(0.5f);

    Animation animation(&scene);
    animation.addActionFrame(0.0f, 1.0f, track.getEntity(), target);
    animation.setLoop(true);
    animation.start();

    step(scene, 0.0, 1);
    EXPECT_NEAR(scene.getComponent<Transform>(target).position.x, 5.0f, 1e-4f) << "the track starts half way";
}

TEST(ActionStartOffset, SpriteAnimationStartsOnTheOffsetFrame) {
    Scene scene;
    Entity target = createSpriteTarget(scene);

    SpriteAnimation anim(&scene);
    anim.setAnimation({0, 1, 2, 3}, {100, 100, 100, 100}, true);
    anim.setTarget(target);
    anim.setStartOffset(0.25f);
    anim.start();

    SpriteAnimationComponent& spriteanim = scene.getComponent<SpriteAnimationComponent>(anim.getEntity());
    step(scene, 0.0, 1);
    EXPECT_EQ(spriteanim.frameIndex, 2) << "250 ms in: the third frame";
    step(scene, 0.05, 1);
    EXPECT_EQ(spriteanim.frameIndex, 3) << "its remaining 50 ms";

    // past the end of a non-looping one: the last frame, then it stops
    SpriteAnimation once(&scene);
    once.setAnimation({0, 1, 2}, {100, 100, 100}, false);
    once.setTarget(createSpriteTarget(scene));
    once.setStartOffset(10.0f);
    once.start();
    step(scene, 0.0, 1);
    EXPECT_EQ(scene.getComponent<SpriteAnimationComponent>(once.getEntity()).frameIndex, 2);
    step(scene, 0.01, 1);
    EXPECT_TRUE(once.isStopped());
}

TEST(ActionStartOffset, PreviewIgnoresOffsets) {
    Scene scene;
    Entity target = createTransformTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 1.0f);
    tween.setTarget(target);
    tween.setStartOffset(0.5f);
    tween.setRandomStart(true);
    tween.start();

    scene.getSystem<ActionSystem>()->updateActionPreview(0.0, tween.getEntity());
    EXPECT_FLOAT_EQ(tween.getTimeCount(), 0.0f) << "an editor preview starts at the clip's beginning";
}

TEST(ActionStartOffset, ParticleControllerIgnoresOffsets) {
    Scene scene;
    Entity system = createTransformTarget(scene);
    scene.addComponent<ParticleSystemComponent>(system, ParticleSystemComponent());

    Action controller(&scene);
    controller.setTarget(system);
    controller.setStartOffset(1.0f);
    controller.start();
    step(scene, 0.0, 1);
    EXPECT_FLOAT_EQ(controller.getTimeCount(), 0.0f) << "a Particle System has Start Delay and Prewarm instead";
}

// ---- sprite animations

TEST(SpriteAnimationStop, LastFrameKeepsItsTime) {
    Scene scene;
    Entity target = createSpriteTarget(scene);

    SpriteAnimation anim(&scene);
    anim.setAnimation({0, 1, 2}, {100, 100, 100}, false);
    anim.setTarget(target);
    anim.start();

    SpriteAnimationComponent& spriteanim = scene.getComponent<SpriteAnimationComponent>(anim.getEntity());

    // 50 ms steps: frame 1 at 100 ms, frame 2 at 200 ms, the end at 300 ms
    step(scene, 0.05, 4);
    EXPECT_EQ(spriteanim.frameIndex, 2);
    EXPECT_TRUE(anim.isRunning()) << "the last frame shows for its own time";
    step(scene, 0.05, 1);
    EXPECT_TRUE(anim.isRunning());
    step(scene, 0.05, 1);
    EXPECT_TRUE(anim.isStopped()) << "stopped after the 300 ms the three frames take";
    EXPECT_EQ(spriteanim.frameIndex, 2) << "stopped on its last frame";

    step(scene, 0.05, 4);
    EXPECT_EQ(spriteanim.frameIndex, 2);
}

TEST(SpriteAnimationStop, SingleFrameAnimationStops) {
    Scene scene;
    Entity target = createSpriteTarget(scene);

    SpriteAnimation anim(&scene);
    anim.setAnimation(0, 0, 100, false);
    anim.setTarget(target);
    anim.start();

    step(scene, 0.05, 1);
    EXPECT_TRUE(anim.isRunning());
    step(scene, 0.05, 1);
    EXPECT_TRUE(anim.isStopped());
    EXPECT_EQ(scene.getComponent<SpriteAnimationComponent>(anim.getEntity()).frameIndex, 0);
}

TEST(SpriteAnimationStop, ZeroTimeFramesAdvanceOncePerUpdate) {
    Scene scene;
    Entity target = createSpriteTarget(scene);

    SpriteAnimation anim(&scene);
    anim.setAnimation({0, 1, 2}, {0, 0, 0}, true);
    anim.setTarget(target);
    anim.start();

    SpriteAnimationComponent& spriteanim = scene.getComponent<SpriteAnimationComponent>(anim.getEntity());

    // would never leave the first update before
    step(scene, 0.016, 1);
    EXPECT_EQ(spriteanim.frameIndex, 1);
    step(scene, 0.016, 1);
    EXPECT_EQ(spriteanim.frameIndex, 2);
    step(scene, 0.016, 1);
    EXPECT_EQ(spriteanim.frameIndex, 0);
    EXPECT_TRUE(anim.isRunning());
}

TEST(SpriteAnimationStop, SpeedScalesFrameTimes) {
    Scene scene;
    Entity target = createSpriteTarget(scene);

    SpriteAnimation anim(&scene);
    anim.setAnimation({0, 1, 2}, {100, 100, 100}, false);
    anim.setTarget(target);
    anim.setSpeed(2.0f);
    anim.start();

    step(scene, 0.05, 2);
    EXPECT_EQ(scene.getComponent<SpriteAnimationComponent>(anim.getEntity()).frameIndex, 2);
    step(scene, 0.05, 1);
    EXPECT_TRUE(anim.isStopped()) << "300 ms of frames at speed 2 take 150 ms";
}

TEST(SpriteAnimationRange, OnlyFrameZero) {
    Scene scene;

    SpriteAnimation anim(&scene);
    anim.setAnimation(0, 0, 100, false);

    SpriteAnimationComponent& spriteanim = scene.getComponent<SpriteAnimationComponent>(anim.getEntity());
    ASSERT_EQ(spriteanim.framesSize, 1u);
    EXPECT_EQ(spriteanim.frames[0], 0);
    EXPECT_EQ(spriteanim.framesTimeSize, 1u);
    EXPECT_EQ(spriteanim.framesTime[0], 100);

    anim.setAnimation(2, 4, 50, true);
    EXPECT_EQ(spriteanim.framesSize, 3u);
    EXPECT_EQ(spriteanim.frames[2], 4);
}
