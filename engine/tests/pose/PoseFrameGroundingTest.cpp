// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The frames' ground correction (spec: pose animation phase 1, section 4.8 frames): the hips are
// interpolated linearly while the legs swing, so a fast springy move between a deep crouch and
// standing used to sink the feet into the floor between the keys

#include "TestUtil.h"

#include "pose/PoseIK.h"
#include "pose/PoseSampler.h"

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

TEST(PoseFrameGrounding, JumpLaunchIsNotPulledDown){
    // crouch (grounded) -> airborne (lift 0.4) -> crouch: the segments with one grounded end are
    // only pushed up out of the floor; frames above it (the launch, the flight) are untouched
    for (const PoseRig* rig : everyRig()){
        PoseAnimationData data = springy({crouch(), bodyWith({{"left_upper_leg.forward", 30}, {"right_upper_leg.forward", 30}}), crouch()}, false);
        data.poses[1].grounded = false;
        data.poses[1].lift = 0.4f;
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
        int untouched = 0, flight = 0, landed = 0;
        for (int i = 0; i <= count; i++){
            const double t = i * length / count;
            EXPECT_GT(h[i], -1e-5) << "frame " << i;   // never below the floor
            const bool groundedHold = t < timeline.start[0] || t >= timeline.start[1] + 0.2;
            if (groundedHold){
                // the holds of the grounded keys: on the floor exactly (the landing too, while
                // the springs still bounce the body up)
                EXPECT_NEAR(h[i], 0.0, 1e-5) << "frame " << i;
                landed++;
                continue;
            }
            if (t >= timeline.start[0] + 0.2 && t < timeline.start[1]){
                EXPECT_GT(h[i], 0.25) << "frame " << i;   // the airborne key's hold (the spring still settling)
                flight++;
            }
            if (rawH[i] >= 0.0){
                // a segment frame above the floor without the correction (the launch, the
                // flight, the fall): the same frame
                const Vec3d moved = Vec3d(rig->hipsWorldPosition(f[i])) - Vec3d(rig->hipsWorldPosition(raw[i]));
                EXPECT_LT(moved.length(), 1e-6 * rig->humanScale()) << "frame " << i;
                untouched++;
            }else{
                EXPECT_NEAR(h[i], 0.0, 1e-5) << "frame " << i;   // pushed up exactly to the floor
            }
        }
        EXPECT_GT(flight, 3);
        EXPECT_GT(landed, 10);
        EXPECT_GT(untouched, count / 3);
        // the launch: the frames of segment 0 after the anticipation rise above the floor
        int launch = 0;
        for (int i = 0; i <= count; i++){
            const double t = i * length / count;
            if (t > timeline.start[0] + 0.08 && t < timeline.start[0] + 0.2 && h[i] > 0.05) launch++;
        }
        EXPECT_GT(launch, 2);
    }
}
