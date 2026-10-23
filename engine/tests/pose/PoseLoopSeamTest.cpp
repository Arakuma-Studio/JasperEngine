// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Review round 1 regression (spec 4.8 step 4, previous-cycle settling): a looping animation is
// continuous for every bone across its wrap time (t = overlap delay), at t = 0 and at t = len,
// for every feel preset, including loose and high-overshoot ones

#include "TestUtil.h"

#include "pose/PoseSampler.h"
#include "pose/PoseTween.h"

#include <gtest/gtest.h>

using namespace posetest;
using H = HumanBone;

namespace {

    std::vector<BodyPose> seamBodies(){
        return {
            bodyWith({{"left_upper_arm.raise", 10}, {"left_lower_arm.bend", 10}}),
            bodyWith({{"left_upper_arm.raise", 120}, {"left_lower_arm.bend", 90}, {"spine.bend", 15}, {"head.side", 20}}),
            bodyWith({{"left_upper_arm.raise", 30}, {"left_upper_arm.forward", 60}, {"hips.turn", 25}}),
            bodyWith({{"left_upper_arm.raise", 80}, {"left_lower_arm.bend", 120}, {"left_hand.bend", 50}}),
        };
    }

    struct Steps {
        double worst[HUMAN_BONE_COUNT] = {};
        double worstAt[HUMAN_BONE_COUNT] = {};
        double hips = 0.0;
    };

    // per-bone largest rotation between consecutive samples, dt apart, over [from, to]
    Steps denseSteps(const PoseRig& rig, const SamplerInput& input, double from, double to, double dt){
        Steps steps;
        HumanPose previous, current;
        sample(input, static_cast<float>(from), previous);
        const int count = static_cast<int>((to - from) / dt);
        for (int i = 1; i <= count; i++){
            const double t = from + i * dt;
            sample(input, static_cast<float>(t), current);
            for (int b = 0; b < HUMAN_BONE_COUNT; b++){
                const double step = radToDeg(rotationError(previous.rotations[b], current.rotations[b]));
                if (step > steps.worst[b]){
                    steps.worst[b] = step;
                    steps.worstAt[b] = t;
                }
            }
            steps.hips = std::max(steps.hips, (Vec3d(rig.hipsWorldPosition(previous)) - Vec3d(rig.hipsWorldPosition(current))).length());
            previous = current;
        }
        return steps;
    }

}

TEST(PoseLoopSeam, ContinuousAcrossEveryWrapForEveryPreset){
    const PoseRig& rig = *tposeRig().rig;
    for (const FeelPreset& preset : feelPresets()){
        for (bool humanize : {false, true}){
            PoseAnimationData data = makeAnimation(seamBodies(), true, 0.1f);
            data.poses[2].body.offset = Vector3(0.1f, 0, 0.05f);
            applyFeelPreset(preset, data.settings);
            // choppiness quantises time on purpose (Stop-motion): the curve under it must be seamless
            data.settings.stepRate = 0.0f;
            data.settings.weights = {1.0f, 1.0f, 1.0f, 0.5f};
            data.settings.humanize.enabled = humanize;
            data.settings.humanize.movingHolds = 0.2f;
            data.settings.humanize.life = 1.0f;
            const SamplerInput input = makeSamplerInput(data, realiseKeys(data, rig), &rig);
            const double length = input.timeline.length;
            // the whole loop, from before 0 to past len (a step of 2.5e-4 s: the fastest spring
            // moves below 1.5 degrees in it; the seam jumps were 12 to 35 degrees)
            const Steps all = denseSteps(rig, input, -0.02, length + 0.02, 2.5e-4);
            for (int b = 0; b < HUMAN_BONE_COUNT; b++){
                EXPECT_LT(all.worst[b], 1.5) << preset.name << (humanize ? " humanize " : " ") << humanBoneName(boneAt(b))
                                             << " jumps at t = " << all.worstAt[b] << " (len " << length << ")";
            }
            EXPECT_LT(all.hips, 2e-3 * rig.humanScale()) << preset.name;
            // around each bone's wrap time and around 0 / len, finer still
            for (H bone : {H::LeftUpperArm, H::LeftLowerArm, H::LeftHand, H::Head, H::Spine}){
                const HumanBoneInfo& info = humanBoneInfo(bone);
                const double delay = data.settings.overlap * data.settings.weights[static_cast<int>(info.group)] * info.depth;
                for (double at : {delay, delay + length, 0.0, length}){
                    const Steps near = denseSteps(rig, input, at - 0.001, at + 0.001, 1e-5);
                    EXPECT_LT(near.worst[boneIndex(bone)], 0.1) << preset.name << " " << humanBoneName(bone) << " at " << at;
                }
            }
            // and sample(0) == sample(len)
            HumanPose a, b;
            sample(input, 0.0f, a);
            sample(input, static_cast<float>(length), b);
            for (int s = 0; s < HUMAN_BONE_COUNT; s++){
                EXPECT_LT(radToDeg(rotationError(a.rotations[s], b.rotations[s])), 0.01) << preset.name;
            }
        }
    }
}
