// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Pose metrics (spec: pose animation phase 2, section 3.2): feet, contacts, balance, limit hits

#include "TestUtil.h"

#include "pose/PoseMetrics.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace posetest;
using H = HumanBone;

namespace {

    std::vector<const PoseRig*> everyRig(){
        std::vector<const PoseRig*> rigs;
        for (const TestRig* t : allRigs()){
            rigs.push_back(t->rig.get());
            rigs.push_back(t->skinned.get());
        }
        return rigs;
    }

    BodyPose with(std::initializer_list<std::pair<const char*, float>> angles){
        BodyPose body;
        for (const auto& [name, value] : angles){
            int index = 0;
            EXPECT_TRUE(dofFromName(name, index)) << name;
            body.angles[index] = value;
        }
        return body;
    }

    PoseMetrics metricsOf(const PoseRig& rig, const BodyPose& body, bool grounded = true, float lift = 0.0f){
        return computeMetrics(rig, rig.fromBodyPose(body, nullptr, grounded, lift), body);
    }

    bool hits(const PoseMetrics& m, const char* name){
        int index = 0;
        dofFromName(name, index);
        return std::find(m.limitHits.begin(), m.limitHits.end(), index) != m.limitHits.end();
    }

}

TEST(PoseMetrics, NeutralStandsBalancedOnBothFeet){
    for (const PoseRig* rig : everyRig()){
        const PoseMetrics m = metricsOf(*rig, BodyPose());
        EXPECT_TRUE(m.left.contact && m.right.contact);
        EXPECT_NEAR(m.left.height, 0.0f, 0.005f);
        EXPECT_NEAR(m.right.height, 0.0f, 0.005f);
        EXPECT_NEAR(m.lowest, 0.0f, 1e-4f);
        EXPECT_TRUE(m.balanced);
        EXPECT_GT(m.balanceMargin, 0.0f);
        // symmetric, a little above the hips' height ratio of a human (humanScale units)
        EXPECT_NEAR(m.centerOfMass.x, 0.0f, 1e-3f);
        EXPECT_GT(m.centerOfMass.y, 0.7f);
        EXPECT_LT(m.centerOfMass.y, 1.2f);
        // the left foot is on the character's left
        EXPECT_GT(m.left.position.x, 0.0f);
        EXPECT_LT(m.right.position.x, 0.0f);
        EXPECT_TRUE(m.limitHits.empty());
    }
}

TEST(PoseMetrics, OneFootStance){
    for (const TestRig* t : allRigs()){
        const PoseRig& rig = *t->rig;
        // left knee up: only the right foot touches, the centre of mass stays between the feet
        const BodyPose lifted = with({{"left_upper_leg.forward", 70}, {"left_lower_leg.bend", 80}});
        const PoseMetrics m = metricsOf(rig, lifted);
        EXPECT_TRUE(m.right.contact) << t->label;
        EXPECT_FALSE(m.left.contact) << t->label;
        EXPECT_GT(m.left.height, 0.1f) << t->label;
        EXPECT_FALSE(m.balanced) << t->label;
        EXPECT_LT(m.balanceMargin, 0.0f) << t->label;
        EXPECT_GT(m.centerOfMass.z, metricsOf(rig, BodyPose()).centerOfMass.z) << t->label << ": the lifted leg is forward";

        // the standing leg brought under the body (adducted by the hip's lateral offset) balances it
        const PoseMetrics neutral = metricsOf(rig, BodyPose());
        const Vec3d hip = toCanonical(rig, slotPosition(rig, rig.neutralPose(), H::RightUpperLeg));
        const Vec3d foot = toCanonical(rig, slotPosition(rig, rig.neutralPose(), H::RightFoot));
        const double adduct = radToDeg(std::asin(std::fabs(neutral.right.position.x * rig.humanScale()) / (hip - foot).length()));
        BodyPose stance = lifted;
        int out = 0, roll = 0;
        dofFromName("right_upper_leg.out", out);
        dofFromName("right_foot.roll", roll);
        stance.angles[out] = static_cast<float>(-adduct);
        stance.angles[roll] = static_cast<float>(-adduct);   // the sole flat again
        const PoseMetrics balanced = metricsOf(rig, stance);
        EXPECT_TRUE(balanced.right.contact) << t->label;
        EXPECT_TRUE(balanced.balanced) << t->label << " margin " << balanced.balanceMargin << " com " << balanced.centerOfMass.x
                                       << " foot " << balanced.right.position.x;
        EXPECT_GT(balanced.balanceMargin, 0.0f) << t->label;
    }
}

TEST(PoseMetrics, AirbornePose){
    for (const PoseRig* rig : everyRig()){
        const PoseMetrics m = metricsOf(*rig, BodyPose(), false, 0.5f);
        EXPECT_FALSE(m.left.contact || m.right.contact);
        EXPECT_NEAR(m.lowest, 0.5f, 1e-3f);
        EXPECT_NEAR(std::min(m.left.height, m.right.height), 0.5f, 0.005f);
        EXPECT_FALSE(m.balanced);
        // no support: minus the distance to the nearest foot point
        EXPECT_LT(m.balanceMargin, 0.0f);
        EXPECT_GT(m.balanceMargin, -0.2f);
    }
}

TEST(PoseMetrics, LimitHitsBeforeClamping){
    const PoseRig& tpose = *tposeRig().rig;
    const BodyPose body = with({{"left_lower_arm.bend", -20}, {"head.twist", 80}, {"neck.twist", 80}, {"left_upper_arm.raise", 30}});
    const PoseMetrics m = metricsOf(tpose, body);
    EXPECT_TRUE(hits(m, "left_lower_arm.bend"));
    EXPECT_TRUE(hits(m, "head.twist"));
    EXPECT_TRUE(hits(m, "neck.twist"));
    EXPECT_FALSE(hits(m, "left_upper_arm.raise"));
    EXPECT_EQ(m.limitHits.size(), 3u);
    // a rig without a neck does not report its DOFs
    const PoseMetrics minimal = metricsOf(*minimalRig().rig, body);
    EXPECT_FALSE(hits(minimal, "neck.twist"));
    EXPECT_TRUE(hits(minimal, "head.twist"));
}

TEST(PoseMetrics, LimitHitsTolerateReadBackNoise){
    // a frame clamped to a limit reads back a hair outside it (a straight knee at -0.003): only
    // more than LIMIT_HIT_TOLERANCE (0.05 degrees) outside is a hit, per DOF and for the swing cap
    const PoseRig& rig = *tposeRig().rig;
    EXPECT_FALSE(hits(metricsOf(rig, with({{"left_lower_leg.bend", -0.003f}})), "left_lower_leg.bend"));
    EXPECT_FALSE(hits(metricsOf(rig, with({{"left_lower_leg.bend", -0.04f}})), "left_lower_leg.bend"));
    EXPECT_TRUE(hits(metricsOf(rig, with({{"left_lower_leg.bend", -0.06f}})), "left_lower_leg.bend"));
    EXPECT_FALSE(hits(metricsOf(rig, with({{"left_lower_leg.bend", 155.04f}})), "left_lower_leg.bend"));
    EXPECT_TRUE(hits(metricsOf(rig, with({{"left_lower_leg.bend", 155.06f}})), "left_lower_leg.bend"));
    // the swing cap (179.5): a hips lean of 179.54 is noise, 179.6 a hit on both swing DOFs
    BodyPose lean = with({{"hips.lean", 179.54f}});
    EXPECT_TRUE(metricsOf(rig, lean).limitHits.empty());
    lean = with({{"hips.lean", 179.6f}});
    EXPECT_TRUE(hits(metricsOf(rig, lean), "hips.lean"));
    EXPECT_TRUE(hits(metricsOf(rig, lean), "hips.side"));
    EXPECT_FALSE(hits(metricsOf(rig, lean), "hips.turn"));
    // the swing is measured after the per-DOF clamp, as clampToLimits does: a knee bent 179.8
    // (clamped to 155, below the cap) is a bend hit only, not a side hit
    const BodyPose knee = with({{"left_lower_leg.bend", 179.8f}});
    const PoseMetrics kneeMetrics = metricsOf(rig, knee);
    BodyPose kneeClamped = knee;
    EXPECT_EQ(kneeMetrics.limitHits, clampToLimits(kneeClamped));
    EXPECT_TRUE(hits(kneeMetrics, "left_lower_leg.bend"));
    EXPECT_FALSE(hits(kneeMetrics, "left_lower_leg.side"));
    // and per-DOF values inside the hips ranges whose swing exceeds the cap hit both swing DOFs
    const BodyPose wide = with({{"hips.lean", 170.0f}, {"hips.side", 60.0f}});
    BodyPose wideClamped = wide;
    EXPECT_EQ(metricsOf(rig, wide).limitHits, clampToLimits(wideClamped));
    // a knee clamped to 0, realised and read back as the frames metrics do (toBodyPose)
    for (const TestRig* t : allRigs()){
        BodyPose bent = with({{"left_lower_leg.bend", -15.0f}, {"right_lower_leg.bend", -15.0f}, {"left_lower_arm.bend", -15.0f}});
        clampToLimits(bent);
        const HumanPose pose = t->rig->fromBodyPose(bent, nullptr, true, 0.0f);
        const PoseMetrics m = computeMetrics(*t->rig, pose, t->rig->toBodyPose(pose));
        EXPECT_TRUE(m.limitHits.empty()) << t->label << " " << m.limitHits.size();
    }
}

TEST(PoseMetrics, MirroredPoseMirrorsTheMetrics){
    const PoseRig& rig = *tposeRig().skinned;
    const BodyPose body = with({{"left_upper_leg.forward", 40}, {"left_lower_leg.bend", 30}, {"spine.side", 10},
                                {"right_upper_arm.raise", 60}});
    const PoseMetrics a = metricsOf(rig, body);
    const PoseMetrics b = metricsOf(rig, mirrorBodyPose(body));
    EXPECT_NEAR(a.centerOfMass.x, -b.centerOfMass.x, 1e-3f);
    EXPECT_NEAR(a.centerOfMass.y, b.centerOfMass.y, 1e-3f);
    EXPECT_NEAR(a.left.height, b.right.height, 1e-3f);
    EXPECT_EQ(a.left.contact, b.right.contact);
    EXPECT_EQ(a.balanced, b.balanced);
    EXPECT_NEAR(a.balanceMargin, b.balanceMargin, 1e-3f);
}
