// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The realisation the editor's pose animation document caches (spec: pose animation phase 2,
// section 5): realised keys, per-key metrics and frames on one rig

#include "TestUtil.h"

#include "pose/PoseRealisation.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace posetest;

namespace {

    int dof(const char* name){
        int index = 0;
        EXPECT_TRUE(dofFromName(name, index)) << name;
        return index;
    }

    PoseAnimationData twoPoses(){
        PoseAnimationData data;
        data.settings.loop = false;   // one-shot: frame 0 is pose 0 exactly (a loop starts with the spring's residual)
        data.poses.resize(2);
        data.poses[0].name = "Stand";
        data.poses[1].name = "Elbow back";
        data.poses[1].body.angles[dof("left_lower_arm.bend")] = -20.0f;   // below the 0..155 limit
        data.poses[1].body.angles[dof("right_upper_leg.forward")] = 30.0f;
        return data;
    }

}

TEST(PoseRealisation, KeysMetricsAndFrames){
    const std::shared_ptr<PoseRig> rig = tposeRig().skinned;
    const PoseAnimationData data = twoPoses();
    const std::shared_ptr<PoseRealisation> realised = realisePoseAnimation(data, rig);
    ASSERT_NE(realised, nullptr);
    EXPECT_EQ(realised->rig, rig);
    ASSERT_EQ(realised->keys.size(), 2u);
    ASSERT_EQ(realised->metrics.size(), 2u);

    // frames: count = round(length * fps) + 1, the length of the timeline
    const PoseTimeline timeline = buildTimeline(data);
    EXPECT_DOUBLE_EQ(realised->timeline.length, timeline.length);
    const size_t count = static_cast<size_t>(std::max(1L, std::lround(timeline.length * data.settings.frameRate)));
    EXPECT_EQ(realised->frames.size(), count + 1);

    // keys are realised with the joint limits first (joint_limits is on by default)...
    BodyPose clamped = data.poses[1].body;
    clampToLimits(clamped);
    const HumanPose expected = rig->fromBodyPose(clamped, rig->skin(), true, 0.0f);
    for (int b = 0; b < HUMAN_BONE_COUNT; b++){
        EXPECT_LT(angleBetween(realised->keys[1].rotations[b], expected.rotations[b]), 1e-5f) << b;
    }
    // ...while the metrics report the key's own angles outside the limits
    ASSERT_EQ(realised->metrics[1].limitHits.size(), 1u);
    EXPECT_EQ(realised->metrics[1].limitHits[0], dof("left_lower_arm.bend"));
    EXPECT_TRUE(realised->metrics[0].limitHits.empty());
    EXPECT_TRUE(realised->metrics[0].balanced);
    EXPECT_NEAR(realised->metrics[1].lowest, 0.0f, 1e-4f);

    // the first frame is the first key (one-shot, no humanize)
    for (int b = 0; b < HUMAN_BONE_COUNT; b++){
        EXPECT_LT(angleBetween(realised->frames.front().rotations[b], realised->keys[0].rotations[b]), 1e-4f) << b;
    }
}

TEST(PoseRealisation, NoRigNoRealisation){
    EXPECT_EQ(realisePoseAnimation(twoPoses(), nullptr), nullptr);
    EXPECT_EQ(realisePoseAnimation(twoPoses(), std::make_shared<PoseRig>()), nullptr);
}

TEST(PoseRealisation, EmptyAnimation){
    const std::shared_ptr<PoseRealisation> realised = realisePoseAnimation(PoseAnimationData(), tposeRig().rig);
    ASSERT_NE(realised, nullptr);
    EXPECT_TRUE(realised->keys.empty());
    EXPECT_TRUE(realised->metrics.empty());
    EXPECT_TRUE(realised->frames.empty());
}
