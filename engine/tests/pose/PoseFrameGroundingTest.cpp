// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The frames' ground correction (spec: pose animation phase 1, section 4.8 frames): the hips are
// interpolated linearly while the legs swing, so a fast springy move between a deep crouch and
// standing used to sink the feet into the floor between the keys

#include "TestUtil.h"

#include "pose/PoseIK.h"
#include "pose/PoseSampler.h"
#include "pose/PoseSecondary.h"
#include "pose/PoseTween.h"

#include <gtest/gtest.h>

using namespace posetest;

namespace {

    BodyPose crouch(){
        return bodyWith({{"left_upper_leg.forward", 100}, {"right_upper_leg.forward", 100},
                         {"left_lower_leg.bend", 125}, {"right_lower_leg.bend", 125},
                         {"left_foot.point", -30}, {"right_foot.point", -30}, {"spine.bend", 25}, {"chest.bend", 15}});
    }

    PoseAnimationData springy(const std::vector<BodyPose>& bodies, bool loop){
        PoseAnimationData data = makeAnimation(bodies, loop, 0.15f);
        data.settings.transition.curve = TransitionCurve::Spring;   // the Rubbery feel: fast and springy
        data.settings.transition.duration = 0.2f;
        data.settings.transition.overshoot = 0.5f;
        data.settings.transition.anticipation = 0.1f;
        data.settings.frameRate = 60;
        return data;
    }

    // height of every frame's lowest point above the floor, humanScale units
    std::vector<double> heights(const PoseRig& rig, const std::vector<HumanPose>& frames){
        std::vector<double> h;
        for (const HumanPose& frame : frames){
            h.push_back((rig.lowestPointPrecise(frame, rig.skin()) - rig.floorHeight()) / rig.humanScale());
        }
        return h;
    }

    // a moderate crouch (the feet stay the lowest point) and the airborne tuck of a jump
    BodyPose bendPose(){
        return bodyWith({{"left_upper_leg.forward", 50}, {"right_upper_leg.forward", 50},
                         {"left_lower_leg.bend", 80}, {"right_lower_leg.bend", 80}});
    }

    BodyPose tuckPose(){
        return bodyWith({{"left_upper_leg.forward", 70}, {"right_upper_leg.forward", 70},
                         {"left_lower_leg.bend", 100}, {"right_lower_leg.bend", 100}});
    }

    std::vector<const PoseRig*> everyRig(){
        std::vector<const PoseRig*> rigs;
        for (const TestRig* t : allRigs()){
            rigs.push_back(t->rig.get());
            rigs.push_back(t->skinned.get());
        }
        return rigs;
    }

}

TEST(PoseFrameGrounding, CrouchToStandStaysOnTheFloor){
    // both keys grounded: every frame (holds and the springy segment) touches the floor exactly,
    // without and with humanize (in-betweens, follow-through, limits come before the correction)
    // and with choppiness
    for (const PoseRig* rig : everyRig()){
        for (int variant = 0; variant < 3; variant++){
            const bool humanize = variant >= 1;
            const float stepRate = variant == 2 ? 12.0f : 0.0f;   // choppy: frames on the step grid
            for (bool loop : {false, true}){
                PoseAnimationData data = springy({crouch(), BodyPose()}, loop);
                data.settings.humanize.enabled = humanize;
                data.settings.humanize.life = 0.0f;
                data.settings.stepRate = stepRate;
                const SamplerInput input = makeSamplerInput(data, realiseKeys(data, *rig), rig);
                const std::vector<HumanPose> f = frames(input, 0);
                ASSERT_GT(f.size(), 20u);
                const std::vector<double> h = heights(*rig, f);
                for (size_t i = 0; i < h.size(); i++){
                    EXPECT_NEAR(h[i], 0.0, 1e-5) << "variant " << variant << " loop " << loop << " frame " << i;
                }
                if (loop){
                    EXPECT_EQ(f.back().hipsPosition, f.front().hipsPosition);
                }
                // without the correction the same frames sink (the regression this guards)
                SamplerInput uncorrected = input;
                uncorrected.grounded.clear();
                const std::vector<double> raw = heights(*rig, frames(uncorrected, 0));
                EXPECT_LT(*std::min_element(raw.begin(), raw.end()), -0.01);
            }
        }
    }
}

TEST(PoseFrameGrounding, JumpNeverPenetratesAndKeepsTheFlight){
    // bend (grounded) -> tuck (lift 0.3) -> bend: no frame is below the floor, the grounded holds
    // are on it, the airborne key's hold is untouched (ground weight 0, above the floor), no
    // frame is raised above its uncorrected height unless it was below the floor, and the flight
    // reaches its uncorrected height
    for (const PoseRig* rig : everyRig()){
        PoseAnimationData data = springy({bendPose(), tuckPose(), bendPose()}, false);
        data.poses[1].grounded = false;
        data.poses[1].lift = 0.3f;
        const SamplerInput input = makeSamplerInput(data, realiseKeys(data, *rig), rig);
        SamplerInput uncorrected = input;
        uncorrected.grounded.clear();
        const std::vector<HumanPose> f = frames(input, 0);
        const std::vector<HumanPose> raw = frames(uncorrected, 0);
        ASSERT_EQ(f.size(), raw.size());
        const std::vector<double> h = heights(*rig, f), rawH = heights(*rig, raw);
        const PoseTimeline& timeline = input.timeline;
        const double length = timeline.length;
        const int count = static_cast<int>(f.size()) - 1;
        int landed = 0, flight = 0;
        double peak = 0.0, rawPeak = 0.0;
        for (int i = 0; i <= count; i++){
            const double t = i * length / count;
            EXPECT_GT(h[i], -1e-5) << "frame " << i;
            EXPECT_LT(h[i], std::max(rawH[i], 0.0) + 1e-5) << "frame " << i;
            peak = std::max(peak, h[i]);
            rawPeak = std::max(rawPeak, rawH[i]);
            if (t < timeline.start[0] || t >= timeline.start[1] + 0.2){
                EXPECT_NEAR(h[i], 0.0, 1e-5) << "frame " << i;   // the grounded holds (the landing too)
                landed++;
            }else if (t >= timeline.start[0] + 0.2 && t < timeline.start[1]){
                const Vec3d moved = Vec3d(rig->hipsWorldPosition(f[i])) - Vec3d(rig->hipsWorldPosition(raw[i]));
                EXPECT_LT(moved.length(), 1e-6 * rig->humanScale()) << "frame " << i;   // the lifted hold
                EXPECT_GT(h[i], 0.15) << "frame " << i;
                flight++;
            }
        }
        EXPECT_GT(landed, 10);
        EXPECT_GT(flight, 3);
        EXPECT_GT(peak, 0.99 * rawPeak);
    }
}

TEST(PoseFrameGrounding, CorrectionIsContinuousAcrossHoldsAndSegments){
    // reviewer probe: an exact correction in a grounded hold followed by a push-up-only launch
    // segment popped 0.023 to 0.08 humanScale in one frame (moving holds, a still-bouncing
    // spring). With the ground weight continuous (fading out over the launch and in over the
    // landing) the correction (corrected minus uncorrected hips height) changes per frame no
    // faster than the uncorrected height of the lowest point does nearby: jumps with a hold
    // between the landing and the relaunch, moving holds on, every feel preset, loops
    int checked = 0;
    double worst = 0.0;
    for (const PoseRig* rig : everyRig()){
        const double hs = rig->humanScale();
        for (const FeelPreset& preset : feelPresets()){
            for (bool loop : {false, true}){
                PoseAnimationData data = makeAnimation({bendPose(), tuckPose(), bendPose(), tuckPose()}, loop, 0.2f);
                if (!loop){
                    PoseKey last = data.poses[0];
                    data.poses.push_back(last);
                }
                applyFeelPreset(preset, data.settings);
                data.settings.frameRate = 60;
                data.settings.humanize.enabled = true;   // moving holds 0.08, in-betweens, follow-through
                data.settings.humanize.life = 0.0f;
                for (int k : {1, 3}){
                    data.poses[k].grounded = false;
                    data.poses[k].lift = 0.3f;
                }
                const SamplerInput input = makeSamplerInput(data, realiseKeys(data, *rig), rig);
                SamplerInput uncorrected = input;
                uncorrected.grounded.clear();
                const std::vector<HumanPose> f = frames(input, 0);
                const std::vector<HumanPose> raw = frames(uncorrected, 0);
                const std::vector<double> h = heights(*rig, f), rawH = heights(*rig, raw);
                const int count = static_cast<int>(f.size()) - 1;
                std::vector<double> offset(f.size()), rawStep(f.size(), 0.0);
                for (int i = 0; i <= count; i++){
                    offset[i] = (rig->hipsWorldPosition(f[i]).y - rig->hipsWorldPosition(raw[i]).y) / hs;
                    if (i > 0) rawStep[i] = std::fabs(rawH[i] - rawH[i - 1]);
                }
                for (int i = 1; i <= count; i++){
                    double nearby = 0.0;
                    for (int j = std::max(i - 1, 1); j <= std::min(i + 1, count); j++){
                        nearby = std::max(nearby, rawStep[j]);
                    }
                    const double change = std::fabs(offset[i] - offset[i - 1]);
                    EXPECT_LE(change, 1.5 * nearby + 0.005) << preset.name << " loop " << loop << " frame " << i
                        << " correction " << offset[i - 1] << " -> " << offset[i] << ", lowest point steps nearby " << nearby;
                    EXPECT_GT(h[i], -1e-5) << preset.name << " loop " << loop << " frame " << i;
                    worst = std::max(worst, change - 1.5 * nearby);
                    checked++;
                }
            }
        }
    }
    std::printf("[ info ] worst correction change over 1.5 x the nearby lowest-point step: %+.5f humanScale\n", worst);
    EXPECT_GT(checked, 1000);
}

TEST(PoseFrameGrounding, FollowThroughFollowsTheCorrectedHips){
    // the correction runs before follow-through, so the torso and head tilts are those of the
    // corrected hips path; a final pass re-grounds the frames follow-through changed
    int compared = 0;
    for (const PoseRig* rig : everyRig()){
        PoseAnimationData data = springy({bendPose(), tuckPose(), bendPose()}, false);
        data.poses[1].grounded = false;
        data.poses[1].lift = 0.3f;
        data.settings.humanize.enabled = true;
        data.settings.humanize.life = 0.0f;
        data.settings.humanize.inbetweens = 0.0f;
        data.settings.humanize.movingHolds = 0.0f;
        data.settings.humanize.jointLimits = false;
        data.settings.humanize.torsoFollow = 1.0f;
        data.settings.humanize.headFollow = 1.0f;
        const SamplerInput input = makeSamplerInput(data, realiseKeys(data, *rig), rig);
        const std::vector<HumanPose> f = frames(input, 0);
        // the corrected path without follow-through, then follow-through on it
        SamplerInput still = input;
        still.settings.humanize.torsoFollow = 0.0f;
        still.settings.humanize.headFollow = 0.0f;
        const std::vector<HumanPose> stillFrames = frames(still, 0);
        std::vector<HumanPose> expected = stillFrames;
        applyFollowThrough(expected, *rig, data.settings.frameRate, false, data.settings.humanize);
        ASSERT_EQ(f.size(), expected.size());
        bool tilted = false;
        for (size_t i = 0; i < f.size(); i++){
            for (int b = 1; b < HUMAN_BONE_COUNT; b++){
                EXPECT_LT(rotationError(f[i].rotations[b], expected[i].rotations[b]), 1e-6) << "frame " << i << " bone " << b;
                tilted = tilted || rotationError(f[i].rotations[b], stillFrames[i].rotations[b]) > 1e-3;
            }
            compared++;
        }
        EXPECT_TRUE(tilted);
        // the grounded holds are still on the floor after follow-through moved the torso
        const std::vector<double> h = heights(*rig, f);
        const PoseTimeline& timeline = input.timeline;
        const int count = static_cast<int>(f.size()) - 1;
        for (int i = 0; i <= count; i++){
            const double t = i * timeline.length / count;
            if (t < timeline.start[0] || t >= timeline.start[1] + 0.2){
                EXPECT_NEAR(h[i], 0.0, 1e-5) << "frame " << i;
            }
        }
    }
    EXPECT_GT(compared, 100);
}
