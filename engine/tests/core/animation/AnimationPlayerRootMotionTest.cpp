// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Root motion on the Animation Player (docs/specs/S94-rendering-anim-gaps.md F34, AC32): Apply
// Root Motion moves the GameObject by the clip's displacement across loops, the clip's Root
// Transform settings decide what stays in the pose, and a clip with a root_motion block plays in
// place with Apply Root Motion off. Written red first: the player had no root motion.

#include <gtest/gtest.h>

#include "AnimatorTestUtil.h"

#include "component/AnimationPlayerComponent.h"
#include "object/AnimationPlayer.h"
#include "subsystem/ActionSystem.h"

#include <cmath>

namespace AnimationPlayerRootMotionTest_cpp {

using namespace jasperengine;
using namespace jasperengine::anim;
using namespace animatortest;

namespace {

    constexpr float kPlayerRootDt = 1.0f / 64.0f;

    class AnimationPlayerRootMotionTest : public ::testing::Test, public AnimatorFixture {
    protected:
        AnimationPlayer play(const std::string& path, bool applyRootMotion){
            AnimationPlayer player(&scene, root);
            EXPECT_TRUE(player.addClip(path, "Walk"));
            player.setPlayAutomatically(true);
            player.setApplyRootMotion(applyRootMotion);
            return player;
        }

        void frames(int count){
            for (int i = 0; i < count; i++) scene.getSystem<ActionSystem>()->update(kPlayerRootDt);
        }

        // the hips walk 2 m along z per 1 s loop and bob 0.2 m
        std::string walk(const std::string& rootMotion){
            ClipData data;
            data.wrap = ClipWrapMode::Loop;
            data.tracks = {track("Hips", ClipChannel::Position, {key(0, {0, 1, 0}), key(0.5f, {0, 1.2f, 1}), key(1, {0, 1, 2})})};
            if (!rootMotion.empty()) data.unknownKeys.push_back({"root_motion", rootMotion});
            return clip("walk", data);
        }
    };

}

// AC32: three loops move the GameObject three times the clip's displacement; the hips stay
TEST_F(AnimationPlayerRootMotionTest, MovesTheRoot){
    play(walk(""), true);
    scene.getSystem<ActionSystem>()->update(0.0);
    float previous = position(root).z;
    for (int i = 0; i < 192; i++){
        frames(1);
        EXPECT_GE(position(root).z, previous - 1e-5f) << "no backward step at a wrap (frame " << i << ")";
        previous = position(root).z;
        EXPECT_NEAR(position(hips).z, 0.0f, 1e-4f) << "the pose keeps the hips in place";
    }
    EXPECT_NEAR(position(root).z, 6.0f, 0.01f) << "3 loops x 2 m";
    EXPECT_NEAR(position(root).y, 0.0f, 1e-5f) << "bake_position_y (the default) keeps the height in the pose";
}

// the GameObject turns with the clip's heading and moves along it
TEST_F(AnimationPlayerRootMotionTest, TurnsTheRoot){
    ClipData data;
    data.wrap = ClipWrapMode::Once;
    // the root bone is the one the clip positions (the Animator's rule): the hips held in place
    data.tracks = {track("Hips", ClipChannel::Position, {key(0, {0, 1, 0}), key(1, {0, 1, 0})}),
                   track("Hips", ClipChannel::Rotation, {key(0, {0, 0, 0, 1}), key(1, {0, 0.7071068f, 0, 0.7071068f})})};
    play(clip("turn", data), true);
    scene.getSystem<ActionSystem>()->update(0.0);
    frames(80);
    EXPECT_NEAR(std::fabs(rotation(root).dot(aboutY(90.0f))), 1.0f, 1e-4f) << "the 90 degree turn moved the GameObject";
    EXPECT_NEAR(std::fabs(rotation(hips).dot(Quaternion())), 1.0f, 1e-4f) << "and left the pose";
}

// Apply Root Motion off: a clip with a root_motion block plays in place, one without keeps its drift
TEST_F(AnimationPlayerRootMotionTest, ApplyOffPlaysInPlace){
    play(walk("{}"), false);
    scene.getSystem<ActionSystem>()->update(0.0);
    frames(40);
    EXPECT_LT(position(root).length(), 1e-6f);
    EXPECT_LT(std::fabs(position(hips).z), 0.001f) << "Unity: the unbaked root motion leaves the pose";
}

TEST_F(AnimationPlayerRootMotionTest, ApplyOffWithoutBlockKeepsTheDrift){
    play(walk(""), false);
    scene.getSystem<ActionSystem>()->update(0.0);
    frames(16);
    EXPECT_NEAR(position(hips).z, 0.5f, 1e-3f) << "an old clip plays as before";
    EXPECT_LT(position(root).length(), 1e-6f);
}

// a player saved before root motion (Legacy Root Drift, set by the scene and prefab migrations)
// plays a clip with a root_motion block as it did: the drift stays in the pose
TEST_F(AnimationPlayerRootMotionTest, LegacyRootDriftKeepsTheDrift){
    AnimationPlayer player = play(walk("{}"), false);
    player.setLegacyRootDrift(true);
    scene.getSystem<ActionSystem>()->update(0.0);
    frames(16);
    EXPECT_NEAR(position(hips).z, 0.5f, 1e-3f) << "the drift stays in the pose";
    EXPECT_LT(position(root).length(), 1e-6f);
}

// bake_position_xz: the horizontal motion stays in the pose, the GameObject does not move
TEST_F(AnimationPlayerRootMotionTest, BakeIntoPoseKeepsTheMotionInThePose){
    play(walk("{\"bake_position_xz\": true}"), true);
    scene.getSystem<ActionSystem>()->update(0.0);
    frames(16);
    EXPECT_NEAR(position(hips).z, 0.5f, 1e-3f);
    EXPECT_LT(position(root).length(), 1e-5f);
}

}  // namespace AnimationPlayerRootMotionTest_cpp
