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
#include "action/Animation.h"
#include "action/PositionTween.h"
#include "action/SpriteAnimation.h"
#include "component/ActionComponent.h"
#include "component/MeshComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"

#include <cmath>
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
    anim.setAnimation({0}, {100}, false);
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
