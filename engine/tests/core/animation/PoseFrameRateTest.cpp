// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S48 R23 (docs/specs/S48-pixel-fx.md AC19): an Animator's Pose Frame Rate holds every track on
// whole steps from its previous key (each key's pose shows at its time, the pose changes at most
// fps + keys times a second), while root motion stays continuous and events fire at their true
// times.

#include <gtest/gtest.h>

#include "AnimatorTestUtil.h"

#include "component/HitStopExemptComponent.h"
#include "pixelart/HitStop.h"

#include <cmath>
#include <set>
#include <vector>

using namespace jasperengine;
using namespace animatortest;

namespace {

    constexpr double kDt = 1.0 / 60.0;

    class AnimatorInstance : public AnimatorFixture, public ::testing::Test {
    protected:
        // Spine x keyed at 0, 0.3, 0.7 (a 1 s loop held by Head), the hips walking 2 units a
        // second (root motion), an event at 0.35 s
        AnimatorComponent& setUp(float fps){
            ClipData data;
            data.wrap = ClipWrapMode::Loop;
            data.tracks = {
                track("Hips", ClipChannel::Position, {key(0, {0, 1, 0}), key(1, {0, 1, 2})}),
                track("Spine", ClipChannel::Position, {key(0, {0, 0.3f, 0}), key(0.3f, {3, 0.3f, 0}), key(0.7f, {1, 0.3f, 0})}),
                track("Head", ClipChannel::Position, {key(0, {0, 0.4f, 0}), key(1, {0, 0.4f, 0})}),
            };
            data.events = {event(0.35f, "mid")};
            AnimatorComponent& component = animator(controllerFile("c", controller({}, {state("Walk", clip("walk", data))})));
            component.applyRootMotion = true;
            component.poseFrameRate = fps;
            component.onEvent.add("test", [this](const AnimationEvent&){ eventFrames.push_back(frame); });
            return component;
        }

        int frame = 0;
        std::vector<int> eventFrames;
    };

}

TEST_F(AnimatorInstance, PoseFrameRateRootMotion){
    setUp(12.0f);
    step(0.0);
    ASSERT_TRUE(instance().bound);
    int changes = 0;
    float previous = position(spine).x;
    float previousRoot = position(root).z;
    for (frame = 1; frame <= 60; frame++){
        step(kDt);
        const float x = position(spine).x;
        if (x != previous) changes++;
        previous = x;
        // each key's pose exactly at its key time (frames 18 and 42 are 0.3 s and 0.7 s)
        if (frame == 18){
            EXPECT_NEAR(x, 3.0f, 1e-4f) << "the 0.3 s key";
        }
        if (frame == 42){
            EXPECT_NEAR(x, 1.0f, 1e-4f) << "the 0.7 s key";
        }
        // the root moves every frame by the same amount: no step
        const float rootZ = position(root).z;
        EXPECT_NEAR(rootZ - previousRoot, 2.0f / 60.0f, 2e-4f) << "frame " << frame;
        previousRoot = rootZ;
        // the hips (the root bone) stay in place in the pose: their motion moved the GameObject
        EXPECT_NEAR(position(hips).z, 0.0f, 1e-4f);
    }
    EXPECT_LE(changes, 12 + 3) << "held poses: at most fps + keys changes a second";
    EXPECT_GE(changes, 6);
    // the event at its true time (0.35 s = frame 21), not on a step
    ASSERT_EQ(eventFrames.size(), 1u);
    EXPECT_EQ(eventFrames[0], 21);
}

TEST_F(AnimatorInstance, PoseFrameRateSmoothIsTheRedControl){
    // Pose Frame Rate 0: the same clip moves the pose every frame
    setUp(0.0f);
    step(0.0);
    int changes = 0;
    float previous = position(spine).x;
    for (frame = 1; frame <= 60; frame++){
        step(kDt);
        if (position(spine).x != previous) changes++;
        previous = position(spine).x;
    }
    EXPECT_GT(changes, 40);
}

// S48 R21: a hit-stop freezes an Animator (the world's delta is 0) unless its GameObject is Hit
// Stop Exempt, which runs at the unfrozen delta
TEST_F(AnimatorInstance, HitStopExemptAnimatorRuns){
    setUp(0.0f);
    step(0.0);
    pixelart::HitStop::reset();
    pixelart::HitStop::play(1.0f);
    const float frozenStart = position(spine).x;
    for (int i = 0; i < 6; i++){
        // what Engine::runFrame gives the scene: the frame time times the hit's factor (0)
        const double world = kDt * pixelart::HitStop::beginFrame(kDt, kDt);
        step(world);
    }
    EXPECT_FLOAT_EQ(position(spine).x, frozenStart) << "frozen with the world";

    scene.addComponent<HitStopExemptComponent>(root, {});
    for (int i = 0; i < 6; i++){
        const double world = kDt * pixelart::HitStop::beginFrame(kDt, kDt);
        step(world);
    }
    EXPECT_GT(position(spine).x, frozenStart + 1e-3f) << "exempt: it runs through the freeze";
    pixelart::HitStop::reset();
}
