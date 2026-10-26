// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Pose Animation window's logic without ImGui (editor/pose/PoseUiLogic, phase 4 section 4)

#include "pose/TestUtil.h"

#include "pose/PoseUiLogic.h"

#include "pose/PoseDofs.h"
#include "pose/PoseRanges.h"
#include "pose/PoseSampler.h"
#include "pose/PoseTween.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::pose;
namespace poseui = jasperengine::editor::poseui;
namespace editor = jasperengine::editor;

namespace {

    PoseAnimationData animationOf(int count, float hold = 0.1f){
        PoseAnimationData data;
        for (int i = 0; i < count; i++){
            PoseKey key;
            key.name = "P" + std::to_string(i);
            key.hold = hold;
            key.body.angles[dofIndex(HumanBone::LeftUpperArm, 0)] = 10.0f * i;
            data.poses.push_back(key);
        }
        return data;
    }

    std::vector<std::string> names(const PoseAnimationData& data){
        std::vector<std::string> out;
        for (const PoseKey& key : data.poses) out.push_back(key.name);
        return out;
    }

    int dof(const char* name){
        int index = -1;
        EXPECT_TRUE(dofFromName(name, index)) << name;
        return index;
    }

}

// ---- names and labels ----

TEST(PoseUiLogic, MirrorPoseName){
    EXPECT_EQ(poseui::mirrorPoseName("Contact R"), "Contact L");
    EXPECT_EQ(poseui::mirrorPoseName("Contact L"), "Contact R");
    EXPECT_EQ(poseui::mirrorPoseName("Left Kick"), "Right Kick");
    EXPECT_EQ(poseui::mirrorPoseName("right kick"), "left kick");
    EXPECT_EQ(poseui::mirrorPoseName("RIGHT"), "LEFT");
    EXPECT_EQ(poseui::mirrorPoseName("Step.L"), "Step.R");
    EXPECT_EQ(poseui::mirrorPoseName("step_r"), "step_l");
    EXPECT_EQ(poseui::mirrorPoseName("Passing-L-2"), "Passing-R-2");
    EXPECT_EQ(poseui::mirrorPoseName("LeftKick"), "RightKick");
    EXPECT_EQ(poseui::mirrorPoseName("(R)"), "(L)");
    // not a side: letters inside words, other words
    EXPECT_EQ(poseui::mirrorPoseName("Run"), "Run");
    EXPECT_EQ(poseui::mirrorPoseName("Leftover Rest"), "Leftover Rest");
    EXPECT_EQ(poseui::mirrorPoseName("Pose 3"), "Pose 3");
    EXPECT_EQ(poseui::mirrorPoseName(""), "");
    // twice is the identity
    for (const char* name : {"Contact R", "Left Kick", "Step.L", "a l b r"}){
        EXPECT_EQ(poseui::mirrorPoseName(poseui::mirrorPoseName(name)), name);
    }
}

TEST(PoseUiLogic, NewPoseNameIsUnique){
    PoseAnimationData data = animationOf(2);
    EXPECT_EQ(poseui::newPoseName(data.poses), "Pose 3");
    data.poses[0].name = "Pose 3";
    EXPECT_EQ(poseui::newPoseName(data.poses), "Pose 4");
    EXPECT_EQ(poseui::newPoseName({}), "Pose 1");
}

TEST(PoseUiLogic, RigStatus){
    std::array<std::string, HUMAN_BONE_COUNT> bones;
    for (int b = 0; b < HUMAN_BONE_COUNT; b++) bones[b] = std::string("bone") + std::to_string(b);
    poseui::RigStatus status = poseui::rigStatus(bones);
    EXPECT_EQ(status.text, "Humanoid 22/22");
    EXPECT_FALSE(status.warning);

    bones[boneIndex(HumanBone::UpperChest)] = "";
    status = poseui::rigStatus(bones);
    EXPECT_EQ(status.text, "Humanoid 21/22");
    EXPECT_FALSE(status.warning);
    EXPECT_NE(status.tooltip.find("upper_chest"), std::string::npos);

    bones[boneIndex(HumanBone::LeftHand)] = "";
    bones[boneIndex(HumanBone::Head)] = "";
    status = poseui::rigStatus(bones);
    EXPECT_EQ(status.text, "Missing: head, left_hand");
    EXPECT_TRUE(status.warning);
}

// ---- selection ----

TEST(PoseUiLogic, SelectClickToggleRange){
    poseui::Selection sel;
    poseui::selectPose(sel, 2, false, false, 6);
    EXPECT_EQ(sel.primary, 2);
    EXPECT_EQ(sel.indices, (std::vector<int>{2}));

    poseui::selectPose(sel, 4, true, false, 6);   // Ctrl: add
    EXPECT_EQ(sel.indices, (std::vector<int>{2, 4}));
    EXPECT_EQ(sel.primary, 4);
    poseui::selectPose(sel, 4, true, false, 6);   // Ctrl again: remove, the primary moves
    EXPECT_EQ(sel.indices, (std::vector<int>{2}));
    EXPECT_EQ(sel.primary, 2);
    poseui::selectPose(sel, 2, true, false, 6);   // the last selected pose stays
    EXPECT_EQ(sel.indices, (std::vector<int>{2}));

    poseui::selectPose(sel, 5, false, true, 6);   // Shift: range from the anchor
    EXPECT_EQ(sel.indices, (std::vector<int>{2, 3, 4, 5}));
    EXPECT_EQ(sel.primary, 5);
    poseui::selectPose(sel, 0, false, true, 6);   // the anchor stays at 2
    EXPECT_EQ(sel.indices, (std::vector<int>{0, 1, 2}));

    poseui::selectPose(sel, 9, false, false, 6);  // out of range: ignored
    EXPECT_EQ(sel.primary, 0);
}

TEST(PoseUiLogic, ClampSelection){
    poseui::Selection sel;
    sel.indices = {1, 4, 5};
    sel.primary = 5;
    sel.anchor = 5;
    poseui::clampSelection(sel, 4);
    EXPECT_EQ(sel.primary, 3);
    EXPECT_EQ(sel.indices, (std::vector<int>{1, 3}));
    poseui::Selection empty;
    poseui::clampSelection(empty, 3);
    EXPECT_EQ(empty.primary, 0);
    EXPECT_EQ(empty.indices, (std::vector<int>{0}));
    poseui::clampSelection(sel, 0);
    EXPECT_EQ(sel.primary, -1);
    EXPECT_TRUE(sel.indices.empty());
}

// ---- strip reorder ----

TEST(PoseUiLogic, ReorderForDrop){
    // move pose 1 before pose 4: 0 2 3 1 4
    EXPECT_EQ(poseui::reorderForDrop(5, {1}, 4), (std::vector<int>{0, 2, 3, 1, 4}));
    // to the end
    EXPECT_EQ(poseui::reorderForDrop(5, {0}, 5), (std::vector<int>{1, 2, 3, 4, 0}));
    // to the front
    EXPECT_EQ(poseui::reorderForDrop(5, {3}, 0), (std::vector<int>{3, 0, 1, 2, 4}));
    // a multi-selection moves together, keeping its order
    EXPECT_EQ(poseui::reorderForDrop(6, {4, 1}, 6), (std::vector<int>{0, 2, 3, 5, 1, 4}));
    EXPECT_EQ(poseui::reorderForDrop(6, {1, 4}, 0), (std::vector<int>{1, 4, 0, 2, 3, 5}));
    // dropped next to itself: nothing moves
    EXPECT_TRUE(poseui::reorderForDrop(5, {2}, 2).empty());
    EXPECT_TRUE(poseui::reorderForDrop(5, {2}, 3).empty());
    EXPECT_TRUE(poseui::reorderForDrop(5, {}, 3).empty());
    // positions after the move
    const std::vector<int> order = poseui::reorderForDrop(6, {1, 4}, 6);
    EXPECT_EQ(poseui::newPositions(order, {1, 4}), (std::vector<int>{4, 5}));
    EXPECT_EQ(poseui::newPosition(order, 0), 0);
}

TEST(PoseUiLogic, ReorderForShift){
    EXPECT_EQ(poseui::reorderForShift(4, {1}, -1), (std::vector<int>{1, 0, 2, 3}));
    EXPECT_EQ(poseui::reorderForShift(4, {1}, 1), (std::vector<int>{0, 2, 1, 3}));
    EXPECT_EQ(poseui::reorderForShift(5, {1, 2}, 1), (std::vector<int>{0, 3, 1, 2, 4}));
    EXPECT_TRUE(poseui::reorderForShift(4, {0}, -1).empty());
    EXPECT_TRUE(poseui::reorderForShift(4, {3}, 1).empty());
}

TEST(PoseUiLogic, InsertionIndexAndDragThreshold){
    const std::vector<float> centres = {50, 160, 270};
    EXPECT_EQ(poseui::insertionIndexAt(centres, 10), 0);
    EXPECT_EQ(poseui::insertionIndexAt(centres, 100), 1);
    EXPECT_EQ(poseui::insertionIndexAt(centres, 200), 2);
    EXPECT_EQ(poseui::insertionIndexAt(centres, 500), 3);
    EXPECT_FALSE(poseui::dragStarted(3, 4.9f));
    EXPECT_TRUE(poseui::dragStarted(6, 0));
    EXPECT_TRUE(poseui::dragStarted(-4.5f, 4.5f));
}

// ---- pose edits ----

TEST(PoseUiLogic, DuplicateAndAdd){
    PoseAnimationData data = animationOf(3);
    data.poses[1].name = "Contact R";
    poseui::Edit edit = poseui::duplicatePoses(data, {1}, false, false);
    ASSERT_TRUE(edit.changed);
    EXPECT_EQ(names(edit.data), (std::vector<std::string>{"P0", "Contact R", "Contact R", "P2"}));
    EXPECT_EQ(edit.selection, (std::vector<int>{2}));
    EXPECT_EQ(edit.primary, 2);

    edit = poseui::duplicatePoses(data, {0, 1}, true, false);
    EXPECT_EQ(names(edit.data), (std::vector<std::string>{"P0", "Contact R", "P2", "P0", "Contact R"}));
    EXPECT_EQ(edit.selection, (std::vector<int>{3, 4}));

    edit = poseui::duplicatePoses(data, {1}, false, true);
    EXPECT_EQ(names(edit.data), (std::vector<std::string>{"P0", "Contact R", "Contact L", "P2"}));
    const BodyPose mirrored = mirrorBodyPose(data.poses[1].body);
    EXPECT_EQ(edit.data.poses[2].body.angles, mirrored.angles);

    // "+" duplicates the primary pose after it, an empty animation gets a neutral pose
    edit = poseui::addPose(data, 0);
    EXPECT_EQ(names(edit.data), (std::vector<std::string>{"P0", "P0", "Contact R", "P2"}));
    EXPECT_EQ(edit.primary, 1);
    edit = poseui::addPose(PoseAnimationData(), -1);
    ASSERT_EQ(edit.data.poses.size(), 1u);
    EXPECT_EQ(edit.data.poses[0].name, "Pose 1");
    EXPECT_EQ(edit.data.poses[0].body.angles, BodyPose().angles);
    EXPECT_TRUE(edit.data.poses[0].grounded);
    EXPECT_EQ(edit.primary, 0);
}

TEST(PoseUiLogic, DeleteMirrorPasteMove){
    PoseAnimationData data = animationOf(4);
    poseui::Edit edit = poseui::deletePoses(data, {1, 2}, 2);
    EXPECT_EQ(names(edit.data), (std::vector<std::string>{"P0", "P3"}));
    EXPECT_EQ(edit.primary, 1);
    edit = poseui::deletePoses(data, {3}, 3);
    EXPECT_EQ(edit.primary, 2);
    edit = poseui::deletePoses(animationOf(1), {0}, 0);
    EXPECT_TRUE(edit.data.poses.empty());
    EXPECT_EQ(edit.primary, -1);

    edit = poseui::mirrorPoses(data, {1}, 1);
    EXPECT_TRUE(edit.changed);
    EXPECT_EQ(edit.data.poses[1].body.angles, mirrorBodyPose(data.poses[1].body).angles);
    EXPECT_EQ(edit.data.poses[1].name, "P1");   // in place: the name stays
    // a neutral pose mirrors to itself: nothing to write
    EXPECT_FALSE(poseui::mirrorPoses(animationOf(1), {0}, 0).changed);

    PoseKey source = data.poses[3];
    source.name = "Other";
    source.hold = 2.0f;
    source.grounded = false;
    source.lift = 0.3f;
    data.poses[0].customTransition = true;
    data.poses[0].transition.duration = 0.5f;
    edit = poseui::pastePose(data, {0}, 0, source);
    EXPECT_EQ(edit.data.poses[0].name, "P0");
    EXPECT_FLOAT_EQ(edit.data.poses[0].hold, 0.1f);
    EXPECT_TRUE(edit.data.poses[0].customTransition);
    EXPECT_FLOAT_EQ(edit.data.poses[0].transition.duration, 0.5f);
    EXPECT_EQ(edit.data.poses[0].body.angles, source.body.angles);
    EXPECT_FALSE(edit.data.poses[0].grounded);
    EXPECT_FLOAT_EQ(edit.data.poses[0].lift, 0.3f);

    edit = poseui::movePoses(data, {1, 2}, 2, poseui::reorderForShift(4, {1, 2}, -1));
    EXPECT_EQ(names(edit.data), (std::vector<std::string>{"P1", "P2", "P0", "P3"}));
    EXPECT_EQ(edit.selection, (std::vector<int>{0, 1}));
    EXPECT_EQ(edit.primary, 1);
}

TEST(PoseUiLogic, ToolsNeutralGroundCenterResetCustom){
    PoseAnimationData data = animationOf(3);
    data.poses[2].body.offset = Vector3(0.2f, 0, -0.1f);
    data.poses[2].grounded = false;
    data.poses[2].lift = 0.4f;
    data.poses[1].customTransition = true;
    data.poses[2].customTransition = true;

    poseui::Edit edit = poseui::resetToNeutral(data, {2}, 2);
    EXPECT_EQ(edit.data.poses[2].body.angles, BodyPose().angles);
    EXPECT_FALSE(edit.data.poses[2].grounded);   // only the body
    edit = poseui::groundPoses(data, {2}, 2);
    EXPECT_TRUE(edit.data.poses[2].grounded);
    EXPECT_FLOAT_EQ(edit.data.poses[2].lift, 0.0f);
    EXPECT_FALSE(poseui::groundPoses(data, {0}, 0).changed);
    edit = poseui::centerPoses(data, {2}, 2);
    EXPECT_FLOAT_EQ(edit.data.poses[2].body.offset.x, 0.0f);
    EXPECT_FLOAT_EQ(edit.data.poses[2].body.offset.z, 0.0f);
    edit = poseui::resetCustomTransitions(data, {0}, 0);
    for (const PoseKey& key : edit.data.poses) EXPECT_FALSE(key.customTransition);
    EXPECT_EQ(edit.selection, (std::vector<int>{0}));
}

// ---- multi-selection values ----

TEST(PoseUiLogic, MixedValuesAndMultiEdit){
    PoseAnimationData data = animationOf(3);
    const int raise = dof("left_upper_arm.raise");
    poseui::MixedValue value = poseui::mixedAngle(data, {0, 1}, raise);
    EXPECT_TRUE(value.any);
    EXPECT_TRUE(value.mixed);
    EXPECT_FLOAT_EQ(value.value, 0.0f);
    value = poseui::mixedAngle(data, {1}, raise);
    EXPECT_FALSE(value.mixed);
    EXPECT_FLOAT_EQ(value.value, 10.0f);
    EXPECT_FALSE(poseui::mixedAngle(data, {}, raise).any);

    // an edit applies to every selected pose
    EXPECT_TRUE(poseui::setAngle(data, {0, 2}, raise, 45.0f));
    EXPECT_FLOAT_EQ(data.poses[0].body.angles[raise], 45.0f);
    EXPECT_FLOAT_EQ(data.poses[1].body.angles[raise], 10.0f);
    EXPECT_FLOAT_EQ(data.poses[2].body.angles[raise], 45.0f);
    EXPECT_FALSE(poseui::mixedAngle(data, {0, 2}, raise).mixed);
    EXPECT_FALSE(poseui::setAngle(data, {0, 2}, raise, 45.0f));   // no change
    EXPECT_TRUE(poseui::setAngle(data, {1}, raise, 190.0f));       // wraps
    EXPECT_FLOAT_EQ(data.poses[1].body.angles[raise], -170.0f);

    value = poseui::mixedValue(data, {0, 1, 2}, [](const PoseKey& key){ return key.hold; });
    EXPECT_FALSE(value.mixed);
    EXPECT_TRUE(poseui::setOnSelection(data, {1, 2}, [](PoseKey& key){ key.hold = 0.3f; }));
    EXPECT_TRUE(poseui::mixedValue(data, {0, 1, 2}, [](const PoseKey& key){ return key.hold; }).mixed);
}


// ---- timing bar ----

TEST(PoseUiLogic, TimingLayoutMatchesTheTimeline){
    PoseAnimationData data = animationOf(3);
    data.poses[0].hold = 0.2f;
    data.poses[1].hold = 0.0f;
    data.poses[1].customTransition = true;
    data.poses[1].transition.duration = 0.3f;
    data.settings.transition.duration = 0.1f;
    data.settings.loop = true;
    poseui::TimingLayout layout = poseui::timingLayout(data);
    const PoseTimeline timeline = buildTimeline(data);
    EXPECT_NEAR(layout.length, timeline.length, 1e-9);
    ASSERT_EQ(layout.holds.size(), 3u);
    ASSERT_EQ(layout.transitions.size(), 3u);   // looping: 2 -> 0 too
    EXPECT_NEAR(layout.holds[0].start, 0.0, 1e-6);
    EXPECT_NEAR(layout.holds[0].end, 0.2, 1e-6);
    EXPECT_NEAR(layout.transitions[0].end, 0.3, 1e-6);
    EXPECT_NEAR(layout.holds[1].start, 0.3, 1e-6);
    EXPECT_NEAR(layout.holds[1].end, 0.3, 1e-6);   // zero hold
    EXPECT_NEAR(layout.transitions[1].end, 0.6, 1e-6);
    EXPECT_TRUE(layout.transitions[1].custom);
    EXPECT_TRUE(layout.transitions[2].loopBack);
    EXPECT_NEAR(layout.transitions[2].end, layout.length, 1e-6);

    data.settings.loop = false;
    layout = poseui::timingLayout(data);
    EXPECT_EQ(layout.transitions.size(), 2u);
    EXPECT_NEAR(layout.holds[2].end, layout.length, 1e-6);
    EXPECT_TRUE(poseui::timingLayout(PoseAnimationData()).holds.empty());
}

TEST(PoseUiLogic, TimingHitTestEdgesAndBodies){
    PoseAnimationData data = animationOf(3);
    data.poses[0].hold = 0.2f;
    data.poses[1].hold = 0.0f;    // pose 1's hold edge sits on the transition 0 -> 1 end
    data.settings.transition.duration = 0.1f;
    data.poses[1].customTransition = true;
    data.poses[1].transition.duration = 0.2f;
    const poseui::TimingLayout layout = poseui::timingLayout(data);
    const double pps = 100.0;   // 1 px = 10 ms

    poseui::TimingHit hit = poseui::timingHitTest(layout, 20.0 + 3.0, pps);   // near pose 0's hold end
    EXPECT_EQ(hit.kind, poseui::TimingHitKind::HoldEdge);
    EXPECT_EQ(hit.pose, 0);
    // x = 30: the end of transition 0 and the (zero) hold of pose 1: the hold edge wins
    hit = poseui::timingHitTest(layout, 30.0, pps);
    EXPECT_EQ(hit.kind, poseui::TimingHitKind::HoldEdge);
    EXPECT_EQ(hit.pose, 1);
    // further than 5 px: bodies
    hit = poseui::timingHitTest(layout, 10.0, pps);
    EXPECT_EQ(hit.kind, poseui::TimingHitKind::Hold);
    EXPECT_EQ(hit.pose, 0);
    hit = poseui::timingHitTest(layout, 40.0, pps);   // in transition 1 -> 2 (0.3 .. 0.5)
    EXPECT_EQ(hit.kind, poseui::TimingHitKind::Transition);
    EXPECT_EQ(hit.pose, 1);
    hit = poseui::timingHitTest(layout, 50.0 + 2.0, pps);   // transition 1 -> 2 ends at 0.5: pose 2's hold starts there
    EXPECT_EQ(hit.kind, poseui::TimingHitKind::TransitionEdge);
    EXPECT_EQ(hit.pose, 1);
    hit = poseui::timingHitTest(layout, 1000.0, pps);
    EXPECT_EQ(hit.kind, poseui::TimingHitKind::None);
    EXPECT_EQ(poseui::poseAtTime(layout, 0.05), 0);
    EXPECT_EQ(poseui::poseAtTime(layout, 0.25), 0);
    EXPECT_EQ(poseui::poseAtTime(layout, 0.35), 1);
    EXPECT_EQ(poseui::poseAtTime(layout, 0.55), 2);
}

TEST(PoseUiLogic, TimingSnapAndDrag){
    PoseAnimationData data = animationOf(2);
    data.settings.frameRate = 30;
    EXPECT_FLOAT_EQ(poseui::timingSnapStep(data.settings), 1.0f / 30.0f);
    data.settings.stepRate = 12;
    EXPECT_FLOAT_EQ(poseui::timingSnapStep(data.settings), 1.0f / 12.0f);
    data.settings.stepRate = 0;

    EXPECT_NEAR(poseui::snapTimingValue(0.11f, 0.1f, true, 0, 5), 0.1f, 1e-6);
    EXPECT_NEAR(poseui::snapTimingValue(0.11f, 0.1f, false, 0, 5), 0.11f, 1e-6);   // Alt
    EXPECT_NEAR(poseui::snapTimingValue(-1.0f, 0.1f, true, 0, 5), 0.0f, 1e-6);
    EXPECT_NEAR(poseui::snapTimingValue(9.0f, 0.1f, true, 0, 5), 5.0f, 1e-5);
    // a duration that rounds below its minimum takes the first grid step above it
    EXPECT_NEAR(poseui::snapTimingValue(0.01f, 1.0f / 30.0f, true, 0.02f, 1.0f), 1.0f / 30.0f, 1e-6);

    poseui::TimingHit hold{poseui::TimingHitKind::HoldEdge, 0};
    float start = poseui::timingEdgeValue(data, hold);
    EXPECT_FLOAT_EQ(start, 0.1f);
    EXPECT_TRUE(poseui::applyTimingDrag(data, hold, start, 0.07f, true));
    EXPECT_NEAR(data.poses[0].hold, 5.0f / 30.0f, 1e-6);
    EXPECT_TRUE(poseui::applyTimingDrag(data, hold, start, -1.0f, true));
    EXPECT_FLOAT_EQ(data.poses[0].hold, 0.0f);

    // dragging a transition end makes it custom, starting from the animation's transition
    data.settings.transition.overshoot = 0.4f;
    poseui::TimingHit transition{poseui::TimingHitKind::TransitionEdge, 1};
    start = poseui::timingEdgeValue(data, transition);
    EXPECT_FLOAT_EQ(start, data.settings.transition.duration);
    EXPECT_FALSE(data.poses[1].customTransition);
    EXPECT_TRUE(poseui::applyTimingDrag(data, transition, start, 0.1f, true));
    EXPECT_TRUE(data.poses[1].customTransition);
    EXPECT_NEAR(data.poses[1].transition.duration, 7.0f / 30.0f, 1e-6);
    EXPECT_FLOAT_EQ(data.poses[1].transition.overshoot, 0.4f);
    EXPECT_FALSE(poseui::applyTimingDrag(data, transition, start, 0.1f, true));   // same value: no change
    EXPECT_TRUE(poseui::applyTimingDrag(data, transition, start, 5.0f, true));
    EXPECT_FLOAT_EQ(data.poses[1].transition.duration, 1.0f);   // clamped to the phase 1 range
}


// ---- Global Edit ----

TEST(PoseUiLogic, JointRotationMatchesTheRig){
    // E from the angles alone gives the rig's own conversion (local = left * E * neutral)
    const posetest::TestRig& t = posetest::aposeRig();
    std::mt19937 random(7);
    std::uniform_real_distribution<float> angle(-60.0f, 60.0f);
    for (int b = 0; b < HUMAN_BONE_COUNT; b++){
        const HumanBone bone = boneAt(b);
        if (!t.rig->has(bone)) continue;
        float angles[3] = {angle(random), angle(random), angle(random)};
        float back[3];
        pose::jointAngles(bone, pose::jointRotation(bone, angles), back);
        float rigBack[3];
        t.rig->anglesFromLocal(bone, t.rig->localFromAngles(bone, angles), rigBack);
        for (int k = 0; k < 3; k++){
            EXPECT_NEAR(back[k], angles[k], 1e-3) << humanBoneName(bone) << " " << k;
            EXPECT_NEAR(back[k], rigBack[k], 1e-3) << humanBoneName(bone) << " " << k;
        }
    }
}

TEST(PoseUiLogic, GlobalEditAppliesTheChange){
    const int raise = dof("left_upper_arm.raise");
    const int bend = dof("spine.bend");
    const int knee = dof("right_lower_leg.bend");
    BodyPose base;
    base.angles[raise] = 10.0f;
    BodyPose current = base;
    current.angles[raise] = 40.0f;       // the edit: the left arm 30 degrees higher
    current.angles[bend] = 0.005f;       // below the 0.01 degree threshold: ignored

    // on the base pose itself the result is the edited pose
    BodyPose same = base;
    std::vector<int> slots;
    EXPECT_EQ(poseui::applyGlobalEditDelta(base, current, same, &slots), 1);
    EXPECT_EQ(slots, (std::vector<int>{boneIndex(HumanBone::LeftUpperArm)}));
    EXPECT_NEAR(same.angles[raise], 40.0f, 1e-3);
    EXPECT_FLOAT_EQ(same.angles[bend], 0.0f);

    // another pose: the arm goes up by the same amount, other slots and the offset stay
    BodyPose other;
    other.angles[raise] = -20.0f;
    other.angles[knee] = 60.0f;
    other.offset = Vector3(0.1f, 0, 0.2f);
    EXPECT_EQ(poseui::applyGlobalEditDelta(base, current, other), 1);
    EXPECT_NEAR(other.angles[raise], 10.0f, 1e-3);
    EXPECT_FLOAT_EQ(other.angles[knee], 60.0f);
    EXPECT_FLOAT_EQ(other.offset.x, 0.1f);
    EXPECT_FLOAT_EQ(other.offset.z, 0.2f);

    // nothing changed: nothing to apply
    BodyPose untouched = other;
    EXPECT_EQ(poseui::applyGlobalEditDelta(base, base, untouched), 0);
    EXPECT_EQ(untouched.angles, other.angles);

    // a rotation about another axis composes in joint-angle space (D * E): the delta of a twist
    // on a pose with a swing is the twist, applied in front
    BodyPose twistBase;
    BodyPose twistCurrent;
    twistCurrent.angles[dof("head.twist")] = 30.0f;
    BodyPose bent;
    bent.angles[dof("head.bend")] = 20.0f;
    poseui::applyGlobalEditDelta(twistBase, twistCurrent, bent);
    const Quatd expected = (pose::jointRotation(HumanBone::Head, &twistCurrent.angles[dofIndex(HumanBone::Head, 0)]) *
                            pose::jointRotation(HumanBone::Head, &BodyPose().angles[0] /* zero */).inverse() *
                            [&]{ BodyPose b; b.angles[dof("head.bend")] = 20.0f; return pose::jointRotation(HumanBone::Head, &b.angles[dofIndex(HumanBone::Head, 0)]); }()).normalized();
    const Quatd got = pose::jointRotation(HumanBone::Head, &bent.angles[dofIndex(HumanBone::Head, 0)]);
    EXPECT_LT(radToDeg(angleBetween(expected, got)), 1e-3);
}

// ---- the session's bone writes during a scene save ----

TEST(PoseUiLogic, BoneWriteGateHoldsThroughAnAsyncSave){
    // Ctrl+S: suspend (bones restored), the scene is SAVING on a thread for a few frames while the
    // user selects a pose, starts posing or previews: nothing may write until it is stopped again
    poseui::BoneWriteGate gate;
    EXPECT_TRUE(gate.mayWrite(true));
    EXPECT_FALSE(gate.mayWrite(false));       // play mode / saving: never
    gate.suspend();                           // stopTransientPreviews before the save
    EXPECT_FALSE(gate.mayWrite(true));        // same frame, scene still STOPPED (sync save): no writes
    EXPECT_FALSE(gate.resume(false));         // update while SAVING: stays closed
    EXPECT_FALSE(gate.mayWrite(false));
    gate.suspend();                           // another suspend while saving (update sees SAVING)
    EXPECT_FALSE(gate.resume(false));
    EXPECT_TRUE(gate.resume(true));           // saved: the next update re-applies the pose once
    EXPECT_TRUE(gate.mayWrite(true));
    EXPECT_FALSE(gate.resume(true));          // nothing more to resume
}

TEST(PoseUiLogic, GlobalEditSurvivesEditsOfThePoses){
    // angles, offset, hold, transition, feel, humanize, mirror, paste: same poses, same primary
    EXPECT_TRUE(poseui::keepsGlobalEdit(4, 4, false, 2, 2));
    // add / duplicate / delete change the count, reorders and another primary cancel it
    EXPECT_FALSE(poseui::keepsGlobalEdit(4, 5, false, 2, 3));
    EXPECT_FALSE(poseui::keepsGlobalEdit(4, 3, false, 2, 2));
    EXPECT_FALSE(poseui::keepsGlobalEdit(4, 4, true, 2, 2));
    EXPECT_FALSE(poseui::keepsGlobalEdit(4, 4, false, 2, 1));
    // the edits behind the menu keep the primary pose and the count
    PoseAnimationData data = animationOf(4);
    const poseui::Edit mirrored = poseui::mirrorPoses(data, {2}, 2);
    EXPECT_TRUE(poseui::keepsGlobalEdit(data.poses.size(), mirrored.data.poses.size(), false, 2, mirrored.primary));
    const poseui::Edit deleted = poseui::deletePoses(data, {1}, 2);
    EXPECT_FALSE(poseui::keepsGlobalEdit(data.poses.size(), deleted.data.poses.size(), false, 2, deleted.primary));
}

// ---- Templates tab (phase 6, section 4) ----

TEST(PoseUiLogic, TemplateCategoriesAreCaseInsensitiveAndUnique){
    std::vector<editor::PoseTemplateInfo> list(5);
    list[0].category = "Basics";
    list[1].category = "Locomotion";
    list[2].category = "locomotion";   // the same category in another case
    list[3].category = "";
    list[4].category = "User";
    EXPECT_EQ(poseui::templateCategories(list), (std::vector<std::string>{"Basics", "Locomotion", "User"}));
}

TEST(PoseUiLogic, TemplateFilter){
    editor::PoseTemplateInfo walk;
    walk.name = "Cartoon Walk";
    walk.category = "Locomotion";
    walk.description = "A bouncy walk with big up and down moves.";
    EXPECT_TRUE(poseui::templateMatches(walk, "", ""));
    EXPECT_TRUE(poseui::templateMatches(walk, "locomotion", ""));
    EXPECT_FALSE(poseui::templateMatches(walk, "Actions", ""));
    EXPECT_TRUE(poseui::templateMatches(walk, "", "WALK"));
    EXPECT_TRUE(poseui::templateMatches(walk, "", "  bouncy   cartoon "));   // every word, anywhere
    EXPECT_TRUE(poseui::templateMatches(walk, "", "locom"));                 // the category too
    EXPECT_FALSE(poseui::templateMatches(walk, "", "walk jump"));
    EXPECT_FALSE(poseui::templateMatches(walk, "Locomotion", "run"));
}

TEST(PoseUiLogic, TemplateAnimationPath){
    std::vector<std::string> taken;
    auto exists = [&taken](const std::string& path){ return std::find(taken.begin(), taken.end(), path) != taken.end(); };
    EXPECT_EQ(poseui::templateAnimationPath("assets/models", "Walk", exists), "assets/models/Walk.poseanim");
    EXPECT_EQ(poseui::templateAnimationPath("assets/models/", "Walk", exists), "assets/models/Walk.poseanim");
    EXPECT_EQ(poseui::templateAnimationPath("", "Walk", exists), "Walk.poseanim");
    EXPECT_EQ(poseui::templateAnimationPath(".", "Walk", exists), "Walk.poseanim");
    // characters unsafe in file names (a user template may have any name)
    EXPECT_EQ(poseui::templateAnimationPath("assets", "Kick: L/R?", exists), "assets/Kick_ L_R_.poseanim");
    taken = {"assets/models/Walk.poseanim", "assets/models/Walk 1.poseanim"};
    EXPECT_EQ(poseui::templateAnimationPath("assets/models", "Walk", exists), "assets/models/Walk 2.poseanim");
}

TEST(PoseUiLogic, ApplyTemplateReplacesOrAppends){
    PoseAnimationData current = animationOf(3);
    current.previewModel = "models/hero.gltf";
    current.settings.frameRate = 24;
    PoseAnimationData walk = animationOf(4);
    for (PoseKey& key : walk.poses) key.name = "W" + key.name;
    walk.settings.frameRate = 60;
    walk.settings.loop = true;

    const poseui::Edit replaced = poseui::applyTemplate(current, walk, editor::PoseTemplateApply::Replace, {1, 2}, 2);
    EXPECT_TRUE(replaced.changed);
    EXPECT_EQ(names(replaced.data), (std::vector<std::string>{"WP0", "WP1", "WP2", "WP3"}));
    EXPECT_EQ(replaced.data.settings.frameRate, 60);                 // the template's settings
    EXPECT_TRUE(replaced.data.settings.loop);
    EXPECT_EQ(replaced.data.previewModel, "models/hero.gltf");      // the asset keeps its preview model
    EXPECT_EQ(replaced.selection, std::vector<int>{0});
    EXPECT_EQ(replaced.primary, 0);

    const poseui::Edit appended = poseui::applyTemplate(current, walk, editor::PoseTemplateApply::Append, {1}, 1);
    EXPECT_TRUE(appended.changed);
    EXPECT_EQ(names(appended.data), (std::vector<std::string>{"P0", "P1", "P2", "WP0", "WP1", "WP2", "WP3"}));
    EXPECT_EQ(appended.data.settings.frameRate, 24);                 // the animation's settings stay
    EXPECT_EQ(appended.selection, (std::vector<int>{3, 4, 5, 6}));
    EXPECT_EQ(appended.primary, 3);

    const poseui::Edit nothing = poseui::applyTemplate(current, PoseAnimationData(), editor::PoseTemplateApply::Append, {1}, 1);
    EXPECT_FALSE(nothing.changed);
    EXPECT_EQ(nothing.primary, 1);
}
