// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// subsystem/ActionSystem: looping keyframe tracks keep their overshoot, relative tracks add to
// the pose the target had when they started (kept across the loops of their animation, an
// anchored UI element moved through its RectTransform offset), tween ping-pong and spin,
// a tween stopping on the update it ends, and the triggers: restart(), stop() cancelling a
// start of the same frame, the stop running before the start, an onStop destroying its entity.

#include <gtest/gtest.h>

#include "Scene.h"
#include "subsystem/ActionSystem.h"
#include "action/Animation.h"
#include "action/PositionTween.h"
#include "action/RotationTween.h"
#include "action/keyframe/ScaleTracks.h"
#include "action/keyframe/TranslateTracks.h"
#include "component/ActionComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/RectTransformComponent.h"
#include "component/Transform.h"
#include "util/Angle.h"

#include <cmath>
#include <vector>

using namespace jasperengine;

namespace {

    void step(Scene& scene, double dt, int count = 1) {
        for (int i = 0; i < count; i++) {
            scene.getSystem<ActionSystem>()->update(dt);
        }
    }

    Entity createTarget(Scene& scene, Vector3 position = Vector3(0, 0, 0)) {
        Entity target = scene.createEntity();
        Transform transform;
        transform.position = position;
        scene.addComponent<Transform>(target, transform);
        return target;
    }

    Vector3 positionOf(Scene& scene, Entity entity) {
        return scene.getComponent<Transform>(entity).position;
    }

    bool sameRotation(const Quaternion& a, const Quaternion& b) {
        return std::fabs(std::fabs(a.dot(b)) - 1.0f) < 1e-4f;
    }

}

// ---- keyframe tracks

TEST(KeyframeTracksLoop, StartsOverKeepingTheOvershoot) {
    Scene scene;
    Entity target = createTarget(scene);

    TranslateTracks track(&scene, {0.0f, 1.0f}, {Vector3(0, 0, 0), Vector3(10, 0, 0)});
    track.setLoop(true);
    track.setTarget(target);
    track.start();

    // timecount 0, 0.375, 0.75, then 1.125 wraps to 0.125
    step(scene, 0.375, 3);
    EXPECT_NEAR(positionOf(scene, target).x, 7.5f, 1e-4f);
    step(scene, 0.375, 1);
    EXPECT_TRUE(track.isRunning()) << "a looping track does not stop at its last key";
    EXPECT_NEAR(positionOf(scene, target).x, 1.25f, 1e-4f);
}

TEST(KeyframeTracksLoop, NonLoopingStopsOnItsLastKey) {
    Scene scene;
    Entity target = createTarget(scene);

    TranslateTracks track(&scene, {0.0f, 1.0f}, {Vector3(0, 0, 0), Vector3(10, 0, 0)});
    track.setTarget(target);
    track.start();

    step(scene, 0.375, 4);
    EXPECT_TRUE(track.isStopped());
    EXPECT_NEAR(positionOf(scene, target).x, 10.0f, 1e-4f);
}

TEST(KeyframeTracksRelative, ValuesAddToTheStartPose) {
    Scene scene;
    Entity target = createTarget(scene, Vector3(5, 1, 0));
    scene.getComponent<Transform>(target).scale = Vector3(2, 2, 2);

    TranslateTracks move(&scene, {0.0f, 1.0f}, {Vector3(0, 0, 0), Vector3(0, 2, 0)});
    move.setRelative(true);
    move.setTarget(target);
    move.start();

    ScaleTracks grow(&scene, {0.0f, 1.0f}, {Vector3(1, 1, 1), Vector3(2, 2, 2)});
    grow.setRelative(true);
    grow.setTarget(target);
    grow.start();

    step(scene, 0.5, 4);
    EXPECT_TRUE(move.isStopped());
    Vector3 position = positionOf(scene, target);
    EXPECT_NEAR(position.x, 5.0f, 1e-4f);
    EXPECT_NEAR(position.y, 3.0f, 1e-4f) << "start y 1 plus the track's 2";
    EXPECT_NEAR(scene.getComponent<Transform>(target).scale.x, 4.0f, 1e-4f) << "start scale 2 times the track's 2";

    // a new run starts from where the last one left it
    move.start();
    step(scene, 0.5, 4);
    EXPECT_NEAR(positionOf(scene, target).y, 5.0f, 1e-4f);
}

TEST(KeyframeTracksRelative, LoopingAnimationKeepsTheFirstBase) {
    Scene scene;
    Entity target = createTarget(scene, Vector3(3, 0, 0));

    TranslateTracks track(&scene, {0.0f, 1.0f}, {Vector3(0, 0, 0), Vector3(10, 0, 0)});
    track.setRelative(true);

    Animation animation(&scene);
    animation.setLoop(true);
    animation.addActionFrame(0.0f, 0.0f, track.getEntity(), target);
    animation.start();

    // 0.25 s steps: the track ends at 1 s (x 13), the animation wraps at the next update and the
    // ended track starts again inside the same run, from the pose it first started at
    step(scene, 0.25, 5);
    EXPECT_NEAR(positionOf(scene, target).x, 13.0f, 1e-4f);
    step(scene, 0.25, 2);
    EXPECT_TRUE(animation.isRunning());
    EXPECT_NEAR(positionOf(scene, target).x, 8.0f, 1e-4f) << "3 + 5 half way through the second loop, not 13 + 5";

    // stopped and started again: a new run takes the current pose
    animation.stop();
    step(scene, 0.25, 1);
    float restartX = positionOf(scene, target).x;
    animation.start();
    step(scene, 0.25, 3);
    EXPECT_NEAR(positionOf(scene, target).x, restartX + 5.0f, 1e-4f);
}

TEST(KeyframeTracksRelative, AnchoredElementMovesByItsOffset) {
    Scene scene;
    Entity target = createTarget(scene, Vector3(40, 20, 0));
    RectTransformComponent layout;
    layout.usingAnchors = true;
    layout.positionOffset = Vector2(4, 2);
    scene.addComponent<RectTransformComponent>(target, layout);

    TranslateTracks track(&scene, {0.0f, 1.0f}, {Vector3(0, 0, 0), Vector3(10, 0, 0)});
    track.setRelative(true);
    track.setTarget(target);
    track.start();

    step(scene, 0.5, 3);
    EXPECT_TRUE(track.isStopped());
    // the layout places it from the offset every frame, so the move is in the offset too
    RectTransformComponent& result = scene.getComponent<RectTransformComponent>(target);
    EXPECT_NEAR(result.positionOffset.x, 14.0f, 1e-4f);
    EXPECT_NEAR(result.positionOffset.y, 2.0f, 1e-4f);
    EXPECT_NEAR(positionOf(scene, target).x, 50.0f, 1e-4f);
    EXPECT_NEAR(positionOf(scene, target).y, 20.0f, 1e-4f);
}

TEST(KeyframeTracksAnchored, AbsoluteTrackMovesTheOffsetToo) {
    Scene scene;
    Entity target = createTarget(scene, Vector3(40, 20, 0));
    RectTransformComponent layout;
    layout.usingAnchors = true;
    scene.addComponent<RectTransformComponent>(target, layout);

    TranslateTracks track(&scene, {0.0f, 1.0f}, {Vector3(40, 20, 0), Vector3(60, 30, 0)});
    track.setTarget(target);
    track.start();

    step(scene, 0.5, 3);
    RectTransformComponent& result = scene.getComponent<RectTransformComponent>(target);
    EXPECT_NEAR(result.positionOffset.x, 20.0f, 1e-4f);
    EXPECT_NEAR(result.positionOffset.y, 10.0f, 1e-4f);
}

// ---- tweens

TEST(TweenPingPong, ForwardThenBackInTwiceTheDuration) {
    Scene scene;
    Entity target = createTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 1.0f);
    tween.setPingPong(true);
    tween.setTarget(target);
    tween.start();

    EXPECT_FLOAT_EQ(scene.getSystem<ActionSystem>()->getDuration(tween.getEntity()), 2.0f);

    step(scene, 0.25, 5);
    EXPECT_NEAR(positionOf(scene, target).x, 10.0f, 1e-4f) << "the far end at 1 s";
    step(scene, 0.25, 1);
    EXPECT_NEAR(positionOf(scene, target).x, 7.5f, 1e-4f) << "coming back";
    step(scene, 0.25, 3);
    EXPECT_NEAR(positionOf(scene, target).x, 0.0f, 1e-4f) << "back at 2 s";
    EXPECT_TRUE(tween.isStopped());
}

TEST(TweenEnd, StopsOnTheUpdateItEnds) {
    Scene scene;
    Entity target = createTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 0.5f);
    tween.setTarget(target);
    tween.start();

    step(scene, 0.25, 2);
    EXPECT_TRUE(tween.isRunning());
    step(scene, 0.25, 1);
    EXPECT_TRUE(tween.isStopped()) << "not one update later";
    EXPECT_NEAR(positionOf(scene, target).x, 10.0f, 1e-4f) << "after applying the end value";
}

TEST(TweenEnd, LoopKeepsTheOvershootOfALongStep) {
    Scene scene;
    Entity target = createTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 1.0f, true);
    tween.setTarget(target);
    tween.start();

    // timecount 2.5 is half way through the third cycle
    step(scene, 2.5, 2);
    EXPECT_NEAR(positionOf(scene, target).x, 5.0f, 1e-4f);
}

TEST(RotationTweenSpin, TurnsPastHalfATurn) {
    Scene scene;
    Entity target = createTarget(scene);

    RotationTween tween(&scene);
    tween.setSpinAction(Quaternion(), Vector3(0, 1, 0), Angle::degToDefault(270.0f), 1.0f);
    tween.setTarget(target);
    tween.start();

    step(scene, 0.5, 2);
    EXPECT_TRUE(sameRotation(scene.getComponent<Transform>(target).rotation, Quaternion(Angle::degToDefault(135.0f), Vector3(0, 1, 0))));
    step(scene, 0.5, 1);
    EXPECT_TRUE(tween.isStopped());
    EXPECT_TRUE(sameRotation(scene.getComponent<Transform>(target).rotation, Quaternion(Angle::degToDefault(270.0f), Vector3(0, 1, 0))))
        << "270 degrees, not the 90 a slerp would take";
}

// ---- triggers

TEST(ActionTriggers, RestartStartsAgainInOneUpdate) {
    Scene scene;
    Entity target = createTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 1.0f);
    tween.setTarget(target);
    tween.start();

    step(scene, 0.25, 3);
    EXPECT_NEAR(positionOf(scene, target).x, 5.0f, 1e-4f);

    tween.restart();
    step(scene, 0.25, 1);
    EXPECT_TRUE(tween.isRunning());
    EXPECT_NEAR(positionOf(scene, target).x, 0.0f, 1e-4f) << "from the beginning";

    // a stopped action restarts as a plain start
    tween.stop();
    step(scene, 0.25, 1);
    EXPECT_TRUE(tween.isStopped());
    tween.restart();
    step(scene, 0.25, 1);
    EXPECT_TRUE(tween.isRunning());
}

TEST(ActionTriggers, StopCancelsAStartOfTheSameFrame) {
    Scene scene;
    Entity target = createTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 1.0f);
    tween.setTarget(target);

    tween.start();
    tween.stop();
    step(scene, 0.25, 1);
    EXPECT_TRUE(tween.isStopped()) << "start then stop in one frame leaves it stopped";

    tween.start();
    step(scene, 0.25, 1);
    tween.start();
    tween.stop();
    step(scene, 0.25, 1);
    EXPECT_TRUE(tween.isStopped());
}

TEST(ActionTriggers, SetTargetRestartsARunningAction) {
    Scene scene;
    Entity first = createTarget(scene);
    Entity second = createTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 1.0f);
    tween.setTarget(first);
    tween.start();
    step(scene, 0.25, 3);

    tween.setTarget(second);
    step(scene, 0.25, 1);
    EXPECT_TRUE(tween.isRunning()) << "the stop and the start of one update";
    EXPECT_FLOAT_EQ(tween.getTimeCount(), 0.25f) << "from the beginning";
}

TEST(ActionTriggers, OnStopDestroyingItsEntity) {
    Scene scene;
    Entity target = createTarget(scene);

    Entity actionEntity;
    {
        PositionTween tween(&scene);
        tween.setEntityOwned(false);
        tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 1.0f);
        tween.setTarget(target);
        tween.start();
        actionEntity = tween.getEntity();
    }

    PositionTween other(&scene);
    other.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 1.0f);
    other.setTarget(createTarget(scene));
    other.start();

    step(scene, 0.25, 1);

    ActionComponent& action = scene.getComponent<ActionComponent>(actionEntity);
    action.onStop.add("destroy", std::function<void()>([&scene, actionEntity]() {
        scene.destroyEntity(actionEntity);
    }));
    // a restart: the stop destroys it, the start must not run on what is left
    action.stopTrigger = true;
    action.startTrigger = true;

    step(scene, 0.25, 2);
    EXPECT_FALSE(scene.isEntityCreated(actionEntity));
    EXPECT_TRUE(other.isRunning()) << "the other action is untouched";
    EXPECT_GT(other.getTimeCount(), 0.25f);
}

TEST(ActionTriggers, ChildOnStartMovingTheComponentArrays) {
    Scene scene;
    Entity target = createTarget(scene);

    PositionTween tween(&scene);
    tween.setAction(Vector3(0, 0, 0), Vector3(10, 0, 0), 1.0f);

    Animation animation(&scene);
    animation.addActionFrame(0.375f, 0.0f, tween.getEntity(), target);
    animation.start();

    // the child's start adds enough ActionComponents to move their array: animationUpdate
    // writes the child's timecount right after starting it
    std::vector<Entity> added;
    tween.getComponent<ActionComponent>().onStart.add("grow", std::function<void()>([&scene, &added]() {
        if (!added.empty()) return;
        for (int i = 0; i < 64; i++) {
            Entity e = scene.createEntity();
            scene.addComponent<ActionComponent>(e, ActionComponent());
            added.push_back(e);
        }
    }));

    // the animation reaches 0.5 s on the third update, 0.125 s into the child's frame
    step(scene, 0.25, 3);
    ASSERT_EQ(added.size(), 64u);
    EXPECT_TRUE(tween.isRunning());
    EXPECT_NEAR(positionOf(scene, target).x, 1.25f, 1e-4f) << "the child got its timecount after the move";
    EXPECT_TRUE(animation.isRunning());
    step(scene, 0.25, 1);
    EXPECT_NEAR(positionOf(scene, target).x, 3.75f, 1e-4f);
}
